/*
 * Explicit diagnostic only. A PCI rescan can affect other devices and drivers.
 * It does not guarantee that any driver matches or opens the target.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* IOPCIFamily IOPCIDevice.h probe options. */
enum {
    pci_probe_needs_scan = 0x00200000,
    pci_probe_done = 0x80000000u,
};

static int parse_target(const char *bdf, const char *id,
                        unsigned int *bus, unsigned int *dev,
                        unsigned int *fn, unsigned int *vendor,
                        unsigned int *product)
{
    int bdf_end = 0, id_end = 0;
    return sscanf(bdf, "%x:%x.%x%n", bus, dev, fn, &bdf_end) == 3 &&
           bdf[bdf_end] == '\0' && *bus <= 255 && *dev <= 31 && *fn <= 7 &&
           sscanf(id, "%x:%x%n", vendor, product, &id_end) == 2 &&
           id[id_end] == '\0' && *vendor < 0xffff && *product < 0xffff;
}

static int read_bdf(io_service_t service, unsigned int *bus,
                    unsigned int *dev, unsigned int *fn)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, CFSTR("pcidebug"), kCFAllocatorDefault, 0);
    char text[128];
    int end = 0, ok = 0;
    if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)value, text, sizeof(text),
                           kCFStringEncodingUTF8) &&
        sscanf(text, "%u:%u:%u%n", bus, dev, fn, &end) == 3 &&
        (text[end] == '\0' || text[end] == '(') &&
        *bus <= 255 && *dev <= 31 && *fn <= 7) {
        ok = 1;
    }
    if (value) {
        CFRelease(value);
    }
    return ok;
}

static int read_id(io_service_t service, CFStringRef key, unsigned int *out)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, key, kCFAllocatorDefault, 0);
    int ok = 0;
    if (value && CFGetTypeID(value) == CFDataGetTypeID() &&
        CFDataGetLength((CFDataRef)value) >= 2) {
        const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)value);
        *out = (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
        ok = 1;
    }
    if (value) {
        CFRelease(value);
    }
    return ok;
}

int main(int argc, char **argv)
{
    unsigned int bus, dev, fn, vendor, product;
    if (argc != 4 || strcmp(argv[3], "--confirm-rescan") != 0 ||
        !parse_target(argv[1], argv[2], &bus, &dev, &fn, &vendor, &product)) {
        fprintf(stderr, "usage: %s BUS:DEVICE.FUNCTION VENDOR:DEVICE "
                "--confirm-rescan\n", argv[0]);
        return 2;
    }

    CFMutableDictionaryRef matching = IOServiceMatching("IOPCIDevice");
    if (!matching) {
        fprintf(stderr, "Unable to create PCI matching dictionary\n");
        return 1;
    }
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "PCI enumeration failed: %#x\n", kr);
        return 1;
    }

    unsigned int matches = 0;
    io_service_t selected = IO_OBJECT_NULL, service;
    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        unsigned int b, d, f;
        if (read_bdf(service, &b, &d, &f) &&
            b == bus && d == dev && f == fn) {
            matches++;
            if (matches == 1) {
                selected = service;
                continue;
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    if (matches != 1) {
        fprintf(stderr, "Rescan aborted: expected one BDF, found %u\n", matches);
        if (selected) {
            IOObjectRelease(selected);
        }
        return 1;
    }

    unsigned int found_vendor, found_product;
    if (!read_id(selected, CFSTR("vendor-id"), &found_vendor) ||
        !read_id(selected, CFSTR("device-id"), &found_product) ||
        found_vendor != vendor || found_product != product) {
        fprintf(stderr, "Rescan aborted: PCI identity is unavailable or differs\n");
        IOObjectRelease(selected);
        return 1;
    }

    printf("Requesting PCI rescan at %02x:%02x.%x %04x:%04x. "
           "Other PCI devices may be affected.\n", bus, dev, fn, vendor, product);
    fflush(stdout);
    kr = IOServiceRequestProbe(selected, pci_probe_needs_scan | pci_probe_done);
    IOObjectRelease(selected);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "PCI rescan request failed: %#x\n", kr);
        return 1;
    }
    printf("PCI rescan request returned success; this does not prove a VFIO "
           "match or successful PCI Open. Check list-devices again.\n");
    return 0;
}
