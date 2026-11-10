#!/usr/bin/env python3
"""Verify coherent DMA alignment and cleanup using the guest implementation."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint64_t dma_addr_t;
typedef uint64_t phys_addr_t;
typedef uint64_t u64;
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096UL
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define APPLE_DMA_TRACE_OP_FREE 1
#define dev_warn(...) ((void)0)
struct device { uint64_t coherent_dma_mask; };
struct apple_dma_dev { int unused; };
static dma_addr_t result;
static int maps, unmaps, map_error;
static u64 seen_mask;
static unsigned int get_order(size_t size)
{
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned int order = 0;
    size_t n = 1;
    while (n < pages) { n <<= 1; order++; }
    return order;
}
static int apple_dma_map_one(struct apple_dma_dev *ad, struct device *dev,
    phys_addr_t phys, size_t size, dma_addr_t orig, u64 mask, dma_addr_t *out)
{
    (void)ad; (void)dev; (void)phys; (void)size; (void)orig;
    maps++;
    seen_mask = mask;
    *out = result;
    return map_error;
}
static dma_addr_t apple_dma_unmap_one(struct apple_dma_dev *ad,
    struct device *dev, dma_addr_t addr, size_t size, int op)
{
    (void)ad; (void)dev; (void)size;
    assert(addr == result && op == APPLE_DMA_TRACE_OP_FREE);
    unmaps++;
    return 0;
}
"""

SUFFIX = r"""
int main(void)
{
    struct apple_dma_dev ad = {0};
    struct device dev = { .coherent_dma_mask = 0xffff };
    dma_addr_t out = 0;
    result = 0x4000;
    assert(!apple_dma_remap_alloc(&ad, &dev, 0, 5000, 0, &out, true));
    assert(out == result && maps == 1 && unmaps == 0 && seen_mask == 0xffff);
    result = 0x3000;
    assert(apple_dma_remap_alloc(&ad, &dev, 0, 5000, 0, &out, true) == -ERANGE);
    assert(unmaps == 1);
    assert(!apple_dma_remap_alloc(&ad, &dev, 0, 5000, 0, &out, false));
    assert(seen_mask == dev.coherent_dma_mask);
    result = 0xf000;
    assert(apple_dma_remap_alloc(&ad, &dev, 0, 8192, 0, &out, true) == -ERANGE);
    assert(unmaps == 2);
    result = 0x1000;
    assert(apple_dma_remap_alloc(&ad, &dev, 0, 1, 0, &out, true) == 0);
    assert(apple_dma_remap_alloc(&ad, &dev, 0, 0, 0, &out, true) == -EINVAL);
    map_error = -EIO;
    assert(apple_dma_remap_alloc(&ad, &dev, 0, 1, 0, &out, true) == -EIO);
    assert(unmaps == 2);
}
"""

def main():
    source = SOURCE.read_text()
    start = source.index("static int apple_dma_remap_alloc(")
    end = source.index("static void *apple_dma_alloc(", start)
    with tempfile.TemporaryDirectory() as tmp:
        cfile = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        cfile.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
