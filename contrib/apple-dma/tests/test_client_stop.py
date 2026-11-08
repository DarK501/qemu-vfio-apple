#!/usr/bin/env python3
"""Verify failed dext DMA cleanup refuses to stop or release client state."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnStillOpen = 1 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
#define SUPERDISPATCH 9
static int releases, deleted, super_stops, cleanup_regions, cleanup_irqs;
static int buffer_cleanup_result;
static int claim_release_result;
static int irq_cleanup_result;
struct Object { void release() { releases++; } };
struct IOService { virtual ~IOService() = default; };
struct VFIOUserPCIDriverUserClient;
struct DriverIvars {
    VFIOUserPCIDriverUserClient *openerClient = nullptr;
    bool claimBlocked = false;
};
struct VFIOUserPCIDriver : IOService { DriverIvars *ivars; };
struct ClientIvars {
    uint64_t activeDMARegionCount = 0, activeDMAChunkCount = 0;
    uint64_t activeDMABytes = 0, peakDMARegionCount = 0;
    uint64_t peakDMAChunkCount = 0, peakDMABytes = 0;
    uint64_t dmaCompleteFailureCount = 0;
    Object *barDescCache[6] = {};
    bool claimed = false;
};
using VFIOUserPCIDriverUserClient_IVars = ClientIvars;
struct VFIOUserPCIDriverUserClient : IOService {
    ClientIvars *ivars;
    IOService *provider;
    IOService *GetProvider() { return provider; }
    kern_return_t Stop(IOService *);
};
static int super_stop(IOService *, int) { super_stops++; return 0; }
static double vfio_user_dma_bytes_to_mb(uint64_t bytes)
{
    return static_cast<double>(bytes);
}
static void vfio_user_dma_log(const char *, ...) {}
static int vfio_user_release_all_interrupts(VFIOUserPCIDriverUserClient *)
{
    cleanup_irqs++;
    return irq_cleanup_result;
}
static void vfio_user_release_all_client_dma_regions(
    VFIOUserPCIDriverUserClient *)
{
    cleanup_regions++;
}
static int vfio_user_release_dma_buffer(VFIOUserPCIDriverUserClient *)
{
    return buffer_cleanup_result;
}
static int vfio_user_release_claim(VFIOUserPCIDriverUserClient *client)
{
    if (claim_release_result != kIOReturnSuccess) {
        return claim_release_result;
    }
    client->ivars->claimed = false;
    DriverIvars *driver =
        static_cast<VFIOUserPCIDriver *>(client->provider)->ivars;
    driver->claimBlocked = false;
    driver->openerClient = nullptr;
    releases++;
    return kIOReturnSuccess;
}
#define OSSafeReleaseNULL(ptr) do { \
    if (ptr) { (ptr)->release(); (ptr) = nullptr; } \
} while (0)
#define IOSafeDeleteNULL(ptr, type, count) do { \
    (void)sizeof(type); (void)(count); deleted++; (ptr) = nullptr; \
} while (0)
"""

SUFFIX = r"""
int main()
{
    DriverIvars driver_vars;
    VFIOUserPCIDriver driver;
    driver.ivars = &driver_vars;
    ClientIvars client_vars;
    VFIOUserPCIDriverUserClient client;
    client.ivars = &client_vars;
    client.provider = &driver;
    client_vars.claimed = true;
    driver_vars.openerClient = &client;
    Object bar;
    client_vars.barDescCache[0] = &bar;

    irq_cleanup_result = kIOReturnStillOpen;
    assert(client.Stop(&driver) == kIOReturnStillOpen);
    assert(client.ivars == &client_vars && client_vars.claimed);
    assert(driver_vars.openerClient == &client && !deleted && !super_stops);
    assert(cleanup_regions == 0 && !releases);
    irq_cleanup_result = kIOReturnSuccess;

    client_vars.activeDMARegionCount = 1;
    assert(client.Stop(&driver) == kIOReturnStillOpen);
    assert(client.ivars == &client_vars && client_vars.claimed);
    assert(driver_vars.openerClient == &client && driver_vars.claimBlocked);
    assert(!releases && !deleted && !super_stops && bar_releases == 0);

    client_vars.activeDMARegionCount = 0;
    buffer_cleanup_result = kIOReturnStillOpen;
    assert(client.Stop(&driver) == kIOReturnStillOpen);
    assert(client.ivars == &client_vars && client_vars.claimed);
    assert(driver_vars.openerClient == &client && !deleted && !super_stops);
    assert(bar_releases == 0);

    buffer_cleanup_result = kIOReturnSuccess;
    claim_release_result = kIOReturnStillOpen;
    assert(client.Stop(&driver) == kIOReturnStillOpen);
    assert(client.ivars == &client_vars && client_vars.claimed);
    assert(driver_vars.openerClient == &client && driver_vars.claimBlocked);
    assert(!deleted && !super_stops && bar_releases == 0);

    claim_release_result = kIOReturnSuccess;
    assert(client.Stop(&driver) == kIOReturnSuccess);
    assert(!client.ivars && !client_vars.claimed && !driver_vars.openerClient);
    assert(deleted == 1 && super_stops == 1 && releases == 2);
    assert(bar_releases == 1 && !driver_vars.claimBlocked);
    assert(cleanup_regions == 4 && cleanup_irqs == 5);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\nIMPL(VFIOUserPCIDriverUserClient, Stop)")
    end = source.index("\nstatic kern_return_t\nvfio_user_release_all_interrupts(", start)
    body = source[start:end].replace(
        "kern_return_t\nIMPL(VFIOUserPCIDriverUserClient, Stop)",
        "kern_return_t VFIOUserPCIDriverUserClient::Stop(IOService *provider)",
        1)
    body = body.replace("Stop(provider, SUPERDISPATCH)",
                        "super_stop(provider, SUPERDISPATCH)")
    prefix = PREFIX.replace(
        "static int releases, deleted, super_stops, cleanup_regions, cleanup_irqs;",
        "static int releases, deleted, super_stops, cleanup_regions, cleanup_irqs, "
        "bar_releases;")
    prefix = prefix.replace("Object { void release() { releases++; } };",
                            "Object { void release() { releases++; bar_releases++; } };")
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "client_stop.cpp"
        binary = pathlib.Path(directory) / "client_stop"
        test.write_text(prefix + body + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
