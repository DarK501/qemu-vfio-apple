/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host-architecture dispatch for Apple PCI passthrough.
 */

#include "qemu/osdep.h"

#include "apple-dext-client.h"
#include "apple-host-backend.h"
#include "qemu/error-report.h"

static const AppleDextClientOps apple_dext_client_ops = {
    .connect_with_root = apple_dext_client_connect_with_root,
    .disconnect = apple_dext_client_disconnect,
    .claim = apple_dext_client_claim,
    .allocate_dma_buffer = apple_dext_client_allocate_dma_buffer,
    .free_dma_buffer = apple_dext_client_free_dma_buffer,
    .probe_dma = apple_dext_client_probe_dma,
    .config_read = apple_dext_client_config_read,
    .config_write = apple_dext_client_config_write,
    .config_read_block = apple_dext_client_config_read_block,
    .get_bar_info = apple_dext_client_get_bar_info,
    .map_bar = apple_dext_client_map_bar,
    .unmap_bar = apple_dext_client_unmap_bar,
    .mmio_read = apple_dext_client_mmio_read,
    .mmio_write = apple_dext_client_mmio_write,
    .setup_interrupts = apple_dext_client_setup_interrupts,
    .reset_device = apple_dext_client_reset_device,
    .set_irq_mask = apple_dext_client_set_irq_mask,
    .read_pending_irqs = apple_dext_client_read_pending_irqs,
    .interrupt_notify_create = apple_dext_client_interrupt_notify_create,
    .interrupt_notify_rearm = apple_dext_client_interrupt_notify_rearm,
    .interrupt_notify_destroy = apple_dext_client_interrupt_notify_destroy,
};

#if defined(__x86_64__)
static const AppleHostBackend apple_host_backend_instance = {
    .name = "Intel x86_64 (IODMACommand/host IOMMU; unverified)",
    .client = &apple_dext_client_ops,
    .register_dma = apple_dext_client_register_dma,
    .unregister_dma = apple_dext_client_unregister_dma,
};
#elif defined(__aarch64__) || defined(__arm64__)
static const AppleHostBackend apple_host_backend_instance = {
    .name = "Apple Silicon arm64 (IODMACommand/DART)",
    .client = &apple_dext_client_ops,
    .register_dma = apple_dext_client_register_dma,
    .unregister_dma = apple_dext_client_unregister_dma,
};
#else
#error "Apple VFIO passthrough supports only x86_64 and arm64 Darwin hosts"
#endif

const AppleHostBackend *apple_host_backend(void)
{
    static gsize logged;

    if (g_once_init_enter(&logged)) {
        info_report("vfio-apple: selected host backend: %s",
                    apple_host_backend_instance.name);
        g_once_init_leave(&logged, 1);
    }

    return &apple_host_backend_instance;
}
