/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host backend abstraction for Darwin VFIO passthrough.
 *
 * Darwin x86_64 selects the Intel-IOMMU host model; Darwin arm64 selects the
 * Apple-DART host model. Both are accessed through DriverKit's IODMACommand
 * interface in the dext. The platform-specific IOMMU programming is owned by
 * DriverKit; QEMU does not program VT-d or DART directly.
 */

#ifndef HW_VFIO_APPLE_HOST_BACKEND_H
#define HW_VFIO_APPLE_HOST_BACKEND_H

#include "qemu/osdep.h"

#include <IOKit/IOKitLib.h>

#include "hw/vfio/apple-dext-client.h"

typedef struct AppleVFIOHostBackendOps AppleVFIOHostBackendOps;
typedef struct AppleVFIOHostBackend AppleVFIOHostBackend;

struct AppleVFIOHostBackendOps {
    io_connect_t (*connect_with_root)(uint8_t bus, uint8_t device,
                                     uint8_t function, const char *host_root,
                                     char **errp);
    void (*disconnect)(io_connect_t connection);
    kern_return_t (*claim)(io_connect_t connection);
    kern_return_t (*register_dma)(io_connect_t connection, uint64_t iova,
                                  uint64_t client_va, uint64_t size,
                                  uint64_t *out_bus_addr,
                                  uint64_t *out_bus_len);
    kern_return_t (*allocate_dma_buffer)(io_connect_t connection,
                                        uint64_t size, uint64_t alignment,
                                        uint64_t *out_bus_addr,
                                        uint64_t *out_bus_len,
                                        mach_vm_address_t *out_addr,
                                        mach_vm_size_t *out_size);
    kern_return_t (*free_dma_buffer)(io_connect_t connection,
                                    mach_vm_address_t addr);
    kern_return_t (*unregister_dma)(io_connect_t connection, uint64_t iova);
    kern_return_t (*probe_dma)(io_connect_t connection, uint64_t iova,
                              uint64_t offset, uint64_t *out_word);
    kern_return_t (*config_read)(io_connect_t connection, uint64_t offset,
                                uint64_t width, uint64_t *out_value);
    kern_return_t (*config_write)(io_connect_t connection, uint64_t offset,
                                 uint64_t width, uint64_t value);
    kern_return_t (*config_read_block)(io_connect_t connection,
                                      uint64_t offset, void *buf, size_t len);
    kern_return_t (*get_bar_info)(io_connect_t connection, uint8_t bar,
                                 uint8_t *out_mem_idx, uint64_t *out_size,
                                 uint8_t *out_type);
    kern_return_t (*map_bar)(io_connect_t connection, uint8_t bar,
                            mach_vm_address_t *out_addr,
                            mach_vm_size_t *out_size, uint8_t *out_type);
    kern_return_t (*unmap_bar)(io_connect_t connection, uint8_t bar,
                              mach_vm_address_t addr);
    kern_return_t (*mmio_read)(io_connect_t connection, uint8_t mem_idx,
                              uint64_t offset, uint64_t width,
                              uint64_t *out_value);
    kern_return_t (*mmio_write)(io_connect_t connection, uint8_t mem_idx,
                               uint64_t offset, uint64_t width,
                               uint64_t value);
    kern_return_t (*setup_interrupts)(io_connect_t connection,
                                     uint32_t *out_num_vectors);
    kern_return_t (*reset_device)(io_connect_t connection);
    kern_return_t (*set_irq_mask)(io_connect_t connection,
                                 const uint64_t mask[4]);
    kern_return_t (*read_pending_irqs)(io_connect_t connection,
                                      uint64_t pending[4]);
    AppleDextInterruptNotify *(*interrupt_notify_create)(
        io_connect_t connection, void (*handler_fn)(void *opaque),
        void *opaque);
    kern_return_t (*interrupt_notify_rearm)(AppleDextInterruptNotify *notify);
    void (*interrupt_notify_destroy)(AppleDextInterruptNotify *notify);
};

struct AppleVFIOHostBackend {
    const char *name;
    const AppleVFIOHostBackendOps *ops;
};

const AppleVFIOHostBackend *apple_vfio_host_backend(void);
const AppleVFIOHostBackendOps *apple_vfio_host_backend_ops(void);

#endif /* HW_VFIO_APPLE_HOST_BACKEND_H */
