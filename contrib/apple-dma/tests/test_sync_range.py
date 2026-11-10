#!/usr/bin/env python3
"""Exercise guest DMA sync range translation and fail-closed handling."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

typedef uint64_t dma_addr_t;
enum dma_data_direction { DMA_TO_DEVICE };
struct device { int unused; };
struct scatterlist;
struct apple_dma_map_entry {
    struct apple_dma_map_entry *next;
    dma_addr_t mapped_dma, orig_dma;
    size_t size;
};
struct apple_dma_dev {
    struct apple_dma_map_entry *maps;
    int maps_lock;
    bool ready;
};
#define list_for_each_entry(e, head, member) \
    for ((e) = *(head); (e); (e) = (e)->next)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)

static jmp_buf panic_env;
static void apple_dma_panic_state(struct device *dev, const char *reason)
{ (void)dev; (void)reason; longjmp(panic_env, 1); }
"""

SUFFIX = r"""
static dma_addr_t synced_addr;
static size_t synced_size;
static int sync_calls;
static void sync_original(struct device *dev, dma_addr_t addr, size_t size,
                          enum dma_data_direction dir)
{
    (void)dev;
    assert(dir == DMA_TO_DEVICE);
    synced_addr = addr;
    synced_size = size;
    sync_calls++;
}

int main(void)
{
    struct apple_dma_map_entry first = {
        .mapped_dma = 0x8000, .orig_dma = 0x1000, .size = 4096,
    };
    struct apple_dma_map_entry duplicate = first;
    struct apple_dma_dev ad = { .maps = &first, .ready = true };
    struct device dev = {0};
    dma_addr_t orig = 0;

    assert(apple_dma_orig_addr(&ad, 0x8000, 4096, &orig) && orig == 0x1000);
    assert(apple_dma_orig_addr(&ad, 0x8123, 0x40, &orig) && orig == 0x1123);
    assert(apple_dma_orig_addr(&ad, 0x8fff, 1, &orig) && orig == 0x1fff);
    assert(!apple_dma_orig_addr(&ad, 0x8fff, 2, &orig));
    assert(!apple_dma_orig_addr(&ad, 0x9000, 1, &orig));
    assert(!apple_dma_orig_addr(&ad, 0x7fff, 1, &orig));
    assert(!apple_dma_orig_addr(&ad, 0x8000, 0, &orig));

    first.next = &duplicate;
    assert(apple_dma_orig_addr(&ad, 0x8123, 4, &orig) && orig == 0x1123);
    duplicate.orig_dma = 0x3000;
    assert(!apple_dma_orig_addr(&ad, 0x8123, 4, &orig));
    if (!setjmp(panic_env)) {
        apple_dma_sync_single_common(&ad, &dev, 0x8123, 4, DMA_TO_DEVICE,
                                     sync_original);
        assert(!"ambiguous range must fail");
    }
    assert(!sync_calls);
    first.next = NULL;

    first.orig_dma = UINT64_MAX - 1;
    assert(!apple_dma_orig_addr(&ad, 0x8001, 2, &orig));
    assert(!apple_dma_orig_addr(&ad, 0x8002, 1, &orig));
    first.orig_dma = 0x1000;
    apple_dma_sync_single_common(&ad, &dev, 0x8123, 0x40, DMA_TO_DEVICE,
                                 sync_original);
    assert(sync_calls == 1 && synced_addr == 0x1123 && synced_size == 0x40);

    if (!setjmp(panic_env)) {
        apple_dma_sync_single_common(&ad, &dev, 0x8fff, 2, DMA_TO_DEVICE,
                                     sync_original);
        assert(!"untracked range must fail");
    }
    assert(sync_calls == 1);
    ad.ready = false;
    if (!setjmp(panic_env)) {
        apple_dma_sync_single_common(&ad, &dev, 0x8000, 1, DMA_TO_DEVICE,
                                     sync_original);
        assert(!"unready companion must fail");
    }
    assert(sync_calls == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static bool apple_dma_orig_addr(")
    end = source.index("\n/*", start)
    sync_start = source.index("typedef void (*apple_dma_sync_fn_t)")
    sync_end = source.index("static void apple_dma_sync_sg_common(", sync_start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] +
                        source[sync_start:sync_end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
