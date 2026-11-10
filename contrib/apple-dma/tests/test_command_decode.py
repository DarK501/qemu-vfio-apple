#!/usr/bin/env python3
"""Verify guest PCI command writes preserve host decode settings."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <assert.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#define PCI_COMMAND 4
#define PCI_COMMAND_IO 1
#define PCI_COMMAND_MEMORY 2
#define IO_OBJECT_NULL 0
#define KERN_SUCCESS 0
#define LOG_GUEST_ERROR 1
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define error_report(...) do { errors++; } while (0)
#define qemu_log_mask(...) do { logs++; } while (0)
typedef int io_connect_t;
typedef int kern_return_t;
static uint8_t physical[8];
static int read_failure, write_failure, reads, writes, errors, logs;
static int config_read(int conn, off_t off, uint32_t size, uint64_t *value)
{
    assert(conn == 1 && off == PCI_COMMAND && size == 1);
    reads++;
    *value = physical[off];
    return read_failure;
}
static int config_write(int conn, off_t off, uint32_t size, uint64_t value)
{
    assert(conn == 1 && off >= PCI_COMMAND && off < PCI_COMMAND + 2 && size == 1);
    writes++;
    if (write_failure) return write_failure;
    physical[off] = value;
    return 0;
}
struct Client {
    int (*config_read)(int, off_t, uint32_t, uint64_t *);
    int (*config_write)(int, off_t, uint32_t, uint64_t);
};
struct Backend { struct Client *client; };
static struct Client client = { config_read, config_write };
static struct Backend backend = { &client };
static struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
int main(void)
{
    uint8_t value[4] = {0, 0x04, 0xaa, 0xbb};
    physical[PCI_COMMAND] = 0x06;
    assert(!apple_vfio_forward_command_write(1, PCI_COMMAND, 4, value));
    assert(physical[PCI_COMMAND] == 0x02 && physical[PCI_COMMAND + 1] == 0x04);
    assert(reads == 1 && writes == 2 && logs == 1);

    value[0] = 0x07;
    assert(!apple_vfio_forward_command_write(1, PCI_COMMAND, 1, value));
    assert(physical[PCI_COMMAND] == 0x06);
    assert(reads == 2 && writes == 3 && logs == 2);

    physical[PCI_COMMAND] = 0x04;
    assert(!apple_vfio_forward_command_write(1, PCI_COMMAND, 1, value));
    assert(physical[PCI_COMMAND] == 0x04);
    assert(reads == 3 && writes == 4 && logs == 3);

    value[0] = 0x02;
    assert(!apple_vfio_forward_command_write(1, PCI_COMMAND + 1, 1, value));
    assert(physical[PCI_COMMAND + 1] == 0x02 && reads == 3 && writes == 5);
    value[0] = 0xff; value[1] = 0x04;
    physical[PCI_COMMAND] = 0x02;
    assert(!apple_vfio_forward_command_write(1, PCI_COMMAND - 1, 2, value));
    assert(physical[PCI_COMMAND] == 0x06 && reads == 4 && writes == 6);
    assert(apple_vfio_forward_command_write(0, PCI_COMMAND, 1, value) == -ENODEV);
    read_failure = 1;
    assert(apple_vfio_forward_command_write(1, PCI_COMMAND, 2, value) == -EIO);
    assert(writes == 6 && errors == 1 && logs == 5);
    read_failure = 0;
    physical[PCI_COMMAND] = 0xff;
    assert(apple_vfio_forward_command_write(1, PCI_COMMAND, 1, value) == -EIO);
    assert(writes == 6 && errors == 2 && logs == 6);
    physical[PCI_COMMAND] = 0x02;
    write_failure = 1;
    assert(apple_vfio_forward_command_write(1, PCI_COMMAND, 1, value) == -EIO);
    assert(physical[PCI_COMMAND] == 0x02 && errors == 3);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static int apple_vfio_forward_command_write(")
    end = source.index("static int apple_vfio_bar_write(", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
