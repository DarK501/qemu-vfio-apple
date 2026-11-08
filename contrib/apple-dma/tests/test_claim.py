#!/usr/bin/env python3
"""Exercise DriverKit device claim ownership and concurrent contenders."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1,
       kIOReturnUnsupported = 2, kIOReturnExclusiveAccess = 3,
       kIOReturnError = 4, kIOReturnNotReady = 5,
       kIOReturnNotAttached = 6, kIOReturnNotOpen = 7 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
struct IOService { virtual ~IOService() = default; };
struct VFIOUserPCIDriverUserClient;
struct IOPCIDevice {
    std::atomic<int> opens {0}, closes {0};
    std::atomic<bool> block_open {false}, in_open {false}, open_continue {false};
    std::atomic<bool> block_close {false}, in_close {false}, close_continue {false};
    bool fail_open = false;
    int Open(VFIOUserPCIDriverUserClient *, int) {
        opens++;
        if (block_open) {
            in_open = true;
            while (!open_continue) { std::this_thread::yield(); }
        }
        return fail_open ? kIOReturnError : kIOReturnSuccess;
    }
    void Close(VFIOUserPCIDriverUserClient *, int) {
        closes++;
        if (block_close) {
            in_close = true;
            while (!close_continue) { std::this_thread::yield(); }
        }
    }
    void ConfigurationRead16(int, uint16_t *value) { *value = 0; }
    void ConfigurationWrite16(int, uint16_t) {}
};
struct VFIOUserPCIDriver_IVars {
    VFIOUserPCIDriverUserClient *openerClient = nullptr;
    bool claimBlocked = false;
};
struct VFIOUserPCIDriver : IOService {
    VFIOUserPCIDriver_IVars *ivars;
    IOPCIDevice *device;
};
struct ClientVars { bool claimed = false; };
struct VFIOUserPCIDriverUserClient : IOService {
    ClientVars *ivars;
    IOService *provider;
    IOService *GetProvider() { return provider; }
};
struct VFIOUserPCIDeviceIdentity { uint8_t bus, device, function; };
static IOPCIDevice *vfio_user_get_pci_device(IOService *service)
{
    return static_cast<VFIOUserPCIDriver *>(service)->device;
}
static void vfio_user_dma_log(const char *, ...) {}
"""

SUFFIX = r"""
int main()
{
    VFIOUserPCIDriver_IVars driver_vars;
    IOPCIDevice pci;
    VFIOUserPCIDriver driver;
    driver.ivars = &driver_vars;
    driver.device = &pci;
    ClientVars vars1, vars2;
    VFIOUserPCIDriverUserClient c1, c2;
    c1.ivars = &vars1; c1.provider = &driver;
    c2.ivars = &vars2; c2.provider = &driver;
    VFIOUserPCIDeviceIdentity identity = {};

    pci.fail_open = true;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnError);
    assert(!driver_vars.openerClient && !vars1.claimed);
    pci.fail_open = false;
    pci.block_open = true;
    int first_result = -1;
    std::thread first([&] { first_result = vfio_user_claim_device(&c1, &driver, &identity); });
    while (!pci.in_open) { std::this_thread::yield(); }
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnExclusiveAccess);
    pci.open_continue = true;
    first.join();
    assert(first_result == kIOReturnSuccess && vfio_user_service_claimed(&c1));
    assert(!vfio_user_service_claimed(&c2));
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnSuccess);
    assert(pci.opens == 2);
    assert(vfio_user_release_claim(&c2) == kIOReturnSuccess);
    assert(pci.closes == 0 && vfio_user_service_claimed(&c1));

    pci.block_close = true;
    int close_result = -1;
    std::thread closing([&] { close_result = vfio_user_release_claim(&c1); });
    while (!pci.in_close) { std::this_thread::yield(); }
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnExclusiveAccess);
    pci.close_continue = true;
    closing.join();
    assert(close_result == kIOReturnSuccess);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 1);
    pci.block_open = false;
    pci.block_close = false;
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnSuccess);
    assert(vfio_user_release_claim(&c2) == kIOReturnSuccess);
    assert(!driver_vars.openerClient && pci.opens == 3 && pci.closes == 2);
    driver_vars.claimBlocked = true;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnNotReady);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.opens == 3);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static bool\nvfio_user_service_claimed(")
    end = source.index("\nstatic kern_return_t\nvfio_user_release_dma_buffer(",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "claim.cpp"
        binary = pathlib.Path(directory) / "claim"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-pthread", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()
