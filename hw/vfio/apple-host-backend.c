/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Darwin Apple-VFIO backends.
 *
 * QEMU does not program VT-d or DART registers. The DriverKit extension uses
 * IODMACommand, which asks the Darwin PCI/DMA stack to prepare device-visible
 * DMA segments. On an Intel Mac that stack programs Intel VT-d; on Apple
 * Silicon it programs DART. Keep distinct backend tables so the host model is
 * explicit while sharing the common DriverKit transport for PCI, BAR and IRQ
 * operations.
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
/* Intel VT-d DMA operations are executed by DriverKit's IODMACommand in the
 * VFIOUserPCIDriver dext. These adapters make the selected Intel mapping path
 * explicit at the QEMU boundary while retaining the shared IOUserClient wire
 * protocol. Returned addresses are the device-visible DMA segments, not GPAs.
 */
static kern_return_t intel_iommu_register_dma(io_connect_t connection,
                                             uint64_t iova, uint64_t client_va,
                                             uint64_t size,
                                             uint64_t *out_bus_addr,
                                             uint64_t *out_bus_len)
{
    return apple_dext_register_dma(connection, iova, client_va, size,
                                   out_bus_addr, out_bus_len);
}

static kern_return_t intel_iommu_allocate_dma_buffer(
    io_connect_t connection, uint64_t size, uint64_t alignment,
    uint64_t *out_bus_addr, uint64_t *out_bus_len,
    mach_vm_address_t *out_addr, mach_vm_size_t *out_size)
{
    return apple_dext_allocate_dma_buffer(connection, size, alignment,
                                          out_bus_addr, out_bus_len,
                                          out_addr, out_size);
}

static kern_return_t intel_iommu_free_dma_buffer(io_connect_t connection,
                                                 mach_vm_address_t addr)
{
    return apple_dext_free_dma_buffer(connection, addr);
}

static kern_return_t intel_iommu_unregister_dma(io_connect_t connection,
                                                uint64_t iova)
{
    return apple_dext_unregister_dma(connection, iova);
}

static kern_return_t intel_iommu_probe_dma(io_connect_t connection,
                                           uint64_t iova, uint64_t offset,
                                           uint64_t *out_word)
{
    return apple_dext_probe_dma(connection, iova, offset, out_word);
}

static const AppleVFIOHostBackendOps darwin_intel_iommu_ops = {
    .connect_with_root = apple_dext_connect_with_root,
    .disconnect = apple_dext_disconnect,
    .claim = apple_dext_claim,
    .register_dma = intel_iommu_register_dma,
    .allocate_dma_buffer = intel_iommu_allocate_dma_buffer,
    .free_dma_buffer = intel_iommu_free_dma_buffer,
    .unregister_dma = intel_iommu_unregister_dma,
    .probe_dma = intel_iommu_probe_dma,
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

static const AppleVFIOHostBackend darwin_intel_iommu_backend = {
    .name = "darwin-intel-iommu",
    .ops = &darwin_intel_iommu_ops,
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
