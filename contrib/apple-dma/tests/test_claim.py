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
enum { kIOPCIConfigurationOffsetVendorID = 0, kIOPCIConfigurationOffsetDeviceID = 2,
       kIOPCIConfigurationOffsetCommand = 4 };
struct IOService { virtual ~IOService() = default; };
struct VFIOUserPCIDriverUserClient;
struct IOPCIDevice {
    std::atomic<int> opens {0}, closes {0};
    std::atomic<bool> block_open {false}, in_open {false}, open_continue {false};
    std::atomic<bool> block_close {false}, in_close {false}, close_continue {false};
    bool fail_open = false, ignore_writes = false;
    uint16_t vendor = 0x1002, device = 0x731f, command = 0;
    int writes = 0;
    int Open(IOService *, int) {
        opens++;
        if (block_open) {
            in_open = true;
            while (!open_continue) { std::this_thread::yield(); }
        }
        return fail_open ? kIOReturnError : kIOReturnSuccess;
    }
    void Close(IOService *, int) {
        closes++;
        if (block_close) {
            in_close = true;
            while (!close_continue) { std::this_thread::yield(); }
        }
    }
    void ConfigurationRead16(int offset, uint16_t *value) {
        *value = offset == 0 ? vendor : offset == 2 ? device : command;
    }
    void ConfigurationWrite16(int, uint16_t value) {
        writes++; if (!ignore_writes) command = value;
    }
};
struct VFIOUserPCIDriver_IVars {
    VFIOUserPCIDriverUserClient *openerClient = nullptr;
    bool claimBlocked = false;
    bool startOpen = false;
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
struct VFIOUserPCIDeviceIdentity {
    uint8_t bus, device, function;
    uint16_t vendorID, deviceID;
};
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
    identity.vendorID = 0x1002; identity.deviceID = 0x731f;

    uint64_t selected[5] = {0, 0, 0, 0x1002, 0x731f};
    assert(vfio_user_identity_matches_input(&identity, selected));
    for (int i = 0; i < 5; i++) {
        selected[i] ^= 1;
        assert(!vfio_user_identity_matches_input(&identity, selected));
        selected[i] ^= 1;
    }

    uint64_t probe[3] = {};
    assert(vfio_user_probe_pci_config(&c1, &driver, &identity, probe) == kIOReturnSuccess);
    assert(probe[0] == 0x1002 && probe[1] == 0x731f && probe[2] == 0);
    assert(pci.writes == 0 && pci.closes == 1 && !driver_vars.openerClient);
    pci.vendor = 0xffff;
    assert(vfio_user_probe_pci_config(&c1, &driver, &identity, probe) == kIOReturnNotReady);
    assert(pci.writes == 0 && pci.closes == 2 && !driver_vars.openerClient);
    pci.vendor = 0x1002;
    driver_vars.claimBlocked = true;
    assert(vfio_user_probe_pci_config(&c1, &driver, &identity, probe) == kIOReturnNotReady);
    assert(pci.opens == 2 && !driver_vars.openerClient);
    driver_vars.claimBlocked = false;
    pci.fail_open = true;
    assert(vfio_user_probe_pci_config(&c1, &driver, &identity, probe) == kIOReturnError);
    assert(pci.closes == 3 && !driver_vars.openerClient && pci.writes == 0);
    pci.fail_open = false;
    pci.block_open = true;
    int probe_result = -1;
    std::thread probing([&] {
        probe_result = vfio_user_probe_pci_config(&c1, &driver, &identity, probe);
    });
    while (!pci.in_open) { std::this_thread::yield(); }
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnExclusiveAccess);
    pci.open_continue = true;
    probing.join();
    assert(probe_result == kIOReturnSuccess && !driver_vars.openerClient);
    pci.block_open = false; pci.in_open = false; pci.open_continue = false;
    pci.opens = 0; pci.closes = 0;

    pci.fail_open = true;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnError);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 1);
    pci.fail_open = false;
    pci.vendor = 0xffff;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnNotReady);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 2 && pci.writes == 0);
    pci.vendor = 0x1002;
    pci.device = 0x1234;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnNotReady);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 3 && pci.writes == 0);
    pci.device = 0x731f;
    pci.command = 0xffff;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnNotReady);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 4 && pci.writes == 0);
    pci.command = 0;
    pci.block_open = true;
    int first_result = -1;
    std::thread first([&] { first_result = vfio_user_claim_device(&c1, &driver, &identity); });
    while (!pci.in_open) { std::this_thread::yield(); }
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnExclusiveAccess);
    assert(vfio_user_probe_pci_config(&c2, &driver, &identity, probe) == kIOReturnExclusiveAccess);
    pci.open_continue = true;
    first.join();
    assert(first_result == kIOReturnSuccess && vfio_user_service_claimed(&c1));
    assert(!vfio_user_service_claimed(&c2));
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnSuccess);
    assert(pci.opens == 5);
    assert(vfio_user_release_claim(&c2) == kIOReturnSuccess);
    assert(pci.closes == 4 && vfio_user_service_claimed(&c1));

    pci.block_close = true;
    int close_result = -1;
    std::thread closing([&] { close_result = vfio_user_release_claim(&c1); });
    while (!pci.in_close) { std::this_thread::yield(); }
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnExclusiveAccess);
    pci.close_continue = true;
    closing.join();
    assert(close_result == kIOReturnSuccess);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 5);
    pci.block_open = false;
    pci.block_close = false;
    assert(vfio_user_claim_device(&c2, &driver, &identity) == kIOReturnSuccess);
    assert(vfio_user_release_claim(&c2) == kIOReturnSuccess);
    assert(!driver_vars.openerClient && pci.opens == 6 && pci.closes == 6);
    driver_vars.claimBlocked = true;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnNotReady);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.opens == 6);
    driver_vars.claimBlocked = false;
    pci.command = 0;
    pci.ignore_writes = true;
    assert(vfio_user_claim_device(&c1, &driver, &identity) == kIOReturnNotReady);
    assert(!driver_vars.openerClient && !vars1.claimed && pci.closes == 7);
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
