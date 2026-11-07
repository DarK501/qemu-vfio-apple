/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Darwin DriverKit host backend for Apple-VFIO.
 *
 * PCIDriverKit is a Darwin host API, not an Apple-Silicon CPU API. Keep the
 * backend selected by the Darwin build and let DriverKit/IODMACommand provide
 * the host-specific PCI and DMA behavior. In particular, do not select a
 * non-Darwin "Intel" stub based on host architecture: Intel Macs are Darwin.
 *
 * This backend forwards to the existing DriverKit client implementation. It
 * does not by itself establish that every Intel Mac/device supports passthrough;
 * that requires validation on the target macOS and hardware.
 */

#include "qemu/osdep.h"

#include "hw/vfio/apple-dext-client.h"
#include "hw/vfio/apple-host-backend.h"

static const AppleVFIOHostBackendOps driverkit_pci_backend_ops = {
    .connect_with_root = apple_dext_connect_with_root,
    .disconnect = apple_dext_disconnect,
    .claim = apple_dext_claim,
    .register_dma = apple_dext_register_dma,
    .allocate_dma_buffer = apple_dext_allocate_dma_buffer,
    .free_dma_buffer = apple_dext_free_dma_buffer,
    .unregister_dma = apple_dext_unregister_dma,
    .probe_dma = apple_dext_probe_dma,
    .config_read = apple_dext_config_read,
    .config_write = apple_dext_config_write,
    .config_read_block = apple_dext_config_read_block,
    .get_bar_info = apple_dext_get_bar_info,
    .map_bar = apple_dext_map_bar,
    .unmap_bar = apple_dext_unmap_bar,
    .mmio_read = apple_dext_mmio_read,
    .mmio_write = apple_dext_mmio_write,
    .setup_interrupts = apple_dext_setup_interrupts,
    .reset_device = apple_dext_reset_device,
    .set_irq_mask = apple_dext_set_irq_mask,
    .read_pending_irqs = apple_dext_read_pending_irqs,
    .interrupt_notify_create = apple_dext_interrupt_notify_create,
    .interrupt_notify_rearm = apple_dext_interrupt_notify_rearm,
    .interrupt_notify_destroy = apple_dext_interrupt_notify_destroy,
};

static const AppleVFIOHostBackend driverkit_pci_backend = {
    .name = "darwin-driverkit-pci",
    .ops = &driverkit_pci_backend_ops,
};

const AppleVFIOHostBackend *apple_vfio_host_backend(void)
{
    return &driverkit_pci_backend;
}
