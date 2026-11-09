/*
 * Explicit host-only diagnostic: claims the device, creates and maps BAR
 * descriptors for BAR2 and BAR5, then unmaps and releases the claim. It never
 * reads or writes BAR contents and involves no guest or DMA. Claiming can
 * change PCI state. Stops at the first failure.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    unsigned int bus, device, function, vendor, product;
    int bdf_end = 0, id_end = 0;
    if (argc != 3 ||
        sscanf(argv[1], "%x:%x.%x%n", &bus, &device, &function, &bdf_end) != 3 ||
        argv[1][bdf_end] != '\0' || bus > 255 || device > 31 || function > 7 ||
        sscanf(argv[2], "%x:%x%n", &vendor, &product, &id_end) != 2 ||
        argv[2][id_end] != '\0' || vendor >= 0xffff || product > 0xffff) {
        fprintf(stderr, "usage: %s BUS:DEVICE.FUNCTION VENDOR:DEVICE\n", argv[0]);
        return 2;
    }

    CFMutableDictionaryRef matching = IOServiceMatching("IOUserService");
    if (matching == NULL) {
        fprintf(stderr, "Unable to create service matching dictionary\n");
        return 1;
    }
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: %#x\n", kr);
        return 1;
    }
    io_service_t selected = IO_OBJECT_NULL, service;
    unsigned int matches = 0;
    int failed = 0;
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
                kr = IOConnectCallScalarMethod(conn, 0, NULL, 0, identity, &count);
                IOServiceClose(conn);
                if (kr != KERN_SUCCESS || count != 6) {
                    fprintf(stderr, "Identity query failed: %#x count=%u\n", kr, count);
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
        fprintf(stderr, "BAR check aborted: identity discovery errors=%d, matching services=%u\n",
                failed, matches);
        if (selected != IO_OBJECT_NULL) {
            IOObjectRelease(selected);
        }
        return 1;
    }
    io_connect_t conn = IO_OBJECT_NULL;
    kr = IOServiceOpen(selected, mach_task_self(), 0, &conn);
    IOObjectRelease(selected);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Opening selected user client failed: %#x\n", kr);
        return 1;
    }

    int rc = 1;
    int claimed = 0;
    printf("BAR check %02x:%02x.%x %04x:%04x: claiming\n",
           bus, device, function, vendor, product);
    fflush(stdout);
    kr = IOConnectCallMethod(conn, 1, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    printf("claim: %#x\n", kr);
    if (kr != KERN_SUCCESS) {
        goto out;
    }
    claimed = 1;

    static const unsigned int bars[] = {0, 2, 5};
    for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        uint64_t in[1] = {bars[i]};
        uint64_t info[3] = {0};
        uint32_t count = 3;
        kr = IOConnectCallMethod(conn, 10, in, 1, NULL, 0, info, &count,
                                 NULL, NULL);
        printf("BAR%u info: kr=%#x mem_idx=%llu size=%#llx type=%#llx\n",
               bars[i], kr, (unsigned long long)info[0],
               (unsigned long long)info[1], (unsigned long long)info[2]);
        if (kr != KERN_SUCCESS || info[1] == 0) {
            goto out;
        }
        if (bars[i] == 0) {
            continue;
        }
        mach_vm_address_t addr = 0;
        mach_vm_size_t size = 0;
        kr = IOConnectMapMemory64(conn, 1 + bars[i], mach_task_self(), &addr,
                                  &size, kIOMapAnywhere);
        printf("BAR%u map: kr=%#x addr=%#llx size=%#llx\n", bars[i], kr,
               (unsigned long long)addr, (unsigned long long)size);
        if (kr != KERN_SUCCESS) {
            goto out;
        }
        kr = IOConnectUnmapMemory64(conn, 1 + bars[i], mach_task_self(), addr);
        printf("BAR%u unmap: kr=%#x\n", bars[i], kr);
        if (kr != KERN_SUCCESS) {
            goto out;
        }
    }
    rc = 0;

out:
    if (claimed) {
        kr = IOConnectCallMethod(conn, 20, NULL, 0, NULL, 0, NULL, NULL,
                                 NULL, NULL);
        printf("release claim: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            rc = 1;
        }
    }
    IOServiceClose(conn);
    printf("BAR check %s\n", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}
