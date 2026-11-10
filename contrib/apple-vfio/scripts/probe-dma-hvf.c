/*
 * Compare HVF mappings of ordinary memory and a DriverKit DMA allocation.
 * The selected device is claimed and bus mastering is enabled. No vCPU or
 * device work is started. Run only on an idle, unclaimed device.
 */
#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>

#define main dma_allocation_probe_main
#include "probe-dma-allocation.c"
#undef main

typedef struct HVFMapResults {
    hv_return_t rw;
    hv_return_t rwx;
} HVFMapResults;

static bool probe_hvf_permissions(const char *label, void *memory, uint64_t gpa,
                                  size_t size, HVFMapResults *results)
{
    const hv_memory_flags_t flags[] = {
        HV_MEMORY_READ | HV_MEMORY_WRITE,
        HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC,
    };
    hv_return_t *values[] = {&results->rw, &results->rwx};
    const char *names[] = {"RW", "RWX"};

    for (size_t i = 0; i < 2; i++) {
        hv_return_t ret = hv_vm_map(memory, gpa, size, flags[i]);
        *values[i] = ret;
        printf("%s %s map: %#x\n", label, names[i], (unsigned int)ret);
        if (ret == HV_SUCCESS) {
            ret = hv_vm_unmap(gpa, size);
            printf("%s %s unmap: %#x\n", label, names[i], (unsigned int)ret);
            if (ret != HV_SUCCESS) {
                fprintf(stderr, "HVF unmap failed; stopping mapping tests\n");
                return false;
            }
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    unsigned int bus = 0, device = 0, function = 0, vendor = 0, product = 0;
    uint64_t input[2] = {DMA_PROBE_SIZE, 128ULL * 1024 * 1024};
    uint64_t output[6] = {0};
    uint32_t count = 6;
    io_service_t service = IO_OBJECT_NULL;
    io_connect_t conn = IO_OBJECT_NULL;
    DMACpuMapping mapping = {0};
    HVFMapResults control = {0}, dma = {0};
    void *ordinary = MAP_FAILED;
    bool vm_created = false, claimed = false, allocated = false;
    bool vm_destroyed = true;
    int failed = 0;
    kern_return_t kr;
    hv_return_t hvret;

    if (argc != 3 ||
        !parse_device(argv[1], argv[2], &bus, &device, &function,
                      &vendor, &product)) {
        fprintf(stderr, "usage: %s BUS:DEVICE.FUNCTION VENDOR:DEVICE\n", argv[0]);
        return 2;
    }
    printf("HVF mapping probe %s %s: claims device, enables bus mastering; "
           "no vCPU or device work is started\n", argv[1], argv[2]);
    fflush(stdout);

    hvret = hv_vm_create(HV_VM_DEFAULT);
    printf("HVF VM create: %#x\n", (unsigned int)hvret);
    if (hvret != HV_SUCCESS) {
        return 1;
    }
    vm_created = true;
    ordinary = mmap(NULL, DMA_PROBE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ordinary == MAP_FAILED) {
        perror("ordinary memory mmap");
        failed = 1;
        goto cleanup;
    }
    service = find_service(bus, device, function, vendor, product);
    if (service == IO_OBJECT_NULL) {
        failed = 1;
        goto cleanup;
    }
    kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    service = IO_OBJECT_NULL;
    printf("open: %#x\n", kr);
    if (kr != KERN_SUCCESS) {
        failed = 1;
        goto cleanup;
    }
    kr = IOConnectCallMethod(conn, 1, NULL, 0, NULL, 0, NULL, NULL,
                             NULL, NULL);
    printf("claim: %#x\n", kr);
    if (kr != KERN_SUCCESS) {
        failed = 1;
        goto cleanup;
    }
    claimed = true;
    kr = IOConnectCallMethod(conn, 3, input, 2, NULL, 0, output, &count,
                             NULL, NULL);
    printf("allocate: %#x\n", kr);
    if (kr != KERN_SUCCESS) {
        failed = 1;
        goto cleanup;
    }
    allocated = true;
    if (count != 6 || output[0] != DMA_PROBE_SIZE || output[2] != 1 ||
        output[3] == 0 || output[3] % input[1] ||
        output[4] < DMA_PROBE_SIZE ||
        output[3] > UINT64_MAX - DMA_PROBE_SIZE) {
        fprintf(stderr, "DMA allocation does not provide an aligned, "
                "contiguous test range\n");
        failed = 1;
        goto cleanup;
    }
    printf("DMA address: %#llx size: %#llx\n",
           (unsigned long long)output[3], (unsigned long long)output[0]);
    kr = IOConnectMapMemory64(conn, 0, mach_task_self(),
                              &mapping.address, &mapping.size, kIOMapAnywhere);
    printf("IOKit map: %#x address=%#llx size=%#llx\n", kr,
           (unsigned long long)mapping.address,
           (unsigned long long)mapping.size);
    if (kr != KERN_SUCCESS) {
        failed = 1;
        goto cleanup;
    }
    mapping.mapped = true;
    if (!mapping.address || mapping.size < DMA_PROBE_SIZE ||
        mapping.address > UINTPTR_MAX ||
        DMA_PROBE_SIZE - 1 > UINTPTR_MAX - mapping.address) {
        fprintf(stderr, "invalid CPU mapping range\n");
        failed = 1;
        goto cleanup;
    }

    if (!probe_hvf_permissions("ordinary", ordinary, output[3],
                               DMA_PROBE_SIZE, &control) ||
        control.rw != HV_SUCCESS || control.rwx != HV_SUCCESS) {
        fprintf(stderr, "ordinary memory control failed; comparison incomplete\n");
        failed = 1;
        goto cleanup;
    }
    if (!probe_hvf_permissions("DriverKit", (void *)(uintptr_t)mapping.address,
                               output[3], DMA_PROBE_SIZE, &dma)) {
        failed = 1;
        goto cleanup;
    }
    if (dma.rw != HV_SUCCESS) {
        fprintf(stderr, "DriverKit RW mapping failed; no usable RW mapping shown\n");
        failed = 1;
    }

cleanup:
    if (vm_created) {
        hvret = hv_vm_destroy();
        printf("HVF VM destroy: %#x\n", (unsigned int)hvret);
        if (hvret != HV_SUCCESS) {
            fprintf(stderr, "HVF teardown failed; skipping explicit DMA cleanup; "
                    "process exit may force teardown\n");
            failed = 1;
            vm_destroyed = false;
        }
    }
    if (vm_destroyed && ordinary != MAP_FAILED &&
        munmap(ordinary, DMA_PROBE_SIZE) != 0) {
        perror("ordinary memory munmap");
        failed = 1;
    }
    if (vm_destroyed && mapping.mapped) {
        kr = IOConnectUnmapMemory64(conn, 0, mach_task_self(), mapping.address);
        printf("IOKit unmap: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "IOKit unmap failed; skipping explicit DMA cleanup\n");
            failed = 1;
            vm_destroyed = false;
        }
    }
    if (vm_destroyed && allocated) {
        kr = IOConnectCallMethod(conn, 4, NULL, 0, NULL, 0, NULL, NULL,
                                 NULL, NULL);
        printf("free DMA buffer: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "DMA buffer completion failed; skipping claim release\n");
            failed = 1;
            vm_destroyed = false;
        }
    }
    if (vm_destroyed && claimed) {
        kr = IOConnectCallMethod(conn, 20, NULL, 0, NULL, 0, NULL, NULL,
                                 NULL, NULL);
        printf("release claim: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            failed = 1;
        }
    }
    if (conn != IO_OBJECT_NULL) {
        kr = IOServiceClose(conn);
        printf("user client close: %#x\n", kr);
        if (kr != KERN_SUCCESS) {
            failed = 1;
        }
    }
    printf("HVF mapping probe %s\n", failed ? "INCOMPLETE" : "COMPLETE");
    return failed ? 1 : 0;
}
