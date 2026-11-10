#!/usr/bin/env python3
"""Exercise the HVF permission comparison and its failure handling."""

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/scripts/probe-dma-hvf.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int hv_return_t;
typedef int hv_memory_flags_t;
enum { HV_SUCCESS = 0, HV_MEMORY_READ = 1, HV_MEMORY_WRITE = 2,
       HV_MEMORY_EXEC = 4 };
static int map_count, unmap_count, map_results[2], fail_unmap;
static hv_return_t hv_vm_map(void *ptr, uint64_t gpa, size_t size, int flags)
{
    assert(ptr == (void *)0x1000 && gpa == 0x2000 && size == 4096);
    assert(flags == (map_count ? 7 : 3));
    return map_results[map_count++];
}
static hv_return_t hv_vm_unmap(uint64_t gpa, size_t size)
{
    assert(gpa == 0x2000 && size == 4096);
    unmap_count++;
    return fail_unmap ? 9 : 0;
}
"""

MAIN_PREFIX = PREFIX + r"""
#include <stdlib.h>
#include <string.h>
typedef int kern_return_t;
typedef int io_service_t;
typedef int io_connect_t;
typedef uintptr_t mach_vm_address_t;
typedef size_t mach_vm_size_t;
typedef struct DMACpuMapping {
    mach_vm_address_t address;
    mach_vm_size_t size;
    bool mapped;
} DMACpuMapping;
#define DMA_PROBE_SIZE (256ULL * 1024 * 1024)
#define KERN_SUCCESS 0
#define IO_OBJECT_NULL 0
#define HV_VM_DEFAULT 0
#define MAP_FAILED ((void *)-1)
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 1
#define MAP_ANON 2
#define kIOMapAnywhere 0
static int sequence, destroyed, unmapped, freed, released, closed;
static hv_return_t hv_vm_create(int flags) { assert(flags == 0); return 0; }
static hv_return_t hv_vm_destroy(void) { destroyed = ++sequence; return 0; }
static void *mmap(void *a, size_t b, int c, int d, int e, int64_t f)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return (void *)0x1000;
}
static int munmap(void *a, size_t b)
{
    assert(a == (void *)0x1000 && b == DMA_PROBE_SIZE && destroyed);
    return 0;
}
static int parse_device(const char *a, const char *b, unsigned int *c,
                        unsigned int *d, unsigned int *e, unsigned int *f,
                        unsigned int *g)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return 1;
}
static int find_service(unsigned int a, unsigned int b, unsigned int c,
                        unsigned int d, unsigned int e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; return 1;
}
static int mach_task_self(void) { return 1; }
static int IOObjectRelease(int x) { assert(x == 1); return 0; }
static int IOServiceOpen(int service, int task, int type, int *conn)
{
    assert(service == 1 && task == 1 && type == 0); *conn = 1; return 0;
}
static int IOConnectCallMethod(int conn, int selector, const uint64_t *input,
                               int in_count, void *a, int b, uint64_t *output,
                               uint32_t *count, void *c, void *d)
{
    (void)input; (void)in_count; (void)a; (void)b; (void)c; (void)d;
    assert(conn == 1);
    if (selector == 3) {
        assert(*count == 6);
        output[0] = DMA_PROBE_SIZE; output[2] = 1;
        output[3] = 0x8000000; output[4] = DMA_PROBE_SIZE;
    } else if (selector == 4) {
        assert(destroyed && unmapped); freed = ++sequence;
    } else if (selector == 20) {
        assert(freed); released = ++sequence;
    } else {
        assert(selector == 1);
    }
    return 0;
}
static int IOConnectMapMemory64(int conn, int type, int task, uintptr_t *addr,
                                 size_t *size, int flags)
{
    assert(conn == 1 && type == 0 && task == 1 && flags == 0);
    *addr = 0x1000; *size = DMA_PROBE_SIZE; return 0;
}
static int IOConnectUnmapMemory64(int conn, int type, int task, uintptr_t addr)
{
    assert(conn == 1 && type == 0 && task == 1 && addr == 0x1000 && destroyed);
    unmapped = ++sequence; return 0;
}
static int IOServiceClose(int conn)
{
    assert(conn == 1 && released); closed = ++sequence; return 0;
}
"""

SUFFIX = r"""
int main(void)
{
    HVFMapResults result = {0};
    map_results[0] = 0;
    map_results[1] = 4;
    assert(probe_hvf_permissions("test", (void *)0x1000, 0x2000, 4096,
                                 &result));
    assert(result.rw == 0 && result.rwx == 4);
    assert(map_count == 2 && unmap_count == 1);
    map_count = unmap_count = 0;
    map_results[0] = 5;
    map_results[1] = 0;
    assert(probe_hvf_permissions("test", (void *)0x1000, 0x2000, 4096,
                                 &result));
    assert(result.rw == 5 && result.rwx == 0);
    assert(map_count == 2 && unmap_count == 1);
    map_count = unmap_count = 0;
    map_results[0] = 0;
    fail_unmap = 1;
    assert(!probe_hvf_permissions("test", (void *)0x1000, 0x2000, 4096,
                                  &result));
    assert(map_count == 1 && unmap_count == 1);
    return 0;
}
"""


class HVFPermissionTest(unittest.TestCase):
    def test_map_permissions_and_unmap_failure(self):
        source = SOURCE.read_text()
        start = source.index("typedef struct HVFMapResults")
        end = source.index("\nint main(", start)
        with tempfile.TemporaryDirectory() as directory:
            test = pathlib.Path(directory) / "probe.c"
            binary = pathlib.Path(directory) / "probe"
            test.write_text(PREFIX + source[start:end] + SUFFIX)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)

    def test_full_probe_cleanup_order(self):
        source = SOURCE.read_text()
        start = source.index("typedef struct HVFMapResults")
        # The full mock uses the real probe's 256 MiB sizes and live GPA.
        prefix = MAIN_PREFIX.replace("gpa == 0x2000 && size == 4096",
                                     "gpa == 0x8000000 && size == DMA_PROBE_SIZE")
        prefix = prefix.replace("(map_count ? 7 : 3)", "(map_count % 2 ? 7 : 3)")
        prefix = prefix.replace("map_results[map_count++]",
                                "map_results[map_count++ % 2]")
        # Constants must precede the mocked HVF functions that use them.
        prefix = "#define DMA_PROBE_SIZE (256ULL * 1024 * 1024)\n" + prefix
        prefix = prefix.replace("#define DMA_PROBE_SIZE (256ULL * 1024 * 1024)\n"
                                "#define KERN_SUCCESS", "#define KERN_SUCCESS")
        with tempfile.TemporaryDirectory() as directory:
            test = pathlib.Path(directory) / "probe.c"
            binary = pathlib.Path(directory) / "probe"
            test.write_text(prefix + source[start:])
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary), "44:00.0", "1002:731f"], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
