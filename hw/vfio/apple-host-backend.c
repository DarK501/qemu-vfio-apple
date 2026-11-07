/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host backend implementation for Apple-VFIO.
 *
 * This isolates the current Apple Silicon DART-backed implementation behind an
 * explicit backend interface. The Apple path remains the only supported host
 * backend for the existing design, while an Intel host backend can be plugged in
 * by implementing the same ops table later.
 */

#include "qemu/osdep.h"

#include "hw/vfio/apple-dext-client.h"
#include "hw/vfio/apple-host-backend.h"

#ifdef CONFIG_DARWIN
static io_connect_t apple_vfio_backend_connect_with_root(uint8_t bus,
                                                       uint8_t device,
                                                       uint8_t function,
                                                       const char *host_root,
                                                       char **errp)
{
    return apple_dext_connect_with_root(bus, device, function, host_root, errp);
}

static void apple_vfio_backend_disconnect(io_connect_t connection)
{
    apple_dext_disconnect(connection);
}

static kern_return_t apple_vfio_backend_claim(io_connect_t connection)
{
    return apple_dext_claim(connection);
}

static kern_return_t apple_vfio_backend_register_dma(io_connect_t connection,
                                                   uint64_t iova,
                                                   uint64_t client_va,
                                                   uint64_t size,
                                                   uint64_t *out_bus_addr,
                                                   uint64_t *out_bus_len)
{
    return apple_dext_register_dma(connection, iova, client_va, size,
                                  out_bus_addr, out_bus_len);
}

static kern_return_t apple_vfio_backend_allocate_dma_buffer(
    io_connect_t connection,
    uint64_t size,
    uint64_t alignment,
    uint64_t *out_bus_addr,
    uint64_t *out_bus_len,
    mach_vm_address_t *out_addr,
    mach_vm_size_t *out_size)
{
    return apple_dext_allocate_dma_buffer(connection, size, alignment,
                                         out_bus_addr, out_bus_len,
                                         out_addr, out_size);
}

static kern_return_t apple_vfio_backend_free_dma_buffer(io_connect_t connection,
                                                       mach_vm_address_t addr)
{
    return apple_dext_free_dma_buffer(connection, addr);
}

static kern_return_t apple_vfio_backend_unregister_dma(io_connect_t connection,
                                                     uint64_t iova)
{
    return apple_dext_unregister_dma(connection, iova);
}

static kern_return_t apple_vfio_backend_probe_dma(io_connect_t connection,
                                                uint64_t iova,
                                                uint64_t offset,
                                                uint64_t *out_word)
{
    return apple_dext_probe_dma(connection, iova, offset, out_word);
}

static kern_return_t apple_vfio_backend_config_read(io_connect_t connection,
                                                    uint64_t offset,
                                                    uint64_t width,
                                                    uint64_t *out_value)
{
    return apple_dext_config_read(connection, offset, width, out_value);
}

static kern_return_t apple_vfio_backend_config_write(io_connect_t connection,
                                                     uint64_t offset,
                                                     uint64_t width,
                                                     uint64_t value)
{
    return apple_dext_config_write(connection, offset, width, value);
}

static kern_return_t apple_vfio_backend_config_read_block(io_connect_t connection,
                                                        uint64_t offset,
                                                        void *buf,
                                                        size_t len)
{
    return apple_dext_config_read_block(connection, offset, buf, len);
}

static kern_return_t apple_vfio_backend_get_bar_info(io_connect_t connection,
                                                    uint8_t bar,
                                                    uint8_t *out_mem_idx,
                                                    uint64_t *out_size,
                                                    uint8_t *out_type)
{
    return apple_dext_get_bar_info(connection, bar, out_mem_idx, out_size,
                                  out_type);
}

static kern_return_t apple_vfio_backend_map_bar(io_connect_t connection,
                                              uint8_t bar,
                                              mach_vm_address_t *out_addr,
                                              mach_vm_size_t *out_size,
                                              uint8_t *out_type)
{
    return apple_dext_map_bar(connection, bar, out_addr, out_size, out_type);
}

static kern_return_t apple_vfio_backend_unmap_bar(io_connect_t connection,
                                                uint8_t bar,
                                                mach_vm_address_t addr)
{
    return apple_dext_unmap_bar(connection, bar, addr);
}

static kern_return_t apple_vfio_backend_mmio_read(io_connect_t connection,
                                                uint8_t mem_idx,
                                                uint64_t offset,
                                                uint64_t width,
                                                uint64_t *out_value)
{
    return apple_dext_mmio_read(connection, mem_idx, offset, width, out_value);
}

static kern_return_t apple_vfio_backend_mmio_write(io_connect_t connection,
                                                 uint8_t mem_idx,
                                                 uint64_t offset,
                                                 uint64_t width,
                                                 uint64_t value)
{
    return apple_dext_mmio_write(connection, mem_idx, offset, width, value);
}

static kern_return_t apple_vfio_backend_setup_interrupts(io_connect_t connection,
                                                       uint32_t *out_num_vectors)
{
    return apple_dext_setup_interrupts(connection, out_num_vectors);
}

