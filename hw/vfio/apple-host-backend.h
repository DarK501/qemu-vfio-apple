/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host-architecture dispatch for Apple PCI passthrough.
 */

#ifndef HW_VFIO_APPLE_HOST_BACKEND_H
#define HW_VFIO_APPLE_HOST_BACKEND_H

#include "hw/vfio/apple-dext-client.h"

typedef struct AppleDextClientOps {
    __typeof__(&apple_dext_client_connect_with_root) connect_with_root;
    __typeof__(&apple_dext_client_disconnect) disconnect;
    __typeof__(&apple_dext_client_claim) claim;
    __typeof__(&apple_dext_client_allocate_dma_buffer) allocate_dma_buffer;
    __typeof__(&apple_dext_client_free_dma_buffer) free_dma_buffer;
    __typeof__(&apple_dext_client_probe_dma) probe_dma;
    __typeof__(&apple_dext_client_config_read) config_read;
    __typeof__(&apple_dext_client_config_write) config_write;
    __typeof__(&apple_dext_client_config_read_block) config_read_block;
    __typeof__(&apple_dext_client_get_bar_info) get_bar_info;
    __typeof__(&apple_dext_client_map_bar) map_bar;
    __typeof__(&apple_dext_client_unmap_bar) unmap_bar;
    __typeof__(&apple_dext_client_mmio_read) mmio_read;
    __typeof__(&apple_dext_client_mmio_write) mmio_write;
    __typeof__(&apple_dext_client_setup_interrupts) setup_interrupts;
    __typeof__(&apple_dext_client_reset_device) reset_device;
    __typeof__(&apple_dext_client_set_irq_mask) set_irq_mask;
    __typeof__(&apple_dext_client_read_pending_irqs) read_pending_irqs;
    __typeof__(&apple_dext_client_interrupt_notify_create)
        interrupt_notify_create;
    __typeof__(&apple_dext_client_interrupt_notify_rearm)
        interrupt_notify_rearm;
    __typeof__(&apple_dext_client_interrupt_notify_destroy)
        interrupt_notify_destroy;
} AppleDextClientOps;

typedef struct AppleHostBackend {
    const char *name;
    const AppleDextClientOps *client;
    __typeof__(&apple_dext_client_register_dma) register_dma;
    __typeof__(&apple_dext_client_unregister_dma) unregister_dma;
} AppleHostBackend;

const AppleHostBackend *apple_host_backend(void);

#endif
