#!/usr/bin/env python3
"""Verify fixed-size ReBAR entries match the actual Darwin BAR apertures."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
PCI = ROOT / "hw/vfio/pci.c"
APPLE = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define PCI_ROM_SLOT 6
#define PCI_REBAR_CAP 4
#define PCI_REBAR_CAP_SIZES 0xfffffff0
#define PCI_REBAR_CTRL 8
#define PCI_REBAR_CTRL_BAR_IDX 7
#define PCI_REBAR_CTRL_NBAR_MASK 0xe0
#define PCI_REBAR_CTRL_NBAR_SHIFT 5
#define PCI_REBAR_CTRL_BAR_SIZE 0x1f00
#define PCI_REBAR_CTRL_BAR_SHIFT 8
#define warn_report(...) ((void)0)
#define LOG_GUEST_ERROR 1
#define qemu_log_mask(...) ((void)0)
typedef struct PCIDevice { uint8_t config[4096]; } PCIDevice;
typedef struct VFIODevice VFIODevice;
typedef struct VFIODeviceIOOps {
    bool (*validate_rebar)(VFIODevice *, unsigned int, unsigned int);
} VFIODeviceIOOps;
struct VFIODevice { void *dev; VFIODeviceIOOps *io_ops; };
typedef struct VFIOBAR {
    struct { uint64_t size; } region;
    size_t size;
    bool ioport;
} VFIOBAR;
typedef struct VFIOPCIDevice {
    PCIDevice parent;
    VFIODevice vbasedev;
    unsigned config_size;
    VFIOBAR bars[6];
} VFIOPCIDevice;
typedef struct VFIOApplePCIDevice { VFIOPCIDevice parent; } VFIOApplePCIDevice;
#define PCI_DEVICE(v) (&(v)->parent)
#define VFIO_APPLE_PCI(p) ((VFIOApplePCIDevice *)(p))
#define VFIO_PCI_DEVICE(p) (&(p)->parent)
static int emulated;
static uint32_t pci_get_long(const uint8_t *p)
{ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t value)
{ p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24; }
static void vfio_add_emulated_long(VFIOPCIDevice *v, int pos, uint32_t val,
                                    uint32_t mask)
{ (void)v; (void)pos; (void)val; (void)mask; emulated++; }
"""

SUFFIX = r"""
int main(void)
{
    VFIOApplePCIDevice a = {0};
    VFIOPCIDevice *v = &a.parent;
    VFIODeviceIOOps ops = { .validate_rebar = apple_vfio_validate_rebar };
    v->vbasedev.dev = &a;
    v->vbasedev.io_ops = &ops;
    v->config_size = 4096;
    v->bars[0].size = v->bars[0].region.size = 256ULL << 20;
    put32(v->parent.config + 0x108, (1 << 5) | (8 << 8));
    assert(!vfio_setup_rebar_ecap(v, 0x100) && emulated == 2);
    emulated = 0;
    v->bars[0].region.size = 128ULL << 20;
    assert(vfio_setup_rebar_ecap(v, 0x100) == -EINVAL && !emulated);
    v->bars[0].region.size = 256ULL << 20;
    put32(v->parent.config + 0x108, (2 << 5) | (8 << 8));
    put32(v->parent.config + 0x110, (1 | (8 << 8)));
    assert(vfio_setup_rebar_ecap(v, 0x100) == -EINVAL && !emulated);
    v->bars[1].size = v->bars[1].region.size = 256ULL << 20;
    assert(!vfio_setup_rebar_ecap(v, 0x100) && emulated == 4);
    emulated = 0;
    v->bars[1].ioport = true;
    assert(vfio_setup_rebar_ecap(v, 0x100) == -EINVAL && !emulated);
    put32(v->parent.config + 0x108, (1 << 5) | (7 | (8 << 8)));
    assert(vfio_setup_rebar_ecap(v, 0x100) == -EINVAL && !emulated);
    put32(v->parent.config + 0x108, 8 << 8);
    assert(vfio_setup_rebar_ecap(v, 0x100) == -EINVAL && !emulated);
}
"""

def main():
    pci = PCI.read_text()
    apple = APPLE.read_text()
    astart = apple.index("static bool apple_vfio_validate_rebar(")
    aend = apple.index("static int apple_vfio_device_feature(", astart)
    pstart = pci.index("static int vfio_setup_rebar_ecap(")
    pend = pci.index("\n/*", pstart)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + apple[astart:aend] + pci[pstart:pend] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
