#!/usr/bin/env python3
"""Verify the guest retains page offsets and rejects DMA range overflow."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef uint64_t u64;
#define PAGE_SIZE 4096UL
#define DMA_MAPPING_ERROR UINT64_MAX
"""

SUFFIX = r"""
int main(void)
{
    dma_addr_t out = 0;
    assert(!apple_dma_window_address(0x8000, 0x1000, 0x1123, 0x100,
                                     0xffff, &out));
    assert(out == 0x8123);
    assert(apple_dma_window_address(0x8001, 0x1000, 0x1123, 0x100,
                                    0xffff, &out) == -ERANGE);
    assert(apple_dma_window_address(0x8000, 0x1000, 0x0fff, 1,
                                    UINT64_MAX, &out) == -ERANGE);
    assert(apple_dma_window_address(UINT64_MAX - 0xfff, 0x1000,
                                    0x2000, 1, UINT64_MAX, &out) == -ERANGE);
    assert(apple_dma_window_address(UINT64_MAX - 0xfff, 0x1000,
                                    0x1000, 4097, UINT64_MAX, &out) == -ERANGE);
    assert(apple_dma_window_address(0xf000, 0x1000, 0x1000,
                                    4097, 0xffff, &out) == -ERANGE);
    assert(apple_dma_window_address(0x8000, 0x1000, 0x1000,
                                    0, UINT64_MAX, &out) == -ERANGE);
}
"""

def main():
    source = SOURCE.read_text()
    start = source.index("static int apple_dma_window_address(")
    end = source.index("\n/*", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
