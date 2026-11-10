#!/usr/bin/env python3
"""Verify unsupported map contexts fail before invoking the original DMA ops."""

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
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef uint64_t u64;
enum dma_data_direction { DMA_TO_DEVICE };
#define DMA_MAPPING_ERROR UINT64_MAX
#define APPLE_DMA_STAT_MAP 0
#define dev_info(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
struct device { int unused; };
struct dma_map_ops {
    dma_addr_t (*map_phys)(struct device *, phys_addr_t, size_t,
                           enum dma_data_direction, unsigned long);
    void (*unmap_phys)(struct device *, dma_addr_t, size_t,
                       enum dma_data_direction, unsigned long);
};
struct apple_dma_dev { bool ready; struct dma_map_ops *orig_dma_ops; };
static bool atomic_context, disabled_irqs;
static int originals;
static struct apple_dma_dev active;
static bool in_atomic(void) { return atomic_context; }
static bool irqs_disabled(void) { return disabled_irqs; }
static struct apple_dma_dev *apple_dma_find_by_dev(struct device *d)
{ (void)d; return &active; }
static unsigned apple_dma_trace_stat_inc(int id) { (void)id; return 0; }
static bool apple_dma_trace_should_log(unsigned c) { (void)c; return false; }
static dma_addr_t apple_dma_orig_map_phys(struct apple_dma_dev *ad,
    struct device *dev, phys_addr_t phys, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{ (void)ad; (void)dev; (void)size; (void)dir; (void)attrs; originals++; return phys; }
static void apple_dma_orig_unmap_phys(struct apple_dma_dev *ad,
    struct device *dev, dma_addr_t addr, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{ (void)ad; (void)dev; (void)addr; (void)size; (void)dir; (void)attrs; }
static bool dma_mapping_error(struct device *dev, dma_addr_t addr)
{ (void)dev; return addr == DMA_MAPPING_ERROR; }
static u64 dma_get_mask(struct device *dev) { (void)dev; return UINT64_MAX; }
static int apple_dma_map_one(struct apple_dma_dev *ad, struct device *dev,
    phys_addr_t phys, size_t size, dma_addr_t orig, u64 mask, dma_addr_t *out)
{ (void)ad; (void)dev; (void)size; (void)orig; assert(mask == UINT64_MAX);
  *out = phys + 4096; return 0; }
"""

SUFFIX = r"""
int main(void)
{
    struct device dev = {0};
    active.ready = true;
    atomic_context = true;
    assert(apple_dma_map_phys(&dev, 0x1000, 4096, DMA_TO_DEVICE, 0)
           == DMA_MAPPING_ERROR && !originals);
    atomic_context = false; disabled_irqs = true;
    assert(apple_dma_map_phys(&dev, 0x1000, 4096, DMA_TO_DEVICE, 0)
           == DMA_MAPPING_ERROR && !originals);
    disabled_irqs = false; active.ready = false;
    assert(apple_dma_map_phys(&dev, 0x1000, 4096, DMA_TO_DEVICE, 0)
           == DMA_MAPPING_ERROR && !originals);
    active.ready = true;
    assert(apple_dma_map_phys(&dev, 0x1000, 4096, DMA_TO_DEVICE, 0)
           == 0x2000 && originals == 1);
}
"""

def main():
    source = SOURCE.read_text()
    start = source.index("static bool apple_dma_usable(")
    end = source.index("static __noreturn void apple_dma_panic_state(", start)
    mstart = source.index("static dma_addr_t apple_dma_map_phys(")
    mend = source.index("static void apple_dma_unmap_phys(", mstart)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + source[mstart:mend] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
