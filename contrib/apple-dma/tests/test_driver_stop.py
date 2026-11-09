#!/usr/bin/env python3
"""Verify service Stop preserves state while an owner claim is live."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnStillOpen = 1 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
#define SUPERDISPATCH 9
static int deleted, super_stops;
struct VFIOUserPCIDriverUserClient {};
struct IOService {};
struct VFIOUserPCIDriver_IVars {
    VFIOUserPCIDriverUserClient *openerClient = nullptr;
    bool startOpen = false;
    bool claimBlocked = false;
};
struct IOPCIDevice { int closes = 0; void Close(IOService *, int) { closes++; } };
struct VFIOUserPCIDriver;
static IOPCIDevice g_pci;
static bool provider_available = true;
static IOPCIDevice *vfio_user_get_pci_device(VFIOUserPCIDriver *) {
    return provider_available ? &g_pci : nullptr;
}
static std::vector<std::string> logs;
static void vfio_user_dma_log(const char *fmt, ...) {
    char message[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    logs.emplace_back(message);
}
struct VFIOUserPCIDriver : IOService {
    VFIOUserPCIDriver_IVars *ivars;
    explicit VFIOUserPCIDriver(VFIOUserPCIDriver_IVars *v) : ivars(v) {}
    kern_return_t Stop(IOService *);
};
static int super_stop(IOService *, int) { super_stops++; return 0; }
#define IOSafeDeleteNULL(ptr, type, count) do { \
    (void)sizeof(type); (void)(count); deleted++; (ptr) = nullptr; \
} while (0)
"""

SUFFIX = r"""
int main()
{
    VFIOUserPCIDriver_IVars vars;
    VFIOUserPCIDriverUserClient owner;
    vars.openerClient = &owner;
    vars.claimBlocked = true;
    VFIOUserPCIDriver driver(&vars);
    IOService provider;
    assert(driver.Stop(&provider) == kIOReturnStillOpen);
    assert(driver.ivars == &vars && vars.openerClient == &owner);
    assert(!deleted && !super_stops);
    assert(logs[0].find("start_open=0 owner_present=1 claim_blocked=1") != std::string::npos);
    assert(logs[1].find("refused: owner present") != std::string::npos);
    vars.openerClient = nullptr;
    assert(driver.Stop(&provider) == kIOReturnSuccess);
    assert(!driver.ivars && deleted == 1 && super_stops == 1);
    assert(g_pci.closes == 0);
    VFIOUserPCIDriver_IVars held;
    held.startOpen = true;
    VFIOUserPCIDriver driver2(&held);
    assert(driver2.Stop(&provider) == kIOReturnSuccess);
    assert(g_pci.closes == 1);
    bool saw_close = false, saw_return = false, saw_super = false;
    for (const auto &line : logs) {
        saw_close |= line.find("closing Start-time PCI Open") != std::string::npos;
        saw_return |= line.find("Start-time PCI Close returned") != std::string::npos;
        saw_super |= line.find("service super Stop returned") != std::string::npos;
    }
    assert(saw_close && saw_return && saw_super);
    VFIOUserPCIDriver_IVars missing;
    missing.startOpen = true;
    VFIOUserPCIDriver driver3(&missing);
    provider_available = false;
    assert(driver3.Stop(&provider) == kIOReturnSuccess);
    bool saw_missing = false;
    for (const auto &line : logs) {
        saw_missing |= line.find("provider unavailable") != std::string::npos;
    }
    assert(saw_missing && g_pci.closes == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\nIMPL(VFIOUserPCIDriver, Stop)")
    end = source.index("\nkern_return_t\nIMPL(VFIOUserPCIDriverUserClient, Start)",
                       start)
    body = source[start:end].replace(
        "kern_return_t\nIMPL(VFIOUserPCIDriver, Stop)",
        "kern_return_t VFIOUserPCIDriver::Stop(IOService *provider)", 1)
    body = body.replace("Stop(provider, SUPERDISPATCH)",
                        "super_stop(provider, SUPERDISPATCH)")
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "driver_stop.cpp"
        binary = pathlib.Path(directory) / "driver_stop"
        test.write_text(PREFIX + body + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
