#!/usr/bin/env python3
"""Exercise the Darwin config filter with the production implementation."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#define PCI_STATUS 6
#define PCI_BASE_ADDRESS_0 16
#define PCI_BASE_ADDRESS_5 36
#define PCI_COMMAND 4
#define PCI_CONFIG_SPACE_SIZE 256
#define PCIE_CONFIG_SPACE_SIZE 4096
#define PCI_NUM_REGIONS 7
#define PCI_REBAR_CAP 4
#define PCI_REBAR_CTRL 8
#define PCI_REBAR_CTRL_NBAR_MASK 0xe0
#define PCI_REBAR_CTRL_NBAR_SHIFT 5
#define PCI_EXT_CAP_ID_REBAR 0x15
#define PCI_EXT_CAP_ID(h) ((h) & 0xffff)
#define PCI_EXT_CAP_NEXT(h) (((h) >> 20) & 0xffc)
#define PCI_EXP_DEVCTL 8
#define PCI_EXP_DEVCTL2 40
#define MSIX_CAP_LENGTH 12
#define QEMU_PCI_CAP_MSI 1
#define QEMU_PCI_CAP_MSIX 2
#define QEMU_PCI_CAP_EXPRESS 4
typedef struct PCIDevice {
    unsigned cap_present, msi_cap, msix_cap;
    struct { unsigned exp_cap; } exp;
    uint8_t config[PCIE_CONFIG_SPACE_SIZE];
    bool express;
} PCIDevice;
typedef struct VFIOPCIDevice {
    PCIDevice parent;
    unsigned msi_cap_size;
} VFIOPCIDevice;
typedef struct AppleVFIOState {
    bool rebar_scan_valid;
    uint16_t physical_rebar_offset, physical_rebar_length;
} AppleVFIOState;
typedef struct VFIOApplePCIDevice {
    VFIOPCIDevice parent;
    AppleVFIOState *apple;
} VFIOApplePCIDevice;
#define PCI_DEVICE(v) (&(v)->parent)
#define VFIO_APPLE_PCI(p) ((VFIOApplePCIDevice *)(p))
static bool ranges_overlap(off_t a, uint32_t n, off_t b, uint32_t m)
{ return a < b + m && b < a + n; }
static uint32_t pci_get_long(const uint8_t *p)
{ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
"""

SUFFIX = r"""
int main(void)
{
    AppleVFIOState apple = {0};
    VFIOApplePCIDevice av = { .apple = &apple };
    VFIOPCIDevice *v = &av.parent;
    PCIDevice *p = &v->parent;
    assert(!apple_vfio_config_write_is_safe(v, PCI_BASE_ADDRESS_0, 4));
    assert(apple_vfio_config_write_is_safe(v, 0x80, 4));
    assert(!apple_vfio_config_write_is_safe(v, 0x200, 4));
    p->cap_present = QEMU_PCI_CAP_MSI | QEMU_PCI_CAP_MSIX;
    p->msi_cap = 0x40; p->msix_cap = 0x60; v->msi_cap_size = 24;
    assert(!apple_vfio_config_write_is_safe(v, 0x40, 4));
    assert(!apple_vfio_config_write_is_safe(v, 0x5f, 2));
    assert(!apple_vfio_config_write_is_safe(v, 0x68, 4));
    assert(apple_vfio_config_write_is_safe(v, 0x6c, 4));
    apple_vfio_cache_physical_rebar(&apple, p->config);
    assert(apple.rebar_scan_valid);
    assert(apple_vfio_config_write_is_safe(v, 0x100, 4));
    assert(apple_vfio_config_write_is_safe(v, 0x200, 4));
    p->config[0x100] = PCI_EXT_CAP_ID_REBAR;
    p->config[0x108] = 2 << PCI_REBAR_CTRL_NBAR_SHIFT;
    apple_vfio_cache_physical_rebar(&apple, p->config);
    assert(apple.rebar_scan_valid);
    assert(!apple_vfio_config_write_is_safe(v, 0x100, 4));
    assert(!apple_vfio_config_write_is_safe(v, 0x103, 2));
    assert(!apple_vfio_config_write_is_safe(v, 0x108, 4));
    assert(!apple_vfio_config_write_is_safe(v, 0x110, 4));
    assert(apple_vfio_config_write_is_safe(v, 0x114, 4));
    p->config[0x108] = 0;
    apple_vfio_cache_physical_rebar(&apple, p->config);
    assert(!apple.rebar_scan_valid);
    assert(!apple_vfio_config_write_is_safe(v, 0x200, 4));
    memset(p->config, 0, sizeof(p->config));
    p->config[0x100] = 1;
    p->config[0x102] = 0x00;
    p->config[0x103] = 0x10; /* next points back to 0x100 */
    apple_vfio_cache_physical_rebar(&apple, p->config);
    assert(!apple.rebar_scan_valid);
    assert(!apple_vfio_config_write_is_safe(v, 0x200, 4));
}
"""

def main():
    source = SOURCE.read_text()
    cache_start = source.index("static void apple_vfio_cache_physical_rebar(")
    cache_end = source.index("static int apple_vfio_region_read(", cache_start)
    start = source.index("static bool apple_vfio_config_write_is_safe(")
    end = source.index("static int apple_vfio_forward_command_write(", start)
    with tempfile.TemporaryDirectory() as tmp:
        cfile = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        cfile.write_text(PREFIX + source[cache_start:cache_end] +
                         source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
