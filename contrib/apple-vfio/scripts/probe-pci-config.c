/* Explicit diagnostic: opening and closing a PCI device can change its state. */
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
        fprintf(stderr, "Probe aborted: identity discovery errors=%d, matching services=%u\n",
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
    uint64_t input[] = {bus, device, function, vendor, product};
    uint64_t output[3] = {0};
    uint32_t count = 3;
    printf("Probing %02x:%02x.%x %04x:%04x; PCI Open/Close may change device state.\n",
           bus, device, function, vendor, product);
    fflush(stdout);
    kr = IOConnectCallScalarMethod(conn, 18, input, 5, output, &count);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS || count != 3) {
        fprintf(stderr, "PCI configuration probe failed: %#x count=%u\n", kr, count);
        return 1;
    }
    printf("Live PCI configuration: vendor=%04llx device=%04llx command=%04llx "
           "(bus mastering %s)\n",
           (unsigned long long)output[0], (unsigned long long)output[1],
           (unsigned long long)output[2], output[2] & 4 ? "enabled" : "disabled");
    return 0;
}
