#!/usr/bin/env python3
"""Verify x86 DMA delegation keeps the active per-device ops unchanged."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define CONFIG_X86 1
#define APPLE_DMA_HAS_MAP_PHYS 1
typedef uint64_t dma_addr_t;
typedef uint64_t phys_addr_t;
typedef unsigned int gfp_t;
enum dma_data_direction { DMA_TO_DEVICE, DMA_FROM_DEVICE };
struct dma_map_ops { int id; };
struct device { const struct dma_map_ops *dma_ops; };
struct apple_dma_dev { const struct dma_map_ops *orig_dma_ops; };
struct page { int id; };
struct scatterlist { int id; };
static struct dma_map_ops original_ops, active_ops;
static const struct dma_map_ops *expected_ops = &original_ops;
static struct device tested_device = { .dma_ops = &active_ops };
static struct page allocated_page;
static int dispatches;

static void check_dispatch(struct device *dev, const struct dma_map_ops *ops)
{
    assert(dev == &tested_device && ops == expected_ops);
    assert(dev->dma_ops == &active_ops);
    dispatches++;
}

static dma_addr_t dma_map_phys_with_ops(struct device *dev,
    const struct dma_map_ops *ops, phys_addr_t phys, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
    check_dispatch(dev, ops);
    assert(phys == 0x1000 && size == 128 && dir == DMA_TO_DEVICE && attrs == 5);
    return 0x101;
}
static void dma_unmap_phys_with_ops(struct device *dev,
    const struct dma_map_ops *ops, dma_addr_t addr, size_t size,
    enum dma_data_direction dir, unsigned long attrs)
{
    check_dispatch(dev, ops);
    assert(addr == 0x101 && size == 128 && dir == DMA_TO_DEVICE && attrs == 5);
}
static void dma_unmap_sg_attrs_with_ops(struct device *dev,
    const struct dma_map_ops *ops, struct scatterlist *sg, int nents,
    enum dma_data_direction dir, unsigned long attrs)
{
    check_dispatch(dev, ops);
    assert(sg->id == 2 && nents == 3 && dir == DMA_TO_DEVICE && attrs == 5);
}
static void *dma_alloc_attrs_with_ops(struct device *dev,
    const struct dma_map_ops *ops, size_t size, dma_addr_t *dma_handle,
    gfp_t gfp, unsigned long attrs)
{
    check_dispatch(dev, ops);
    assert(size == 128 && gfp == 6 && attrs == 5);
    *dma_handle = 0x103;
    return (void *)(uintptr_t)0x1234;
}
static void dma_free_attrs_with_ops(struct device *dev,
    const struct dma_map_ops *ops, size_t size, void *cpu_addr,
    dma_addr_t dma_handle, unsigned long attrs)
{
    check_dispatch(dev, ops);
    assert(size == 128 && cpu_addr == (void *)(uintptr_t)0x1234);
    assert(dma_handle == 0x103 && attrs == 5);
}
static struct page *dma_alloc_pages_with_ops(struct device *dev,
    const struct dma_map_ops *ops, size_t size, dma_addr_t *dma_handle,
    enum dma_data_direction dir, gfp_t gfp)
{
    check_dispatch(dev, ops);
    assert(size == 128 && dir == DMA_TO_DEVICE && gfp == 6);
    *dma_handle = 0x104;
    return &allocated_page;
}
static void dma_free_pages_with_ops(struct device *dev,
    const struct dma_map_ops *ops, size_t size, struct page *page,
    dma_addr_t dma_handle, enum dma_data_direction dir)
{
    check_dispatch(dev, ops);
    assert(size == 128 && page == &allocated_page && dma_handle == 0x104);
    assert(dir == DMA_TO_DEVICE);
}
"""

SUFFIX = r"""
int main(void)
{
    struct apple_dma_dev owner = { .orig_dma_ops = &original_ops };
    struct scatterlist sg = { .id = 2 };
    dma_addr_t handle = 0;

    assert(apple_dma_orig_map_phys(&owner, &tested_device, 0x1000, 128,
                                   DMA_TO_DEVICE, 5) == 0x101);
    apple_dma_orig_unmap_phys(&owner, &tested_device, 0x101, 128,
                              DMA_TO_DEVICE, 5);
    apple_dma_orig_unmap_sg(&owner, &tested_device, &sg, 3,
                            DMA_TO_DEVICE, 5);
    assert(apple_dma_orig_alloc_attrs(&owner, &tested_device, 128, &handle,
                                      6, 5) == (void *)(uintptr_t)0x1234);
    assert(handle == 0x103);
    apple_dma_orig_free_attrs(&owner, &tested_device, 128,
                              (void *)(uintptr_t)0x1234, 0x103, 5);
    assert(apple_dma_orig_alloc_pages(&owner, &tested_device, 128, &handle,
                                      DMA_TO_DEVICE, 6) == &allocated_page);
    assert(handle == 0x104);
    apple_dma_orig_free_pages(&owner, &tested_device, 128, &allocated_page,
                              0x104, DMA_TO_DEVICE);
    assert(dispatches == 7 && tested_device.dma_ops == &active_ops);

    owner.orig_dma_ops = NULL;
    expected_ops = NULL;
    assert(apple_dma_orig_map_phys(&owner, &tested_device, 0x1000, 128,
                                   DMA_TO_DEVICE, 5) == 0x101);
    apple_dma_orig_unmap_phys(&owner, &tested_device, 0x101, 128,
                              DMA_TO_DEVICE, 5);
    apple_dma_orig_unmap_sg(&owner, &tested_device, &sg, 3,
                            DMA_TO_DEVICE, 5);
    assert(apple_dma_orig_alloc_attrs(&owner, &tested_device, 128, &handle,
                                      6, 5) == (void *)(uintptr_t)0x1234);
    apple_dma_orig_free_attrs(&owner, &tested_device, 128,
                              (void *)(uintptr_t)0x1234, 0x103, 5);
    assert(apple_dma_orig_alloc_pages(&owner, &tested_device, 128, &handle,
                                      DMA_TO_DEVICE, 6) == &allocated_page);
    apple_dma_orig_free_pages(&owner, &tested_device, 128, &allocated_page,
                              0x104, DMA_TO_DEVICE);
    assert(dispatches == 14 && tested_device.dma_ops == &active_ops);
}
"""

