#!/usr/bin/env python3
"""Exercise DriverKit PCI identity discovery without opening the device."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1,
       kIOReturnUnsupported = 2, kIOReturnNotFound = 3 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSSafeReleaseNULL(object) do { if (object) object->release(); object = nullptr; } while (0)
struct OSContainer {
    virtual ~OSContainer() = default;
    void release() {}
};
struct OSData : OSContainer {
    std::vector<uint8_t> bytes;
    OSData(std::initializer_list<uint8_t> value) : bytes(value) {}
    size_t getLength() const { return bytes.size(); }
    const void *getBytesNoCopy() const { return bytes.empty() ? nullptr : bytes.data(); }
};
struct OSNumber : OSContainer {
    uint64_t value;
    explicit OSNumber(uint64_t n) : value(n) {}
    uint64_t unsigned64BitValue() const { return value; }
    uint32_t unsigned32BitValue() const { return static_cast<uint32_t>(value); }
};
struct IOService { virtual ~IOService() = default; };
struct IOPCIDevice {
    std::map<std::string, OSContainer *> props;
    kern_return_t GetBusDeviceFunction(uint8_t *bus, uint8_t *dev, uint8_t *fn) {
        *bus = 0x44; *dev = 0; *fn = 0; return kIOReturnSuccess;
    }
    kern_return_t SearchProperty(const char *name, const char *, uint64_t,
                                 OSContainer **out) {
        auto it = props.find(name);
        if (it == props.end()) return kIOReturnNotFound;
        *out = it->second; return kIOReturnSuccess;
    }
};
struct VFIOUserPCIDeviceIdentity {
    uint8_t bus, device, function;
    uint16_t vendorID, deviceID;
    uint32_t classCode;
};
struct Service : IOService { IOPCIDevice *device; };
static IOPCIDevice *vfio_user_get_pci_device(IOService *service) {
    return static_cast<Service *>(service)->device;
}
"""

SUFFIX = r"""
int main() {
    OSData vendor {0x02, 0x10, 0, 0};
    OSNumber device(0x731f);
    OSData cls {0, 0, 3, 0};
    IOPCIDevice pci;
    pci.props = {{"vendor-id", &vendor}, {"device-id", &device},
                 {"class-code", &cls}};
    Service service; service.device = &pci;
    VFIOUserPCIDeviceIdentity id;
    assert(vfio_user_read_identity(&service, &id) == kIOReturnSuccess);
    assert(id.bus == 0x44 && id.device == 0 && id.function == 0);
    assert(id.vendorID == 0x1002 && id.deviceID == 0x731f && id.classCode == 0x030000);
    pci.props.erase("class-code");
    assert(vfio_user_read_identity(&service, &id) == kIOReturnNotFound);
    OSData short_data {0x02, 0x10};
    pci.props["class-code"] = &cls; pci.props["vendor-id"] = &short_data;
    assert(vfio_user_read_identity(&service, &id) == kIOReturnBadArgument);
    OSNumber invalid(0xffff);
    pci.props["vendor-id"] = &invalid;
    assert(vfio_user_read_identity(&service, &id) == kIOReturnBadArgument);
    OSNumber too_large(0x100000000ULL);
    pci.props["vendor-id"] = &too_large;
    assert(vfio_user_read_identity(&service, &id) == kIOReturnBadArgument);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static kern_return_t\nvfio_user_registry_u32(")
    end = source.index("\nstatic bool\nvfio_user_service_claimed(", start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "identity.cpp"
        binary = pathlib.Path(directory) / "identity"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
