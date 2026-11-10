#!/usr/bin/env python3
"""Exercise reset dispatch and the DriverKit function-reset policy."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
DEXT = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"
QEMU = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <cassert>
#include <cerrno>
using kern_return_t = int;
using io_connect_t = int;
enum { KERN_SUCCESS = 0, IO_OBJECT_NULL = 0,
       kIOReturnSuccess = 0, kIOReturnBadArgument = 1,
       kIOReturnNotOpen = 2, kIOReturnNotAttached = 3,
       kIOReturnUnsupported = 4,
       kIOPCIDeviceResetTypeFunctionReset = 10,
       kIOPCIDeviceResetOptionNone = 0 };
static int info_count, warn_count, logs, calls, function_ret;
#define IOLog(...) (++logs)
#define info_report(...) (++info_count)
#define warn_report(...) (++warn_count)
struct IOService {};
struct IOPCIDevice {
    kern_return_t Reset(int type, int option) {
        assert(option == kIOPCIDeviceResetOptionNone);
        ++calls;
        assert(type == kIOPCIDeviceResetTypeFunctionReset);
        return function_ret;
    }
};
struct VFIOUserPCIDriverUserClient {
    void *ivars;
    IOService *provider;
    IOService *GetProvider() { return provider; }
};
static bool claimed = true, pci_available = true;
static IOPCIDevice pci;
static bool vfio_user_service_claimed(VFIOUserPCIDriverUserClient *) {
    return claimed;
}
static IOPCIDevice *vfio_user_get_pci_device(IOService *) {
    return pci_available ? &pci : nullptr;
}
struct VFIODevice { const char *name; io_connect_t conn; };
static io_connect_t apple_vfio_connection(VFIODevice *dev) { return dev->conn; }
static int backend_ret, backend_calls;
static kern_return_t backend_reset(io_connect_t conn) {
    assert(conn == 1);
    ++backend_calls;
    return backend_ret;
}
struct Client { kern_return_t (*reset_device)(io_connect_t); };
struct Backend { const Client *client; };
static Client client_ops = { backend_reset };
static Backend backend = { &client_ops };
static const Backend *apple_host_backend() { return &backend; }
"""

SUFFIX = r"""
int main() {
    IOService provider;
    VFIOUserPCIDriverUserClient client = { &pci, &provider };
    assert(vfio_user_reset_device(nullptr) == kIOReturnBadArgument);
    claimed = false;
    assert(vfio_user_reset_device(&client) == kIOReturnNotOpen);
    claimed = true;
    client.provider = nullptr;
    assert(vfio_user_reset_device(&client) == kIOReturnNotAttached);
    client.provider = &provider;
    pci_available = false;
    assert(vfio_user_reset_device(&client) == kIOReturnUnsupported);
    pci_available = true;
    function_ret = 0; calls = logs = 0;
    assert(vfio_user_reset_device(&client) == 0 && calls == 1 && logs == 2);
    function_ret = 7; calls = logs = 0;
    assert(vfio_user_reset_device(&client) == 7 && calls == 1 && logs == 3);

    VFIODevice dev = { "gpu", 0 };
    assert(apple_vfio_device_reset(&dev) == -ENODEV);
    assert(backend_calls == 0 && warn_count == 1);
    dev.conn = 1; backend_ret = 9;
    assert(apple_vfio_device_reset(&dev) == -EIO);
    assert(backend_calls == 1 && info_count == 1 && warn_count == 2);
    backend_ret = 0;
    assert(apple_vfio_device_reset(&dev) == 0);
    assert(backend_calls == 2 && info_count == 3 && warn_count == 2);
}
"""


def main():
    dext = DEXT.read_text()
    start = dext.index("static kern_return_t\nvfio_user_reset_device(")
    end = dext.index("\nstatic kern_return_t\nvfio_user_setup_interrupts(", start)
    qemu = QEMU.read_text()
    qstart = qemu.index("static int apple_vfio_device_reset(")
    qend = qemu.index("\nstatic ", qstart + 1)
    if "requesting hot reset" in dext[start:end]:
        raise AssertionError("DriverKit reset must not escalate to hot reset")
    with tempfile.TemporaryDirectory() as directory:
        source = pathlib.Path(directory) / "reset.cpp"
        binary = pathlib.Path(directory) / "reset"
        source.write_text(PREFIX + dext[start:end] + qemu[qstart:qend] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