static kern_return_t apple_vfio_backend_reset_device(io_connect_t connection)
{
    return apple_dext_reset_device(connection);
}

static kern_return_t apple_vfio_backend_set_irq_mask(io_connect_t connection,
                                                    const uint64_t mask[4])
{
    return apple_dext_set_irq_mask(connection, mask);
}

static kern_return_t apple_vfio_backend_read_pending_irqs(io_connect_t connection,
                                                         uint64_t pending[4])
{
    return apple_dext_read_pending_irqs(connection, pending);
}

static AppleDextInterruptNotify *apple_vfio_backend_interrupt_notify_create(
    io_connect_t connection,
    void (*handler_fn)(void *opaque),
    void *opaque)
{
    return apple_dext_interrupt_notify_create(connection, handler_fn, opaque);
}

static kern_return_t apple_vfio_backend_interrupt_notify_rearm(
    AppleDextInterruptNotify *notify)
{
    return apple_dext_interrupt_notify_rearm(notify);
}

static void apple_vfio_backend_interrupt_notify_destroy(
    AppleDextInterruptNotify *notify)
{
    apple_dext_interrupt_notify_destroy(notify);
}

static const AppleVFIOHostBackendOps apple_vfio_apple_backend_ops = {
    .connect_with_root = apple_vfio_backend_connect_with_root,
    .disconnect = apple_vfio_backend_disconnect,
    .claim = apple_vfio_backend_claim,
    .register_dma = apple_vfio_backend_register_dma,
    .allocate_dma_buffer = apple_vfio_backend_allocate_dma_buffer,
    .free_dma_buffer = apple_vfio_backend_free_dma_buffer,
    .unregister_dma = apple_vfio_backend_unregister_dma,
    .probe_dma = apple_vfio_backend_probe_dma,
    .config_read = apple_vfio_backend_config_read,
    .config_write = apple_vfio_backend_config_write,
    .config_read_block = apple_vfio_backend_config_read_block,
    .get_bar_info = apple_vfio_backend_get_bar_info,
    .map_bar = apple_vfio_backend_map_bar,
    .unmap_bar = apple_vfio_backend_unmap_bar,
    .mmio_read = apple_vfio_backend_mmio_read,
    .mmio_write = apple_vfio_backend_mmio_write,
    .setup_interrupts = apple_vfio_backend_setup_interrupts,
    .reset_device = apple_vfio_backend_reset_device,
    .set_irq_mask = apple_vfio_backend_set_irq_mask,
    .read_pending_irqs = apple_vfio_backend_read_pending_irqs,
    .interrupt_notify_create = apple_vfio_backend_interrupt_notify_create,
    .interrupt_notify_rearm = apple_vfio_backend_interrupt_notify_rearm,
    .interrupt_notify_destroy = apple_vfio_backend_interrupt_notify_destroy,
};

static const AppleVFIOHostBackend apple_vfio_apple_backend = {
    .name = "apple-dart",
    .ops = &apple_vfio_apple_backend_ops,
};
#else
static io_connect_t apple_vfio_intel_connect_with_root(uint8_t bus,
                                                     uint8_t device,
                                                     uint8_t function,
                                                     const char *host_root,
                                                     char **errp)
{
    error_setg(errp,
               "vfio-apple: Intel host backend is not implemented yet; "
               "the Apple Silicon DART-backed path is the only supported "
               "backend at this stage");
    return IO_OBJECT_NULL;
}

static void apple_vfio_intel_disconnect(io_connect_t connection)
{
    (void)connection;
}

