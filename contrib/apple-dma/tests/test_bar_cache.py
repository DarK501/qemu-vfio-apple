#!/usr/bin/env python3
"""Exercise DriverKit BAR descriptor caching across distinct services."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNotReady = 2,
       kIOReturnNotAttached = 3, kIOReturnUnsupported = 4,
       kIOReturnNoMemory = 5, kIOReturnNotOpen = 6,
       kVFIOUserPCIDriverUserClientMemoryTypeIRQState = 1,
       kVFIOUserPCIDriverUserClientMemoryTypeDMABuffer = 2,
       kVFIOUserPCIDriverUserClientMemoryTypeBAR0 = 10,
       kVFIOUserPCIDriverUserClientMemoryTypeBAR5 = 15 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
struct IOMemoryDescriptor {
    int refs = 1;
    void retain() { refs++; }
    void release() { assert(refs > 0); refs--; }
};
struct IOService { virtual ~IOService() = default; };
struct IOPCIDevice {
    IOMemoryDescriptor *bar;
    int copies = 0;
    int GetBARInfo(uint8_t idx, uint8_t *memoryIndex, uint64_t *size,
                   uint8_t *barType) {
        *memoryIndex = idx;
        *size = 4096;
        *barType = 0;
        return kIOReturnSuccess;
    }
    int _CopyDeviceMemoryWithIndex(uint8_t, IOMemoryDescriptor **memory,
                                   IOService *) {
        copies++;
        bar->retain();
        *memory = bar;
        return kIOReturnSuccess;
    }
};
struct VFIOUserPCIDriver_IVars {
    IOService *openerClient = nullptr;
};
struct VFIOUserPCIDriver : IOService {
    VFIOUserPCIDriver_IVars *ivars;
    IOPCIDevice *device;
};
static IOPCIDevice *vfio_user_get_pci_device(IOService *service)
{
    return static_cast<VFIOUserPCIDriver *>(service)->device;
}
struct ClientIvars {
    bool claimed = false;
    IOMemoryDescriptor *barDescCache[6] = {};
    IOMemoryDescriptor *irqSharedBuffer = nullptr;
    IOMemoryDescriptor *dmaBuffer = nullptr;
};
struct Client : IOService {
    ClientIvars *ivars;
    IOService *provider;
    IOService *GetProvider() { return provider; }
    int copy(uint64_t type, uint64_t *options, IOMemoryDescriptor **memory);
};
static bool vfio_user_service_claimed(Client *client)
{
    return client->ivars->claimed &&
           static_cast<VFIOUserPCIDriver *>(client->provider)->ivars->openerClient ==
           client;
}
"""

SUFFIX = r"""
int main()
{
    IOMemoryDescriptor a, b;
    IOPCIDevice pci_a { &a }, pci_b { &b };
    VFIOUserPCIDriver_IVars vars_a, vars_b;
    VFIOUserPCIDriver driver_a, driver_b;
    driver_a.ivars = &vars_a; driver_a.device = &pci_a;
    driver_b.ivars = &vars_b; driver_b.device = &pci_b;
    ClientIvars primary_a_vars, primary_b_vars, secondary_vars;
    primary_a_vars.claimed = primary_b_vars.claimed = true;
    Client primary_a, primary_b, secondary_a, secondary_b;
    primary_a.ivars = &primary_a_vars; primary_a.provider = &driver_a;
    primary_b.ivars = &primary_b_vars; primary_b.provider = &driver_b;
    secondary_a.ivars = &secondary_vars; secondary_a.provider = &driver_a;
    secondary_b.ivars = &secondary_vars; secondary_b.provider = &driver_b;
    vars_a.openerClient = &primary_a;
    vars_b.openerClient = &primary_b;
    uint64_t options;
    IOMemoryDescriptor *result = nullptr;
    assert(primary_a.copy(10, &options, &result) == kIOReturnSuccess);
    assert(result == &a && pci_a.copies == 1);
    result->release();
    assert(primary_b.copy(10, &options, &result) == kIOReturnSuccess);
    assert(result == &b && pci_b.copies == 1);
    result->release();
    assert(secondary_a.copy(10, &options, &result) == kIOReturnNotOpen);
    assert(secondary_b.copy(10, &options, &result) == kIOReturnNotOpen);
    assert(primary_a.copy(10, &options, &result) == kIOReturnSuccess);
    assert(result == &a && pci_a.copies == 1);
    result->release();
    assert(primary_b.copy(10, &options, &result) == kIOReturnSuccess);
    assert(result == &b && pci_b.copies == 1);
    result->release();
    primary_a_vars.barDescCache[0]->release();
    primary_a_vars.barDescCache[0] = nullptr;
    assert(primary_b.copy(10, &options, &result) == kIOReturnSuccess);
    assert(result == &b && pci_b.copies == 1);
    result->release();
    primary_b_vars.barDescCache[0]->release();
    assert(a.refs == 1 && b.refs == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\nIMPL(VFIOUserPCIDriverUserClient, "
                         "CopyClientMemoryForType)")
    body = source[start:]
    body = body.replace(
        "kern_return_t\nIMPL(VFIOUserPCIDriverUserClient, CopyClientMemoryForType)",
        "int Client::copy(uint64_t type, uint64_t *options, "
        "IOMemoryDescriptor **memory)", 1)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "bar_cache.cpp"
        binary = pathlib.Path(directory) / "bar_cache"
        test.write_text(PREFIX + body + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
