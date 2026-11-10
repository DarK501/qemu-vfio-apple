/*
 * Host-only DMA mapping probe. It claims the selected PCI device, asks the
 * dext to prepare a 256 MiB DMA buffer, then completes and releases it. It
 * does not start a VM, map BARs, or issue device work. Claiming enables bus
 * mastering; run only when the device is otherwise idle.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMA_PROBE_SIZE (256ULL * 1024 * 1024)
#define DMA_PROBE_DEFAULT_ALIGNMENT 4096ULL

typedef struct DMACpuMapping {
    mach_vm_address_t address;
    mach_vm_size_t size;
    bool mapped;
} DMACpuMapping;

static bool probe_cpu_alias(io_connect_t conn, uint64_t size,
                            DMACpuMapping mappings[2])
{
    vm_prot_t current = 0, maximum = 0;
    kern_return_t kr;

    kr = IOConnectMapMemory64(conn, 0, mach_task_self(),
                              &mappings[0].address, &mappings[0].size,
                              kIOMapAnywhere);
    printf("IOKit map: kr=%#x address=%#llx size=%#llx\n", kr,
           (unsigned long long)mappings[0].address,
           (unsigned long long)mappings[0].size);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    mappings[0].mapped = true;
    if (mappings[0].address == 0 || mappings[0].size < size ||
        mappings[0].address > UINTPTR_MAX ||
        size - 1 > UINTPTR_MAX - mappings[0].address) {
        fprintf(stderr, "IOKit map does not cover the requested buffer\n");
        return false;
    }

    kr = mach_vm_remap(mach_task_self(), &mappings[1].address, size, 0,
                       VM_FLAGS_ANYWHERE, mach_task_self(), mappings[0].address,
                       false, &current, &maximum, VM_INHERIT_NONE);
    printf("Mach shared remap: kr=%#x address=%#llx protections=%#x/%#x\n",
           kr, (unsigned long long)mappings[1].address,
           (unsigned int)current, (unsigned int)maximum);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    mappings[1].size = size;
    mappings[1].mapped = true;
    if (mappings[1].address == 0 || mappings[1].address > UINTPTR_MAX ||
        size - 1 > UINTPTR_MAX - mappings[1].address ||
        (current & (VM_PROT_READ | VM_PROT_WRITE)) !=
        (VM_PROT_READ | VM_PROT_WRITE)) {
        fprintf(stderr, "Mach alias lacks required address range or permissions\n");
        return false;
    }

    uint64_t first = mappings[0].address, second = mappings[1].address;
    if ((first <= second && second - first < size) ||
        (second <= first && first - second < size)) {
        fprintf(stderr, "CPU mappings overlap; alias test is inconclusive\n");
        return false;
    }

    volatile uint8_t *a = (volatile uint8_t *)(uintptr_t)first;
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)second;
    const uint64_t offsets[] = {0, size / 2, size - 1};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint64_t offset = offsets[i];
        uint8_t saved = a[offset];
        uint8_t value = (uint8_t)(saved ^ 0x5a);
        a[offset] = value;
        if (b[offset] != value) {
            fprintf(stderr, "CPU alias mismatch: map 0 -> map 1 at offset %#llx\n",
                    (unsigned long long)offset);
            a[offset] = saved;
            return false;
        }
        value ^= 0xff;
        b[offset] = value;
        if (a[offset] != value) {
            fprintf(stderr, "CPU alias mismatch: map 1 -> map 0 at offset %#llx\n",
                    (unsigned long long)offset);
            a[offset] = saved;
            return false;
        }
        a[offset] = saved;
    }
    printf("CPU alias: PASS (three offsets, both directions)\n");
    return true;
}

static bool unmap_cpu_alias(io_connect_t conn, DMACpuMapping mappings[2])
{
    kern_return_t kr;

    if (mappings[1].mapped) {
        kr = mach_vm_deallocate(mach_task_self(), mappings[1].address,
                                mappings[1].size);
        printf("Mach alias deallocate: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            return false;
        }
        mappings[1].mapped = false;
    }
    if (mappings[0].mapped) {
        kr = IOConnectUnmapMemory64(conn, 0, mach_task_self(),
                                    mappings[0].address);
        printf("IOKit unmap: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            return false;
        }
        mappings[0].mapped = false;
    }
    return true;
}

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
    uint64_t alignment = DMA_PROBE_DEFAULT_ALIGNMENT;
    io_service_t service;
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr;
    int claimed = 0, allocation_attempted = 0, failed = 0;
    bool test_alias = false, mappings_unmapped = true;
    DMACpuMapping mappings[2] = {0};

    if ((argc < 3 || argc > 5) ||
        (argc == 5 && strcmp(argv[4], "--cpu-alias") != 0) ||
        !parse_device(argv[1], argv[2], &bus, &device, &function,
                      &vendor, &product)) {
        fprintf(stderr, "usage: %s BUS:DEVICE.FUNCTION VENDOR:DEVICE "
                "[ALIGNMENT [--cpu-alias]]\n", argv[0]);
        return 2;
    }
    test_alias = argc == 5;
    if (argc >= 4) {
        char *end = NULL;
        unsigned long long parsed;

        parsed = strtoull(argv[3], &end, 0);
        if (argv[3][0] == '\0' || end == argv[3] || *end != '\0' ||
            parsed < DMA_PROBE_DEFAULT_ALIGNMENT ||
            (parsed & (parsed - 1)) != 0) {
            fprintf(stderr, "alignment must be a power of two >= %llu\n",
                    (unsigned long long)DMA_PROBE_DEFAULT_ALIGNMENT);
            return 2;
        }
        alignment = (uint64_t)parsed;
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
           (unsigned long long)alignment);
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
        uint64_t input[2] = {DMA_PROBE_SIZE, alignment};
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
            if (!failed && test_alias &&
                !probe_cpu_alias(conn, DMA_PROBE_SIZE, mappings)) {
                failed = 1;
            }
        }
    }

    if (allocation_attempted) {
        mappings_unmapped = unmap_cpu_alias(conn, mappings);
        if (!mappings_unmapped) {
            failed = 1;
            fprintf(stderr, "CPU unmap failed; skipping explicit DMA buffer "
                    "free and claim release; client close may attempt cleanup\n");
        }
    }

    if (allocation_attempted && mappings_unmapped) {
        kr = IOConnectCallMethod(conn, 4, NULL, 0, NULL, 0, NULL, NULL,
                                 NULL, NULL);
        printf("free DMA buffer: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            failed = 1;
            fprintf(stderr, "DMA completion failed; claim release will be "
                    "refused while the allocation remains live\n");
        }
    }

    if (claimed && mappings_unmapped) {
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
