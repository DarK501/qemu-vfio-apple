/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * C API for communicating with the VFIOUserPCIDriver DriverKit extension.
 *
 * Copyright (c) 2026 Scott J. Goldman
 */

#ifndef HW_VFIO_APPLE_DEXT_CLIENT_H
#define HW_VFIO_APPLE_DEXT_CLIENT_H

#include <IOKit/IOKitLib.h>
#include <stdint.h>

/* Opaque async interrupt notification handle. */
typedef struct AppleDextInterruptNotify AppleDextInterruptNotify;

io_connect_t apple_dext_connect_with_root(uint8_t bus, uint8_t device,
                                          uint8_t function,
                                          const char *host_root,
                                          char **errp);
io_connect_t apple_dext_connect(uint8_t bus, uint8_t device,
                                uint8_t function);
void apple_dext_disconnect(io_connect_t connection);
kern_return_t apple_dext_claim(io_connect_t connection);
kern_return_t apple_dext_register_dma(io_connect_t connection, uint64_t iova,
                                       uint64_t client_va, uint64_t size,
                                       uint64_t *out_bus_addr,
                                       uint64_t *out_bus_len);
kern_return_t apple_dext_allocate_dma_buffer(io_connect_t connection,
                                             uint64_t size, uint64_t alignment,
                                             uint64_t *out_bus_addr,
                                             uint64_t *out_bus_len,
                                             mach_vm_address_t *out_addr,
                                             mach_vm_size_t *out_size);
kern_return_t apple_dext_free_dma_buffer(io_connect_t connection,
                                          mach_vm_address_t addr);
kern_return_t apple_dext_unregister_dma(io_connect_t connection,
                                         uint64_t iova);
kern_return_t apple_dext_probe_dma(io_connect_t connection, uint64_t iova,
                                   uint64_t offset, uint64_t *out_word);
kern_return_t apple_dext_config_read(io_connect_t connection, uint64_t offset,
                                     uint64_t width, uint64_t *out_value);
kern_return_t apple_dext_config_write(io_connect_t connection, uint64_t offset,
                                      uint64_t width, uint64_t value);
kern_return_t apple_dext_config_read_block(io_connect_t connection,
                                           uint64_t offset, void *buf,
                                           size_t len);
kern_return_t apple_dext_get_bar_info(io_connect_t connection, uint8_t bar,
                                      uint8_t *out_mem_idx, uint64_t *out_size,
                                      uint8_t *out_type);
kern_return_t apple_dext_map_bar(io_connect_t connection, uint8_t bar,
                                 mach_vm_address_t *out_addr,
                                 mach_vm_size_t *out_size, uint8_t *out_type);
kern_return_t apple_dext_unmap_bar(io_connect_t connection, uint8_t bar,
                                   mach_vm_address_t addr);
kern_return_t apple_dext_mmio_read(io_connect_t connection, uint8_t mem_idx,
                                   uint64_t offset, uint64_t width,
                                   uint64_t *out_value);
kern_return_t apple_dext_mmio_write(io_connect_t connection, uint8_t mem_idx,
                                    uint64_t offset, uint64_t width,
                                    uint64_t value);
kern_return_t apple_dext_setup_interrupts(io_connect_t connection,
                                          uint32_t *out_num_vectors);
kern_return_t apple_dext_reset_device(io_connect_t connection);
kern_return_t apple_dext_set_irq_mask(io_connect_t connection,
                                      const uint64_t mask[4]);
kern_return_t apple_dext_read_pending_irqs(io_connect_t connection,
                                           uint64_t pending[4]);
AppleDextInterruptNotify *apple_dext_interrupt_notify_create(
    io_connect_t connection, void (*handler_fn)(void *opaque), void *opaque);
kern_return_t apple_dext_interrupt_notify_rearm(
    AppleDextInterruptNotify *notify);
void apple_dext_interrupt_notify_destroy(AppleDextInterruptNotify *notify);

#ifndef APPLE_VFIO_BACKEND_IMPLEMENTATION
#include "hw/vfio/apple-host-backend.h"

/* Route all consumers through the architecture-selected backend. The backend
 * implementation defines APPLE_VFIO_BACKEND_IMPLEMENTATION to bind these ops
 * to the underlying DriverKit client without recursive macro expansion.
 */
