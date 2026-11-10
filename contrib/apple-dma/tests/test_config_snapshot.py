#!/usr/bin/env python3
"""Verify malformed physical PCIe snapshots fail before QEMU parses them."""

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
#include <errno.h>
#include <sys/types.h>
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
#define VFIO_PCI_CONFIG_REGION_INDEX 7
#define IO_OBJECT_NULL 0
#define KERN_SUCCESS 0
#define LOG_GUEST_ERROR 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define error_report(...) do { errors++; } while (0)
#define qemu_log_mask(...) do { guest_logs++; } while (0)
typedef int io_connect_t;
typedef int kern_return_t;
typedef struct AppleVFIOState {
    bool rebar_scan_valid;
    uint16_t physical_rebar_offset, physical_rebar_length;
} AppleVFIOState;
typedef struct VFIOApplePCIDevice { AppleVFIOState *apple; } VFIOApplePCIDevice;
typedef struct VFIODevice { void *dev; } VFIODevice;
#define VFIO_APPLE_PCI(p) ((VFIOApplePCIDevice *)(p))
static unsigned errors, guest_logs;
static uint8_t physical[PCIE_CONFIG_SPACE_SIZE];
static int fail_read;
static uint32_t pci_get_long(const uint8_t *p)
{ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t val)
{ p[0] = val; p[1] = val >> 8; p[2] = val >> 16; p[3] = val >> 24; }
static io_connect_t apple_vfio_connection(VFIODevice *v)
{ (void)v; return 1; }
static int apple_vfio_bar_read(VFIODevice *v, uint8_t nr, off_t off,
                               uint32_t size, void *data)
{ (void)v; (void)nr; (void)off; (void)size; (void)data; return -EINVAL; }
static int config_read(int conn, off_t off, uint32_t size, uint64_t *value)
{ (void)conn; memcpy(value, physical + off, size); return 0; }
static int config_read_block(int conn, off_t off, void *data, uint32_t size)
{
    (void)conn;
    if (fail_read) return 1;
    memcpy(data, physical + off, size);
    return 0;
}
struct Client {
    int (*config_read)(int, off_t, uint32_t, uint64_t *);
    int (*config_read_block)(int, off_t, void *, uint32_t);
};
struct Backend { struct Client *client; };
static struct Client client = { config_read, config_read_block };
static struct Backend backend = { &client };
static struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
int main(void)
{
    AppleVFIOState apple = {0};
    VFIOApplePCIDevice device = { &apple };
    VFIODevice vdev = { &device };
    uint8_t data[PCIE_CONFIG_SPACE_SIZE];

    put32(physical + 0x100, PCI_EXT_CAP_ID_REBAR);
    put32(physical + 0x108, 1 << PCI_REBAR_CTRL_NBAR_SHIFT);
    assert(apple_vfio_region_read(&vdev, 7, 0, sizeof(data), data) == sizeof(data));
    assert(apple.rebar_scan_valid && apple.physical_rebar_offset == 0x100);
    put32(physical + 0x108, 0);
    assert(apple_vfio_region_read(&vdev, 7, 0, sizeof(data), data) == -EINVAL);
    assert(!apple.rebar_scan_valid && errors == 1 && guest_logs == 1);
    memset(physical, 0, sizeof(physical));
    put32(physical + 0x100, 1 | (0x100U << 20));
    assert(apple_vfio_region_read(&vdev, 7, 0, sizeof(data), data) == -EINVAL);
    assert(!apple.rebar_scan_valid && errors == 2 && guest_logs == 2);
    memset(physical, 0, sizeof(physical));
    assert(apple_vfio_region_read(&vdev, 7, 0, sizeof(data), data) == sizeof(data));
    assert(apple.rebar_scan_valid);
    fail_read = 1;
    assert(apple_vfio_region_read(&vdev, 7, 0, sizeof(data), data) == -EIO);
    fail_read = 0;
    assert(apple_vfio_region_read(&vdev, 7, 0, 4, data) == 4);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static void apple_vfio_cache_physical_rebar(")
    end = source.index("static bool apple_vfio_config_write_is_safe(", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
