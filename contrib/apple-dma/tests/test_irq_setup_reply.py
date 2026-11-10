#!/usr/bin/env python3
"""Verify interrupt setup never accepts an absent or invalid vector count."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dext-client.c"

PREFIX = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef int kern_return_t;
typedef int io_connect_t;
enum { KERN_SUCCESS = 0, kIOReturnSuccess = 0, IO_OBJECT_NULL = 0,
       kIOReturnBadArgument = 9, kSelectorSetupInterrupts = 13 };
static uint64_t value;
static uint32_t count = 1;
static int transport;
static kern_return_t IOConnectCallMethod(int conn, int selector,
    const void *input, int input_count, const void *in_struct, int in_size,
    uint64_t *output, uint32_t *output_count, void *out_struct, void *out_size)
{
    assert(conn == 1 && selector == 13 && !input && !input_count && !in_struct &&
           !in_size && *output_count == 1 && !out_struct && !out_size);
    output[0] = value; *output_count = count;
    return transport;
}
"""

SUFFIX = r"""
int main(void)
{
    uint32_t result = 123;
    value = 1;
    assert(!apple_dext_client_setup_interrupts(1, &result) && result == 1);
    value = 256;
    assert(!apple_dext_client_setup_interrupts(1, &result) && result == 256);
    value = 257;
    assert(apple_dext_client_setup_interrupts(1, &result) == kIOReturnBadArgument &&
           result == 256);
    value = 0;
    assert(apple_dext_client_setup_interrupts(1, &result) == kIOReturnBadArgument);
    value = 1; count = 0;
    assert(apple_dext_client_setup_interrupts(1, &result) == kIOReturnBadArgument);
    count = 2;
    assert(apple_dext_client_setup_interrupts(1, &result) == kIOReturnBadArgument);
    count = 1; transport = 7;
    assert(apple_dext_client_setup_interrupts(1, &result) == 7);
    assert(apple_dext_client_setup_interrupts(0, &result) == kIOReturnBadArgument);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\napple_dext_client_setup_interrupts(")
    end = source.index("\nkern_return_t\napple_dext_client_reset_device(", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
