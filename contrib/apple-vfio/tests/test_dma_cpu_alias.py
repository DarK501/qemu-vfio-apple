#!/usr/bin/env python3
"""Exercise the host probe's alias check with shared and distinct mappings."""

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/scripts/probe-dma-allocation.c"

PREFIX = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
typedef int io_connect_t;
typedef int kern_return_t;
typedef uintptr_t mach_vm_address_t;
typedef size_t mach_vm_size_t;
typedef int vm_prot_t;
enum { KERN_SUCCESS = 0, kIOMapAnywhere = 0, VM_FLAGS_ANYWHERE = 1,
       VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_INHERIT_NONE = 2 };
static void *addresses[2];
static size_t map_size;
static int map_count, remap_count, unmap_count, dealloc_count;
static int fail_map, fail_remap, fail_unmap, fail_dealloc, protection;
static int mach_task_self(void) { return 7; }
static int IOConnectMapMemory64(int conn, int type, int task, uintptr_t *addr,
                                 size_t *size, int flags)
{
    assert(conn == 1 && type == 0 && task == 7 && flags == 0);
    map_count++;
    if (fail_map) { return 9; }
    *addr = (uintptr_t)addresses[0];
    *size = map_size;
    return 0;
}
static int mach_vm_remap(int target, uintptr_t *addr, size_t size,
                         uintptr_t mask, int flags, int source,
                         uintptr_t srcaddr, int copy, int *current,
                         int *maximum, int inherit)
{
    assert(target == 7 && source == 7 && size == 4096 && mask == 0 &&
           flags == VM_FLAGS_ANYWHERE && srcaddr == (uintptr_t)addresses[0] &&
           !copy && inherit == VM_INHERIT_NONE);
    remap_count++;
    if (fail_remap) { return 9; }
    *addr = (uintptr_t)addresses[1];
    *current = *maximum = protection;
    return 0;
}
static int mach_vm_deallocate(int task, uintptr_t addr, size_t size)
{
    assert(task == 7 && addr == (uintptr_t)addresses[1] && size == 4096);
    dealloc_count++;
    return fail_dealloc ? 10 : 0;
}
static int IOConnectUnmapMemory64(int conn, int type, int task, uintptr_t addr)
{
    assert(conn == 1 && type == 0 && task == 7);
    assert(addr == (uintptr_t)addresses[0]);
    unmap_count++;
    return fail_unmap ? 10 : 0;
}
"""

SUFFIX = r"""
static void reset(void)
{
    map_count = remap_count = unmap_count = dealloc_count = 0;
    fail_map = fail_remap = fail_unmap = fail_dealloc = 0;
    protection = VM_PROT_READ | VM_PROT_WRITE;
    map_size = 4096;
}
int main(void)
{
    FILE *f = tmpfile();
    assert(f && ftruncate(fileno(f), 4096) == 0);
    void *first = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fileno(f), 0);
    void *second = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fileno(f), 0);
    FILE *other = tmpfile();
    assert(other && ftruncate(fileno(other), 4096) == 0);
    void *different = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                           fileno(other), 0);
    assert(first != MAP_FAILED && second != MAP_FAILED &&
           different != MAP_FAILED && first != second);
    addresses[0] = first; addresses[1] = second;
    ((uint8_t *)first)[0] = 0x37;
    ((uint8_t *)first)[2048] = 0x58;
    ((uint8_t *)first)[4095] = 0x7a;
    DMACpuMapping mappings[2] = {0};
    reset();
    assert(probe_cpu_alias(1, 4096, mappings));
    assert(((uint8_t *)second)[0] == 0x37 &&
           ((uint8_t *)second)[2048] == 0x58 &&
           ((uint8_t *)second)[4095] == 0x7a);
    assert(unmap_cpu_alias(1, mappings) && unmap_count == 1 &&
           dealloc_count == 1);
    reset();
    mappings[0] = (DMACpuMapping){0}; mappings[1] = (DMACpuMapping){0};
    fail_map = 1;
    assert(!probe_cpu_alias(1, 4096, mappings));
    assert(!mappings[0].mapped && !mappings[1].mapped);
    assert(unmap_cpu_alias(1, mappings) && !unmap_count && !dealloc_count);
    reset();
    mappings[0] = (DMACpuMapping){0}; mappings[1] = (DMACpuMapping){0};
    fail_remap = 1;
    assert(!probe_cpu_alias(1, 4096, mappings));
    assert(mappings[0].mapped && !mappings[1].mapped);
    assert(unmap_cpu_alias(1, mappings) && unmap_count == 1);
    reset();
    mappings[0] = (DMACpuMapping){0}; mappings[1] = (DMACpuMapping){0};
    map_size = 2048;
    assert(!probe_cpu_alias(1, 4096, mappings));
    assert(mappings[0].mapped && !mappings[1].mapped);
    assert(unmap_cpu_alias(1, mappings) && unmap_count == 1);
    reset();
    mappings[0] = (DMACpuMapping){0}; mappings[1] = (DMACpuMapping){0};
    protection = VM_PROT_READ;
    assert(!probe_cpu_alias(1, 4096, mappings));
    assert(unmap_cpu_alias(1, mappings) && unmap_count == 1 &&
           dealloc_count == 1);
    reset();
    mappings[0] = (DMACpuMapping){0}; mappings[1] = (DMACpuMapping){0};
    addresses[1] = different;
    assert(!probe_cpu_alias(1, 4096, mappings));
    assert(((uint8_t *)first)[0] == 0x37);
    fail_dealloc = 1;
    assert(!unmap_cpu_alias(1, mappings));
    assert(mappings[0].mapped && mappings[1].mapped && unmap_count == 0);
    fail_dealloc = 0;
    fail_unmap = 1;
    assert(!unmap_cpu_alias(1, mappings));
    assert(mappings[0].mapped && !mappings[1].mapped && unmap_count == 1);
    assert(munmap(first, 4096) == 0 && munmap(second, 4096) == 0 &&
           munmap(different, 4096) == 0);
    assert(fclose(f) == 0 && fclose(other) == 0);
    return 0;
}
"""


class CpuAliasTests(unittest.TestCase):
    def test_alias_and_cleanup(self):
        source = SOURCE.read_text()
        start = source.index("typedef struct DMACpuMapping")
        end = source.index("static int parse_device(", start)
        with tempfile.TemporaryDirectory() as directory:
            test = pathlib.Path(directory) / "alias.c"
            binary = pathlib.Path(directory) / "alias"
            test.write_text(PREFIX + source[start:end] + SUFFIX)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