#define apple_dext_connect_with_root(bus, device, function, root, errp) \
    (apple_vfio_host_backend_ops()->connect_with_root( \
        (bus), (device), (function), (root), (errp)))
#define apple_dext_connect(bus, device, function) \
    (apple_vfio_host_backend_ops()->connect_with_root( \
        (bus), (device), (function), NULL, NULL))
#define apple_dext_disconnect(conn) \
    (apple_vfio_host_backend_ops()->disconnect(conn))
#define apple_dext_claim(conn) (apple_vfio_host_backend_ops()->claim(conn))
#define apple_dext_register_dma(conn, iova, va, size, addr, len) \
    (apple_vfio_host_backend_ops()->register_dma( \
        (conn), (iova), (va), (size), (addr), (len)))
#define apple_dext_allocate_dma_buffer(conn, size, align, addr, len, va, vsize) \
    (apple_vfio_host_backend_ops()->allocate_dma_buffer( \
        (conn), (size), (align), (addr), (len), (va), (vsize)))
#define apple_dext_free_dma_buffer(conn, addr) \
    (apple_vfio_host_backend_ops()->free_dma_buffer((conn), (addr)))
#define apple_dext_unregister_dma(conn, iova) \
    (apple_vfio_host_backend_ops()->unregister_dma((conn), (iova)))
#define apple_dext_probe_dma(conn, iova, off, word) \
    (apple_vfio_host_backend_ops()->probe_dma((conn), (iova), (off), (word)))
#define apple_dext_config_read(conn, off, width, value) \
    (apple_vfio_host_backend_ops()->config_read((conn), (off), (width), (value)))
#define apple_dext_config_write(conn, off, width, value) \
    (apple_vfio_host_backend_ops()->config_write((conn), (off), (width), (value)))
#define apple_dext_config_read_block(conn, off, buf, len) \
    (apple_vfio_host_backend_ops()->config_read_block( \
        (conn), (off), (buf), (len)))
#define apple_dext_get_bar_info(conn, bar, idx, size, type) \
    (apple_vfio_host_backend_ops()->get_bar_info( \
        (conn), (bar), (idx), (size), (type)))
#define apple_dext_map_bar(conn, bar, addr, size, type) \
    (apple_vfio_host_backend_ops()->map_bar( \
        (conn), (bar), (addr), (size), (type)))
#define apple_dext_unmap_bar(conn, bar, addr) \
    (apple_vfio_host_backend_ops()->unmap_bar((conn), (bar), (addr)))
#define apple_dext_mmio_read(conn, idx, off, width, value) \
    (apple_vfio_host_backend_ops()->mmio_read( \
        (conn), (idx), (off), (width), (value)))
#define apple_dext_mmio_write(conn, idx, off, width, value) \
    (apple_vfio_host_backend_ops()->mmio_write( \
        (conn), (idx), (off), (width), (value)))
#define apple_dext_setup_interrupts(conn, vectors) \
    (apple_vfio_host_backend_ops()->setup_interrupts((conn), (vectors)))
#define apple_dext_reset_device(conn) \
    (apple_vfio_host_backend_ops()->reset_device(conn))
#define apple_dext_set_irq_mask(conn, mask) \
    (apple_vfio_host_backend_ops()->set_irq_mask((conn), (mask)))
#define apple_dext_read_pending_irqs(conn, pending) \
    (apple_vfio_host_backend_ops()->read_pending_irqs((conn), (pending)))
#define apple_dext_interrupt_notify_create(conn, fn, opaque) \
    (apple_vfio_host_backend_ops()->interrupt_notify_create( \
        (conn), (fn), (opaque)))
#define apple_dext_interrupt_notify_rearm(notify) \
    (apple_vfio_host_backend_ops()->interrupt_notify_rearm(notify))
#define apple_dext_interrupt_notify_destroy(notify) \
    (apple_vfio_host_backend_ops()->interrupt_notify_destroy(notify))
#endif /* !APPLE_VFIO_BACKEND_IMPLEMENTATION */

#endif /* HW_VFIO_APPLE_DEXT_CLIENT_H */
