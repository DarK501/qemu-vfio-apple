/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Darwin Apple-VFIO backend selection and DriverKit adapter.
 *
 * DriverKit's IODMACommand provides the device-visible DMA mappings. The OS
 * selects/programs the host IOMMU (Intel VT-d on x86_64 Macs or DART on Apple
 * Silicon). QEMU selects an architecture-labelled backend but does not touch
 * the IOMMU registers directly.
 */

#include "qemu/osdep.h"

#define APPLE_VFIO_BACKEND_IMPLEMENTATION
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

#if defined(__x86_64__)
static const AppleVFIOHostBackend darwin_intel_iommu_backend = {
    .name = "darwin-intel-iommu",
    .ops = &driverkit_pci_backend_ops,
};
#elif defined(__aarch64__) || defined(__arm64__)
static const AppleVFIOHostBackend darwin_apple_dart_backend = {
    .name = "darwin-apple-dart",
    .ops = &driverkit_pci_backend_ops,
};
#else
#error "Apple VFIO supports Darwin x86_64 (Intel) or arm64 (Apple Silicon) hosts"
#endif

const AppleVFIOHostBackend *apple_vfio_host_backend(void)
{
#if defined(__x86_64__)
    return &darwin_intel_iommu_backend;
#else
    return &darwin_apple_dart_backend;
#endif
}

const AppleVFIOHostBackendOps *apple_vfio_host_backend_ops(void)
{
    return apple_vfio_host_backend()->ops;
}
