#!/usr/bin/env python3
"""Exercise Darwin interrupt availability and mask bounds."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IO_OBJECT_NULL 0
#define KERN_SUCCESS 0
#define VFIO_IRQ_INFO_EVENTFD 1
#define VFIO_IRQ_INFO_NORESIZE 2
#define error_report(...) ((void)0)
enum { VFIO_PCI_MSI_IRQ_INDEX, VFIO_PCI_MSIX_IRQ_INDEX,
       VFIO_PCI_INTX_IRQ_INDEX, VFIO_PCI_ERR_IRQ_INDEX, VFIO_PCI_REQ_IRQ_INDEX };
enum { VFIO_INT_NONE, VFIO_INT_MSI, VFIO_INT_MSIX, VFIO_INT_INTx };
typedef int io_connect_t;
typedef struct { unsigned entries; } MSIX;
typedef struct { int use; } Vector;
typedef struct { int interrupt; unsigned nr_vectors; Vector *msi_vectors;
                 MSIX *msix; } VFIOPCIDevice;
typedef struct { unsigned num_irq_vectors; } AppleVFIOState;
typedef struct { VFIOPCIDevice parent; AppleVFIOState *apple; } VFIOApplePCIDevice;
typedef struct { void *dev; } VFIODevice;
struct vfio_irq_info { unsigned index, flags, count; };
struct vfio_irq_set { int unused; };
#define VFIO_APPLE_PCI(p) ((VFIOApplePCIDevice *)(p))
#define VFIO_PCI_DEVICE(p) (&(p)->parent)
static int conn = 1, set_result, calls;
static uint64_t actual[4];
static int apple_vfio_connection(VFIODevice *v) { (void)v; return conn; }
static int set_mask(int connection, const uint64_t *mask)
{
    assert(connection == 1);
    calls++;
    memcpy(actual, mask, sizeof(actual));
    return set_result;
}
static const struct Client { int (*set_irq_mask)(int, const uint64_t *); }
    client = { set_mask };
static const struct Backend { const struct Client *client; }
    backend = { &client };
static const struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
int main(void)
{
    Vector vectors[257] = {0};
    MSIX msix = { .entries = 257 };
    AppleVFIOState apple = { .num_irq_vectors = 2 };
    VFIOApplePCIDevice av = { .apple = &apple };
    VFIODevice base = { .dev = &av };
    struct vfio_irq_info info = { .index = VFIO_PCI_MSIX_IRQ_INDEX };
    struct vfio_irq_set irq = {0};
    av.parent.msix = &msix;
    av.parent.msi_vectors = vectors;
    assert(!apple_vfio_get_irq_info(&base, &info) && info.count == 2);
    av.parent.interrupt = VFIO_INT_MSIX;
    av.parent.nr_vectors = 2;
    vectors[1].use = 1;
    assert(!apple_vfio_set_irqs(&base, &irq));
    assert(calls == 1 && actual[0] == 2);
    av.parent.nr_vectors = 3;
    assert(apple_vfio_set_irqs(&base, &irq) == -EINVAL && calls == 1);
    apple.num_irq_vectors = 257;
    assert(!apple_vfio_get_irq_info(&base, &info) && info.count == 256);
    av.parent.nr_vectors = 257;
    assert(apple_vfio_set_irqs(&base, &irq) == -EINVAL && calls == 1);
    av.parent.nr_vectors = 256;
    vectors[255].use = 1;
    assert(!apple_vfio_set_irqs(&base, &irq));
    assert(actual[3] == (UINT64_C(1) << 63));
    set_result = 1;
    assert(apple_vfio_set_irqs(&base, &irq) == -EIO);
    conn = 0;
    assert(apple_vfio_set_irqs(&base, &irq) == -ENODEV);
}
"""

def main():
    source = SOURCE.read_text()
    start = source.index("static int apple_vfio_get_irq_info(")
    end = source.index("static int apple_vfio_bar_read(", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