ARM_PREFIX = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t dma_addr_t;
typedef unsigned int gfp_t;
enum dma_data_direction { DMA_TO_DEVICE, DMA_FROM_DEVICE };
struct dma_map_ops { int id; };
struct device { const struct dma_map_ops *dma_ops; };
struct apple_dma_dev { const struct dma_map_ops *orig_dma_ops; };
struct page { int id; };
struct scatterlist { int id; };
static struct dma_map_ops original_ops, active_ops;
static struct device tested_device = { .dma_ops = &active_ops };
static struct page allocated_page;
static int calls;
static const struct dma_map_ops *get_dma_ops(struct device *dev)
{
    return dev->dma_ops;
}
static void set_dma_ops(struct device *dev, const struct dma_map_ops *ops)
{
    dev->dma_ops = ops;
}
static void check_original(struct device *dev)
{
    assert(dev == &tested_device && dev->dma_ops == &original_ops);
    calls++;
}
static dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
    unsigned long offset, size_t size, enum dma_data_direction dir,
    unsigned long attrs)
{
    check_original(dev);
    assert(page->id == 1 && offset == 8 && size == 128);
    assert(dir == DMA_TO_DEVICE && attrs == 5);
    return 0x201;
}
static void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr,
    size_t size, enum dma_data_direction dir, unsigned long attrs)
{
    check_original(dev);
    assert(addr == 0x201 && size == 128);
    assert(dir == DMA_TO_DEVICE && attrs == 5);
}
static void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg,
    int nents, enum dma_data_direction dir, unsigned long attrs)
{
    check_original(dev);
    assert(sg->id == 2 && nents == 3);
    assert(dir == DMA_TO_DEVICE && attrs == 5);
}
static void *dma_alloc_attrs(struct device *dev, size_t size,
    dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
    check_original(dev);
    assert(size == 128 && gfp == 6 && attrs == 5);
    *dma_handle = 0x203;
    return (void *)(uintptr_t)0x2234;
}
static void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
    dma_addr_t dma_handle, unsigned long attrs)
{
    check_original(dev);
    assert(size == 128 && cpu_addr == (void *)(uintptr_t)0x2234);
    assert(dma_handle == 0x203 && attrs == 5);
}
static struct page *dma_alloc_pages(struct device *dev, size_t size,
    dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp)
{
    check_original(dev);
    assert(size == 128 && dir == DMA_TO_DEVICE && gfp == 6);
    *dma_handle = 0x204;
    return &allocated_page;
}
static void dma_free_pages(struct device *dev, size_t size, struct page *page,
    dma_addr_t dma_handle, enum dma_data_direction dir)
{
    check_original(dev);
    assert(size == 128 && page == &allocated_page && dma_handle == 0x204);
    assert(dir == DMA_TO_DEVICE);
}
"""

ARM_SUFFIX = r"""
int main(void)
{
    struct apple_dma_dev owner = { .orig_dma_ops = &original_ops };
    struct page page = { .id = 1 };
    struct scatterlist sg = { .id = 2 };
    dma_addr_t handle;

    assert(apple_dma_orig_map_page(&owner, &tested_device, &page, 8, 128,
                                   DMA_TO_DEVICE, 5) == 0x201);
    apple_dma_orig_unmap_page(&owner, &tested_device, 0x201, 128,
                              DMA_TO_DEVICE, 5);
    apple_dma_orig_unmap_sg(&owner, &tested_device, &sg, 3,
                            DMA_TO_DEVICE, 5);
    assert(apple_dma_orig_alloc_attrs(&owner, &tested_device, 128, &handle,
                                      6, 5) == (void *)(uintptr_t)0x2234);
    assert(handle == 0x203);
    apple_dma_orig_free_attrs(&owner, &tested_device, 128,
                              (void *)(uintptr_t)0x2234, 0x203, 5);
    assert(apple_dma_orig_alloc_pages(&owner, &tested_device, 128, &handle,
                                      DMA_TO_DEVICE, 6) == &allocated_page);
    assert(handle == 0x204);
    apple_dma_orig_free_pages(&owner, &tested_device, 128, &allocated_page,
                              0x204, DMA_TO_DEVICE);
    assert(calls == 7 && tested_device.dma_ops == &active_ops);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index(
        "#ifndef CONFIG_X86\n"
        "#define APPLE_DMA_CALL_ORIG_RET(")
    end = source.index("\n/* ------------------------------------------------------------------ */", start)
    body = source[start:end]
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "dma_ops_dispatch.c"
        binary = pathlib.Path(directory) / "dma_ops_dispatch"
        test.write_text(PREFIX + body + SUFFIX)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        test.write_text(ARM_PREFIX + body + ARM_SUFFIX)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()