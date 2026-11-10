/*
 * Host-only DMA mapping probe. It claims the selected PCI device, asks the
 * dext to prepare a 256 MiB DMA buffer, then completes and releases it. It
 * does not start a VM, map BARs, or issue device work. Claiming enables bus
 * mastering; run only when the device is otherwise idle.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DMA_PROBE_SIZE (256ULL * 1024 * 1024)
#define DMA_PROBE_ALIGNMENT 4096ULL

static int parse_device(const char *bdf, const char *id,
                        unsigned int *bus, unsigned int *device,
                        unsigned int *function, unsigned int *vendor,
                        unsigned int *product)
{
    int bdf_end = 0, id_end = 0;

    return sscanf(bdf, "%x:%x.%x%n", bus, device, function, &bdf_end) == 3 &&
           bdf[bdf_end] == '\0' && *bus <= 255 && *device <= 31 &&
           *function <= 7 &&
           sscanf(id, "%x:%x%n", vendor, product, &id_end) == 2 &&
           id[id_end] == '\0' && *vendor < 0xffff && *product <= 0xffff;
}

static io_service_t find_service(unsigned int bus, unsigned int device,
                                 unsigned int function, unsigned int vendor,
                                 unsigned int product)
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOUserService");
    io_iterator_t iter = IO_OBJECT_NULL;
    io_service_t selected = IO_OBJECT_NULL, service;
    unsigned int matches = 0;
    int failed = 0;
    kern_return_t kr;

    if (matching == NULL) {
        fprintf(stderr, "Unable to create service matching dictionary\n");
        return IO_OBJECT_NULL;
    }
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: %#x\n", kr);
        return IO_OBJECT_NULL;
    }

    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        CFTypeRef cls = IORegistryEntryCreateCFProperty(
            service, CFSTR("IOUserClass"), kCFAllocatorDefault, 0);
        int is_vfio = cls != NULL && CFGetTypeID(cls) == CFStringGetTypeID() &&
                      CFEqual(cls, CFSTR("VFIOUserPCIDriver"));
        if (cls != NULL) {
            CFRelease(cls);
        }
        if (is_vfio) {
            io_connect_t conn = IO_OBJECT_NULL;
            kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
            if (kr != KERN_SUCCESS) {
                fprintf(stderr, "Opening VFIO user client failed: %#x\n", kr);
                failed = 1;
            } else {
                uint64_t identity[6] = {0};
                uint32_t count = 6;
                kr = IOConnectCallScalarMethod(conn, 0, NULL, 0, identity,
                                               &count);
                IOServiceClose(conn);
                if (kr != KERN_SUCCESS || count != 6) {
                    fprintf(stderr, "Identity query failed: %#x count=%u\n",
                            kr, count);
                    failed = 1;
                } else if (identity[0] == bus && identity[1] == device &&
                           identity[2] == function && identity[3] == vendor &&
                           identity[4] == product) {
                    matches++;
                    if (matches == 1) {
                        IOObjectRetain(service);
                        selected = service;
                    }
                }
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);

    if (failed || matches != 1) {
        fprintf(stderr,
                "Probe aborted: identity discovery errors=%d, matching services=%u\n",
                failed, matches);
        if (selected != IO_OBJECT_NULL) {
            IOObjectRelease(selected);
        }
        return IO_OBJECT_NULL;
    }
    return selected;
}

int main(int argc, char **argv)
{
    unsigned int bus = 0, device = 0, function = 0, vendor = 0, product = 0;
    io_service_t service;
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr;
    int claimed = 0, allocation_attempted = 0, failed = 0;

    if (argc != 3 ||
        !parse_device(argv[1], argv[2], &bus, &device, &function,
                      &vendor, &product)) {
        fprintf(stderr, "usage: %s BUS:DEVICE.FUNCTION VENDOR:DEVICE\n", argv[0]);
        return 2;
    }

    service = find_service(bus, device, function, vendor, product);
    if (service == IO_OBJECT_NULL) {
        return 1;
    }
    kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Opening selected user client failed: %#x\n", kr);
        return 1;
    }

    printf("DMA mapping probe %02x:%02x.%x %04x:%04x; "
           "no device work or guest VM will be started\n",
           bus, device, function, vendor, product);
    printf("Requesting %llu bytes with %llu-byte alignment\n",
           (unsigned long long)DMA_PROBE_SIZE,
           (unsigned long long)DMA_PROBE_ALIGNMENT);
    fflush(stdout);

    kr = IOConnectCallMethod(conn, 1, NULL, 0, NULL, 0, NULL, NULL,
                             NULL, NULL);
    printf("claim: %#x\n", kr);
    if (kr != KERN_SUCCESS) {
        failed = 1;
        goto out;
    }
    claimed = 1;

    {
        uint64_t input[2] = {DMA_PROBE_SIZE, DMA_PROBE_ALIGNMENT};
        uint64_t output[6] = {0};
        uint32_t output_count = 6;

        allocation_attempted = 1;
        kr = IOConnectCallMethod(conn, 3, input, 2, NULL, 0, output,
                                 &output_count, NULL, NULL);
        printf("allocate: %#x\n", kr);
        if (kr != KERN_SUCCESS || output_count != 6) {
            fprintf(stderr, "DMA buffer allocation failed or returned "
                    "unexpected output count %u\n", output_count);
            failed = 1;
        } else {
            printf("allocated=%llu flags=%#llx segments=%llu "
                   "first_segment=%#llx/%#llx class=%#llx\n",
                   (unsigned long long)output[0],
                   (unsigned long long)output[1],
                   (unsigned long long)output[2],
                   (unsigned long long)output[3],
                   (unsigned long long)output[4],
                   (unsigned long long)output[5]);
            if (output[0] != DMA_PROBE_SIZE || output[2] == 0) {
                fprintf(stderr, "DMA allocation result is incomplete\n");
                failed = 1;
            }
        }
    }

    if (allocation_attempted) {
        kr = IOConnectCallMethod(conn, 4, NULL, 0, NULL, 0, NULL, NULL,
                                 NULL, NULL);
        printf("free DMA buffer: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            failed = 1;
        }
    }

    if (claimed) {
        kr = IOConnectCallMethod(conn, 20, NULL, 0, NULL, 0, NULL, NULL,
                                 NULL, NULL);
        printf("release claim: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            failed = 1;
        }
    }

out:
    IOServiceClose(conn);
    printf("DMA mapping probe %s\n", failed ? "FAILED" : "PASSED");
    return failed ? 1 : 0;
}