static kern_return_t apple_vfio_intel_claim(io_connect_t connection)
{
    (void)connection;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_register_dma(io_connect_t connection,
                                                 uint64_t iova,
                                                 uint64_t client_va,
                                                 uint64_t size,
                                                 uint64_t *out_bus_addr,
                                                 uint64_t *out_bus_len)
{
    (void)connection; (void)iova; (void)client_va; (void)size;
    (void)out_bus_addr; (void)out_bus_len;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_allocate_dma_buffer(
    io_connect_t connection,
    uint64_t size,
    uint64_t alignment,
    uint64_t *out_bus_addr,
    uint64_t *out_bus_len,
    mach_vm_address_t *out_addr,
    mach_vm_size_t *out_size)
{
    (void)connection; (void)size; (void)alignment; (void)out_bus_addr;
    (void)out_bus_len; (void)out_addr; (void)out_size;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_free_dma_buffer(io_connect_t connection,
                                                     mach_vm_address_t addr)
{
    (void)connection; (void)addr;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_unregister_dma(io_connect_t connection,
                                                   uint64_t iova)
{
    (void)connection; (void)iova;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_probe_dma(io_connect_t connection,
                                              uint64_t iova,
                                              uint64_t offset,
                                              uint64_t *out_word)
{
    (void)connection; (void)iova; (void)offset; (void)out_word;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_config_read(io_connect_t connection,
                                                uint64_t offset,
                                                uint64_t width,
                                                uint64_t *out_value)
{
    (void)connection; (void)offset; (void)width; (void)out_value;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_config_write(io_connect_t connection,
                                                 uint64_t offset,
                                                 uint64_t width,
                                                 uint64_t value)
{
    (void)connection; (void)offset; (void)width; (void)value;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_config_read_block(io_connect_t connection,
                                                      uint64_t offset,
                                                      void *buf,
                                                      size_t len)
{
    (void)connection; (void)offset; (void)buf; (void)len;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_get_bar_info(io_connect_t connection,
                                                uint8_t bar,
                                                uint8_t *out_mem_idx,
                                                uint64_t *out_size,
                                                uint8_t *out_type)
{
    (void)connection; (void)bar; (void)out_mem_idx; (void)out_size;
    (void)out_type;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_map_bar(io_connect_t connection,
                                            uint8_t bar,
                                            mach_vm_address_t *out_addr,
                                            mach_vm_size_t *out_size,
                                            uint8_t *out_type)
{
    (void)connection; (void)bar; (void)out_addr; (void)out_size; (void)out_type;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_unmap_bar(io_connect_t connection,
                                              uint8_t bar,
                                              mach_vm_address_t addr)
{
    (void)connection; (void)bar; (void)addr;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_mmio_read(io_connect_t connection,
                                              uint8_t mem_idx,
                                              uint64_t offset,
                                              uint64_t width,
                                              uint64_t *out_value)
{
    (void)connection; (void)mem_idx; (void)offset; (void)width; (void)out_value;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_mmio_write(io_connect_t connection,
                                               uint8_t mem_idx,
                                               uint64_t offset,
                                               uint64_t width,
                                               uint64_t value)
{
    (void)connection; (void)mem_idx; (void)offset; (void)width; (void)value;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_setup_interrupts(io_connect_t connection,
                                                       uint32_t *out_num_vectors)
{
    (void)connection; (void)out_num_vectors;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_reset_device(io_connect_t connection)
{
    (void)connection;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_set_irq_mask(io_connect_t connection,
                                                  const uint64_t mask[4])
{
    (void)connection; (void)mask;
    return kIOReturnNotSupported;
}

static kern_return_t apple_vfio_intel_read_pending_irqs(io_connect_t connection,
                                                       uint64_t pending[4])
{
    (void)connection; (void)pending;
    return kIOReturnNotSupported;
}

static AppleDextInterruptNotify *apple_vfio_intel_interrupt_notify_create(
    io_connect_t connection,
    void (*handler_fn)(void *opaque),
    void *opaque)
{
    (void)connection; (void)handler_fn; (void)opaque;
    return NULL;
}

static kern_return_t apple_vfio_intel_interrupt_notify_rearm(
    AppleDextInterruptNotify *notify)
{
    (void)notify;
    return kIOReturnNotSupported;
}

static void apple_vfio_intel_interrupt_notify_destroy(
    AppleDextInterruptNotify *notify)
{
    (void)notify;
}

static const AppleVFIOHostBackendOps apple_vfio_intel_backend_ops = {
    .connect_with_root = apple_vfio_intel_connect_with_root,
    .disconnect = apple_vfio_intel_disconnect,
    .claim = apple_vfio_intel_claim,
    .register_dma = apple_vfio_intel_register_dma,
    .allocate_dma_buffer = apple_vfio_intel_allocate_dma_buffer,
    .free_dma_buffer = apple_vfio_intel_free_dma_buffer,
    .unregister_dma = apple_vfio_intel_unregister_dma,
    .probe_dma = apple_vfio_intel_probe_dma,
    .config_read = apple_vfio_intel_config_read,
    .config_write = apple_vfio_intel_config_write,
    .config_read_block = apple_vfio_intel_config_read_block,
    .get_bar_info = apple_vfio_intel_get_bar_info,
    .map_bar = apple_vfio_intel_map_bar,
    .unmap_bar = apple_vfio_intel_unmap_bar,
    .mmio_read = apple_vfio_intel_mmio_read,
    .mmio_write = apple_vfio_intel_mmio_write,
    .setup_interrupts = apple_vfio_intel_setup_interrupts,
    .reset_device = apple_vfio_intel_reset_device,
    .set_irq_mask = apple_vfio_intel_set_irq_mask,
    .read_pending_irqs = apple_vfio_intel_read_pending_irqs,
    .interrupt_notify_create = apple_vfio_intel_interrupt_notify_create,
    .interrupt_notify_rearm = apple_vfio_intel_interrupt_notify_rearm,
    .interrupt_notify_destroy = apple_vfio_intel_interrupt_notify_destroy,
};

static const AppleVFIOHostBackend apple_vfio_intel_backend = {
    .name = "intel",
    .ops = &apple_vfio_intel_backend_ops,
};
#endif

const AppleVFIOHostBackend *apple_vfio_host_backend(void)
{
#ifdef CONFIG_DARWIN
    return &apple_vfio_apple_backend;
#else
    return &apple_vfio_intel_backend;
#endif
}
