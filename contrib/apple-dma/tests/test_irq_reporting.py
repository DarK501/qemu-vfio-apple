#!/usr/bin/env python3
"""Verify interrupt read and notification errors are not silently ignored."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
DEVICE = ROOT / "hw/vfio/apple-device.c"
CLIENT = ROOT / "hw/vfio/apple-dext-client.c"

PREFIX = r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int kern_return_t;
typedef int io_connect_t;
enum { KERN_SUCCESS = 0, kIOReturnSuccess = 0, IO_OBJECT_NULL = 0, kIOReturnBadArgument = 9,
       kSelectorCheckInterrupt = 14, LOG_GUEST_ERROR = 1 };
#define error_report(...) do { errors++; } while (0)
#define qemu_log_mask(...) do { logs++; } while (0)
static int errors, logs, status, rearm_status, rearm_calls, deliveries;
static uint32_t reply_count = 4;
static int IOConnectCallMethod(int conn, int sel, const void *input,
    int input_count, const void *structure, int structure_size,
    uint64_t *output, uint32_t *count, void *outstruct, void *outsize)
{
    assert(conn == 1 && sel == 14 && !input && !input_count && !structure &&
           !structure_size && !outstruct && !outsize);
    output[0] = 2; output[1] = 1; *count = reply_count;
    return status;
}
typedef struct { int dummy; } VFIOPCIDevice;
typedef struct { int dummy; } VFIODevice;
typedef struct { int irq_notifier; void *irq_notify; } AppleVFIOState;
typedef struct { VFIOPCIDevice parent; AppleVFIOState *apple; } VFIOApplePCIDevice;
#define VFIO_PCI_DEVICE(p) (&(p)->parent)
static int event_notifier_test_and_clear(int *n) { (void)n; return 1; }
static int apple_vfio_connection(VFIODevice *v) { (void)v; return 1; }
static void apple_vfio_deliver_irq(VFIOPCIDevice *v, uint32_t vector)
{ (void)v; assert(vector == 1 || vector == 64); deliveries++; }
static int rearm(void *n) { (void)n; rearm_calls++; return rearm_status; }
"""

MIDDLE = r"""
static const struct Client {
    int (*read_pending_irqs)(int, uint64_t *);
    int (*interrupt_notify_rearm)(void *);
} client = { apple_dext_client_read_pending_irqs, rearm };
static const struct Backend { const struct Client *client; } backend = { &client };
static const struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
int main(void)
{
    AppleVFIOState apple = {0};
    VFIOApplePCIDevice dev = { .apple = &apple };
    uint64_t pending[4] = {9, 9, 9, 9};
    reply_count = 3;
    assert(apple_dext_client_read_pending_irqs(1, pending) == kIOReturnBadArgument);
    assert(pending[0] == 9 && pending[3] == 9);
    apple_vfio_irq_handler(&dev);
    assert(!deliveries && rearm_calls == 1 && errors == 1 && logs == 1);
    reply_count = 4;
    rearm_status = 7;
    apple_vfio_irq_handler(&dev);
    assert(deliveries == 2 && rearm_calls == 2 && errors == 2 && logs == 2);
    status = 5; rearm_status = 0;
    apple_vfio_irq_handler(&dev);
    assert(deliveries == 2 && rearm_calls == 3 && errors == 3 && logs == 3);
}
"""


def main():
    client = CLIENT.read_text()
    start = client.index("kern_return_t\napple_dext_client_read_pending_irqs(")
    end = client.index("\nstruct AppleDextInterruptNotify", start)
    device = DEVICE.read_text()
    handler = device.index("static void apple_vfio_irq_handler(")
    handler_end = device.index("\nbool apple_vfio_get_bar_info(", handler)
    # The real VFIOPCIDevice contains its base VFIODevice.
    prefix = PREFIX.replace(
        "typedef struct { int dummy; } VFIOPCIDevice;\n"
        "typedef struct { int dummy; } VFIODevice;",
        "typedef struct { int dummy; } VFIODevice;\n"
        "typedef struct { VFIODevice vbasedev; } VFIOPCIDevice;")
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(prefix + client[start:end] + MIDDLE +
                        device[handler:handler_end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
