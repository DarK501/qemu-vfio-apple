#!/usr/bin/env python3
"""Exercise the DriverKit claim, release, and close client calls."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dext-client.c"

PREFIX = r"""
#include <assert.h>
#include <stddef.h>
typedef int kern_return_t;
typedef int io_connect_t;
enum { IO_OBJECT_NULL = 0, kIOReturnBadArgument = 1,
       kSelectorClaim = 1, kSelectorReleaseClaim = 20 };
static int selector_seen, calls, closes;
static kern_return_t IOConnectCallMethod(io_connect_t connection, int selector,
    const void *input, int input_count, const void *input_struct,
    int input_size, void *output, void *output_count,
    void *output_struct, void *output_size)
{
    assert(connection == 7 && !input && !input_count && !input_struct &&
           !input_size && !output && !output_count && !output_struct &&
           !output_size);
    selector_seen = selector;
    calls++;
    return 42;
}
static kern_return_t IOServiceClose(io_connect_t connection)
{
    assert(connection == 7);
    closes++;
    return 43;
}
"""

SUFFIX = r"""
int main(void)
{
    assert(apple_dext_client_claim(0) == kIOReturnBadArgument);
    assert(apple_dext_client_release_claim(0) == kIOReturnBadArgument);
    assert(apple_dext_client_disconnect(0) == kIOReturnBadArgument);
    assert(!calls && !closes);
    assert(apple_dext_client_claim(7) == 42 && selector_seen == 1);
    assert(apple_dext_client_release_claim(7) == 42 && selector_seen == 20);
    assert(apple_dext_client_disconnect(7) == 43);
    assert(calls == 2 && closes == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\napple_dext_client_disconnect(")
    end = source.index("\nkern_return_t\napple_dext_client_register_dma(", start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "claim_client.c"
        binary = pathlib.Path(directory) / "claim_client"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
