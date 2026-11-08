#!/usr/bin/env python3
"""Verify bounce-buffer IPC completes before unmapping its host address."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dext-client.c"

PREFIX = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int kern_return_t;
typedef int io_connect_t;
typedef uintptr_t mach_vm_address_t;
enum { KERN_SUCCESS = 0, IO_OBJECT_NULL = 0, kIOReturnBadArgument = 1,
       kSelectorFreeDMABuffer = 2 };
static int calls, unmaps, call_result, unmap_result;
static int mach_task_self(void) { return 7; }
static int IOConnectCallMethod(int conn, int selector, void *input,
    int input_count, void *input_struct, int input_size, void *output,
    void *output_count, void *output_struct, void *output_size)
{
    assert(conn == 1 && selector == kSelectorFreeDMABuffer);
    assert(!input && !input_count && !input_struct && !input_size);
    assert(!output && !output_count && !output_struct && !output_size);
    calls++;
    return call_result;
}
static int IOConnectUnmapMemory64(int conn, int type, int task, uintptr_t addr)
{
    assert(conn == 1 && type == 0 && task == 7 && addr == 0x1000);
    assert(calls == 1);
    unmaps++;
    return unmap_result;
}
"""

SUFFIX = r"""
int main(void)
{
    assert(apple_dext_client_free_dma_buffer(0, 0x1000) == kIOReturnBadArgument);
    call_result = 9;
    assert(apple_dext_client_free_dma_buffer(1, 0x1000) == 9);
    assert(calls == 1 && !unmaps);
    calls = 0;
    call_result = 0;
    unmap_result = 8;
    assert(apple_dext_client_free_dma_buffer(1, 0x1000) == 8);
    assert(calls == 1 && unmaps == 1);
    calls = unmaps = unmap_result = 0;
    assert(apple_dext_client_free_dma_buffer(1, 0) == KERN_SUCCESS);
    assert(calls == 1 && !unmaps);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\napple_dext_client_free_dma_buffer(")
    end = source.index("\nkern_return_t\napple_dext_client_unregister_dma(",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "bounce_client.c"
        binary = pathlib.Path(directory) / "bounce_client"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
