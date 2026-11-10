#!/usr/bin/env python3
"""Verify invalid replacement DMA addresses unwind the actual map path."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <setjmp.h>
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef uint64_t u64;
#define PAGE_SIZE 4096UL
#define U32_MAX UINT32_MAX
#define DMA_MAPPING_ERROR UINT64_MAX
#define dev_warn_ratelimited(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))
#define kfree free
#define IS_ERR(p) (false)
#define PTR_ERR(p) (-EIO)
struct device { u64 mask; };
struct dma_window { dma_addr_t dma_addr; size_t size; unsigned int refcount; };
struct apple_dma_dev { int unused; };
struct apple_dma_map_entry { int unused; };
static struct dma_window window;
static bool window_exists;
static int created, released, published;
static bool conflict;
static jmp_buf panic_env;
static void apple_dma_panic_state(struct device *dev, const char *reason)
{ (void)dev; (void)reason; longjmp(panic_env, 1); }
static struct apple_dma_map_entry *apple_dma_alloc_entry(void)
{ return calloc(1, sizeof(struct apple_dma_map_entry)); }
static void wnd_shape_for(phys_addr_t gpa, size_t size,
                          phys_addr_t *base, size_t *window_size)
{ *base = gpa & ~(PAGE_SIZE - 1); *window_size = size + (gpa - *base); }
static struct dma_window *wnd_lookup(struct apple_dma_dev *ad,
                                     phys_addr_t base, size_t size)
{ (void)ad; (void)base; (void)size; return window_exists ? &window : NULL; }
static struct dma_window *wnd_create(struct apple_dma_dev *ad,
                                     phys_addr_t base, size_t size)
{ (void)ad; (void)base; window.size = size; created++; window_exists = true; return &window; }
static void wnd_put(struct apple_dma_dev *ad, struct dma_window *w)
{ (void)ad; assert(w->refcount); w->refcount--; released++; }
static bool apple_dma_range_conflicts(struct apple_dma_dev *ad, u64 address,
                                      u64 size, struct dma_window *w)
{ (void)ad; (void)address; (void)size; (void)w; return conflict; }
static bool apple_dma_publish_entry(struct apple_dma_dev *ad,
                                    struct apple_dma_map_entry *e,
                                    dma_addr_t dma, dma_addr_t orig,
                                    size_t size, struct dma_window *w)
{ (void)ad; (void)dma; (void)orig; (void)size; (void)w;
  if (conflict) return false;
  published++; free(e); return true; }
"""

SUFFIX = r"""
int main(void)
{
    struct apple_dma_dev ad = {0};
    struct device dev = { .mask = UINT64_MAX };
    dma_addr_t out = 0;
    window.dma_addr = 0x8001;
    assert(apple_dma_map_locked(&ad, &dev, 0x1123, 32, 0x1123, dev.mask, &out) == -ERANGE);
    assert(created == 1 && released == 1 && !published && !window.refcount);
    window.dma_addr = 0x8000;
    assert(!apple_dma_map_locked(&ad, &dev, 0x1123, 32, 0x1123, dev.mask, &out));
    assert(created == 1 && published == 1 && window.refcount == 1 && out == 0x8123);
    dev.mask = 0x8000;
    assert(apple_dma_map_locked(&ad, &dev, 0x1123, 32, 0x1123, dev.mask, &out) == -ERANGE);
    assert(released == 2 && published == 1 && window.refcount == 1);
    /* A different mask for another API may validly reuse this window. */
    assert(!apple_dma_map_locked(&ad, &dev, 0x1123, 32, 0x1123, 0xffff, &out));
    assert(published == 2 && window.refcount == 2 && out == 0x8123);
    conflict = true;
    if (!setjmp(panic_env)) {
        apple_dma_map_locked(&ad, &dev, 0x1123, 32, 0x1123, 0xffff, &out);
        assert(!"overlapping mapping ownership must be fatal");
    }
    assert(released == 2 && published == 2 && window.refcount == 3);
}
"""

def main():
    source = SOURCE.read_text()
    addr_start = source.index("static int apple_dma_window_address(")
    addr_end = source.index("\n/*", addr_start)
    map_start = source.index("static int apple_dma_map_locked(")
    map_end = source.index("\nstatic int apple_dma_map_one(", map_start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[addr_start:addr_end] +
                        source[map_start:map_end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
