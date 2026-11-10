#!/usr/bin/env python3
"""Exercise DMA allocation reply validation and rollback."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dext-client.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
typedef int kern_return_t;
typedef int io_connect_t;
typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;
enum { KERN_SUCCESS = 0, kIOReturnSuccess = 0, IO_OBJECT_NULL = 0,
       kIOReturnBadArgument = 1,
       kIOReturnError = 2, kSelectorAllocateDMABuffer = 3,
       kIOMapAnywhere = 0 };
static int allocation_result, mapping_result, cleanup_result;
static int calls, maps, cleanups, unmaps, reports;
static uint32_t reply_count = 6;
static uint64_t reply[6] = {4096, 3, 1, 0x10000, 4096, 0};
static uint64_t mapped_addr = 0x20000, mapped_size = 4096, cleanup_addr;
static void error_report(const char *fmt, ...) { (void)fmt; reports++; }
static int mach_task_self(void) { return 7; }
static int IOConnectCallMethod(int conn, int selector, const uint64_t *input,
    int input_count, void *input_struct, int input_size, uint64_t *output,
    uint32_t *output_count, void *output_struct, void *output_size)
{
    assert(conn == 1 && selector == kSelectorAllocateDMABuffer);
    assert(input_count == 2 && input[0] == 4096 && input[1] == 4096);
    assert(!input_struct && !input_size && !output_struct && !output_size);
    calls++;
    for (int i = 0; i < 6; i++) { output[i] = reply[i]; }
    *output_count = reply_count;
    return allocation_result;
}
static int IOConnectMapMemory64(int conn, int type, int task, uint64_t *addr,
                                 uint64_t *size, int flags)
{
    assert(conn == 1 && type == 0 && task == 7 && flags == kIOMapAnywhere);
    maps++;
    if (!mapping_result) { *addr = mapped_addr; *size = mapped_size; }
    return mapping_result;
}
static int IOConnectUnmapMemory64(int conn, int type, int task, uint64_t addr)
{
    assert(conn == 1 && type == 0 && task == 7 && addr == 0);
    unmaps++;
    return 0;
}
static int apple_dext_client_free_dma_buffer(int conn, uint64_t addr)
{
    assert(conn == 1);
    cleanups++;
    cleanup_addr = addr;
    return cleanup_result;
}
"""

SUFFIX = r"""
static void reset(void)
{
    calls = maps = cleanups = unmaps = reports = 0;
    allocation_result = mapping_result = cleanup_result = 0;
    reply_count = 6;
    reply[0] = 4096; reply[1] = 3; reply[2] = 1;
    reply[3] = 0x10000; reply[4] = 4096; reply[5] = 0;
    mapped_addr = 0x20000; mapped_size = 4096; cleanup_addr = 0;
}
static int allocate(uint64_t *bus, uint64_t *len, uint64_t *addr, uint64_t *size)
{
    return apple_dext_client_allocate_dma_buffer(1, 4096, 4096,
                                                  bus, len, addr, size);
}
int main(void)
{
    uint64_t bus = 9, len = 9, addr = 9, size = 9;
    reset();
    assert(allocate(&bus, &len, &addr, &size) == 0);
    assert(bus == 0x10000 && len == 4096 && addr == 0x20000 && size == 4096);
    assert(calls == 1 && maps == 1 && !cleanups);
    reset();
    assert(allocate(&bus, &len, &addr, NULL) == kIOReturnBadArgument && !calls);
    reset();
    allocation_result = 10;
    assert(allocate(&bus, &len, &addr, &size) == 10);
    assert(!maps && !cleanups);
    reset();
    reply_count = 5;
    assert(allocate(&bus, &len, &addr, &size) == kIOReturnError);
    assert(cleanups == 1 && cleanup_addr == 0 && !maps && reports == 1);
    reset();
    reply[2] = 33;
    assert(allocate(&bus, &len, &addr, &size) == kIOReturnError);
    assert(cleanups == 1 && !maps);
    reset();
    reply[3] = UINT64_MAX - 4094;
    assert(allocate(&bus, &len, &addr, &size) == kIOReturnError);
    assert(cleanups == 1 && cleanup_addr == 0 && !maps && reports == 1);
    reset();
    reply[3] = UINT64_MAX - 4095;
    assert(allocate(&bus, &len, &addr, &size) == 0);
    assert(bus == UINT64_MAX - 4095 && len == 4096);
    reset();
    mapping_result = 11;
    assert(allocate(&bus, &len, &addr, &size) == 11);
    assert(cleanups == 1 && cleanup_addr == 0);
    reset();
    mapped_size = 2048;
    assert(allocate(&bus, &len, &addr, &size) == kIOReturnError);
    assert(cleanups == 1 && cleanup_addr == 0x20000);
    reset();
    mapped_addr = UINTPTR_MAX - 4094;
    assert(allocate(&bus, &len, &addr, &size) == kIOReturnError);
    assert(cleanups == 1 && cleanup_addr == mapped_addr && reports == 1);
    reset();
    mapped_addr = UINTPTR_MAX - 4095;
    assert(allocate(&bus, &len, &addr, &size) == 0);
    assert(addr == mapped_addr && size == 4096);
    reset();
    mapped_addr = 0;
    assert(allocate(&bus, &len, &addr, &size) == kIOReturnError);
    assert(cleanups == 1 && unmaps == 1);
    reset();
    mapping_result = 11; cleanup_result = 12;
    assert(allocate(&bus, &len, &addr, &size) == 12);
    assert(cleanups == 1 && reports == 1);
    reset();
    assert(allocate(&bus, &len, NULL, NULL) == 0);
    assert(!maps && !cleanups && bus == 0x10000 && len == 4096);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\napple_dext_client_allocate_dma_buffer(")
    end = source.index("\nkern_return_t\napple_dext_client_free_dma_buffer(",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "bounce_allocation.c"
        binary = pathlib.Path(directory) / "bounce_allocation"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
