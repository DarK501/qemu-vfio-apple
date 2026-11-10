/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Experimental host-memory backend for a DriverKit DMA allocation.
 * The backing allocation is retained until QEMU exits, after vCPUs stop and
 * deferred RAMBlock RCU callbacks are drained.
 */

#include "qemu/osdep.h"

#include "apple-dext-client.h"
#include "apple-dma-pool.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/units.h"
#include "system/system.h"
#include "system/hostmem.h"
#include "qom/object_interfaces.h"

#define APPLE_DMA_DEFAULT_ALIGNMENT (128ULL * MiB)

OBJECT_DECLARE_SIMPLE_TYPE(AppleDMAMemoryBackend, MEMORY_BACKEND_APPLE_DMA)

struct AppleDMAMemoryBackend {
    HostMemoryBackend parent;
    char *bdf;
    char *host_root;
    uint64_t alignment;
    uint64_t dma_address;
    uint64_t dma_length;
    mach_vm_address_t mapping;
    mach_vm_size_t mapping_size;
    io_connect_t connection;
    bool claimed;
    bool diagnostic_ram_device;
    uint64_t test_offset;
};

typedef struct AppleDMABacking {
    struct AppleDMABacking *next;
    io_connect_t connection;
    mach_vm_address_t mapping;
    bool claimed;
    char *bdf;
} AppleDMABacking;

static AppleDMABacking *apple_dma_backings;
static Notifier apple_dma_exit_notifier;
static bool apple_dma_exit_notifier_registered;
static Error *apple_dma_migration_blocker;

static void apple_dma_exit(Notifier *notifier, void *data);

static void apple_dma_register_exit_notifier(void)
{
    if (!apple_dma_exit_notifier_registered) {
        apple_dma_exit_notifier.notify = apple_dma_exit;
        qemu_add_exit_notifier(&apple_dma_exit_notifier);
        apple_dma_exit_notifier_registered = true;
    }
}

static bool apple_dma_parse_bdf(const char *text, uint8_t *bus,
                                uint8_t *device, uint8_t *function)
{
    unsigned int b, d, f;
    int end = 0;

    if (!text || sscanf(text, "%x:%x.%x%n", &b, &d, &f, &end) != 3 ||
        text[end] != '\0' || b > UINT8_MAX || d > 31 || f > 7) {
        return false;
    }
    *bus = b;
    *device = d;
    *function = f;
    return true;
}

static void apple_dma_exit(Notifier *notifier, void *data)
{
    AppleDMABacking *backing;

    (void)notifier;
    (void)data;

    /*
     * qemu_cleanup() has stopped vCPUs before exit notifiers run. Drain
     * deferred RAMBlock reclamation before releasing any preallocated host
     * pointer retained by this backend.
     */
    drain_call_rcu();
    while ((backing = apple_dma_backings) != NULL) {
        kern_return_t kr;

        apple_dma_backings = backing->next;
        kr = apple_dext_client_free_dma_buffer(backing->connection,
                                                backing->mapping);
        if (kr != KERN_SUCCESS) {
            error_report("vfio-apple: failed to release DMA pool for %s "
                         "(kr=0x%x); host DMA teardown is unverified",
                         backing->bdf, kr);
        }
        if (backing->claimed) {
            kr = apple_dext_client_release_claim(backing->connection);
            if (kr != KERN_SUCCESS) {
                error_report("vfio-apple: failed to release DMA pool claim "
                             "for %s (kr=0x%x)", backing->bdf, kr);
            }
        }
        apple_dext_client_disconnect(backing->connection);
        g_free(backing->bdf);
        g_free(backing);
    }
    migrate_del_blocker(&apple_dma_migration_blocker);
}

static void apple_dma_track(AppleDMAMemoryBackend *backend)
{
    AppleDMABacking *backing = g_new0(AppleDMABacking, 1);

    backing->connection = backend->connection;
    backing->mapping = backend->mapping;
    backing->claimed = backend->claimed;
    backing->bdf = g_strdup(backend->bdf);
    backing->next = apple_dma_backings;
    apple_dma_backings = backing;
    apple_dma_register_exit_notifier();
}

static bool apple_dma_backend_alloc(HostMemoryBackend *hostmem, Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(hostmem);
    uint8_t bus, device, function;
    char *connect_error = NULL;
    kern_return_t kr;

    if (!hostmem->size ||
        !QEMU_IS_ALIGNED(hostmem->size, qemu_real_host_page_size()) ||
        backend->alignment < qemu_real_host_page_size() ||
        (backend->alignment & (backend->alignment - 1))) {
        error_setg(errp, "size must be page aligned and alignment a power of "
                   "two of at least 4096 bytes");
        return false;
    }
    if (hostmem->guest_memfd || hostmem->prealloc) {
        error_setg(errp, "guest_memfd and preallocation are unsupported for "
                   "DriverKit-backed memory");
        return false;
    }
    if (!apple_dma_migration_blocker) {
        Error *local_err = NULL;

        error_setg(&apple_dma_migration_blocker,
                   "migration is disabled while DriverKit-backed RAM exists");
        if (migrate_add_blocker(&apple_dma_migration_blocker, &local_err) < 0) {
            error_propagate(errp, local_err);
            return false;
        }
        apple_dma_register_exit_notifier();
    }
    if (!apple_dma_parse_bdf(backend->bdf, &bus, &device, &function)) {
        error_setg(errp, "bdf must have the form BUS:DEVICE.FUNCTION");
        return false;
    }

    backend->connection = apple_dext_client_connect_with_root(
        bus, device, function, backend->host_root, &connect_error);
    if (backend->connection == IO_OBJECT_NULL) {
        error_setg(errp, "cannot connect to DriverKit service for %s: %s",
                   backend->bdf,
                   connect_error ? connect_error : "connection failed");
        free(connect_error);
        return false;
    }
    free(connect_error);

    kr = apple_dext_client_claim(backend->connection);
    if (kr != KERN_SUCCESS) {
        error_setg(errp, "cannot claim %s for diagnostic allocation (kr=0x%x)",
                   backend->bdf, kr);
        apple_dext_client_disconnect(backend->connection);
        backend->connection = IO_OBJECT_NULL;
        return false;
    }
    backend->claimed = true;

    kr = apple_dext_client_allocate_dma_buffer(
        backend->connection, hostmem->size, backend->alignment,
        &backend->dma_address, &backend->dma_length,
        &backend->mapping, &backend->mapping_size);
    if (kr != KERN_SUCCESS) {
        error_setg(errp, "DriverKit DMA allocation for %s failed (kr=0x%x)",
                   backend->bdf, kr);
        goto fail;
    }
    if ((backend->dma_address & (backend->alignment - 1)) ||
        backend->dma_length < hostmem->size ||
        backend->mapping_size < hostmem->size ||
        backend->mapping > UINTPTR_MAX ||
        !QEMU_IS_ALIGNED(backend->mapping, qemu_real_host_page_size()) ||
        hostmem->size - 1 > UINTPTR_MAX - backend->mapping ||
        hostmem->size - 1 > UINT64_MAX - backend->dma_address) {
        error_setg(errp, "DriverKit DMA allocation for %s is not a "
                   "contiguous, aligned, fully mapped range", backend->bdf);
        goto fail;
    }

    hostmem->merge = false;
    hostmem->dump = true;
    hostmem->prealloc = false;
    if (backend->diagnostic_ram_device) {
        memory_region_init_ram_device_ptr(&hostmem->mr, OBJECT(hostmem),
                                           "apple-dma-pool", hostmem->size,
                                           (void *)(uintptr_t)backend->mapping);
    } else {
        memory_region_init_ram_ptr(&hostmem->mr, OBJECT(hostmem),
                                   "apple-dma-pool", hostmem->size,
                                   (void *)(uintptr_t)backend->mapping);
    }
    /*
     * ram_ptr regions leave mr->align at 0, which makes memory-device
     * plug code compute size % 0.
     */
    hostmem->mr.align = qemu_real_host_page_size();
    apple_dma_track(backend);
    return true;

fail:
    if (backend->mapping) {
        kr = apple_dext_client_free_dma_buffer(backend->connection,
                                                backend->mapping);
        if (kr != KERN_SUCCESS) {
            error_report("vfio-apple: failed to roll back diagnostic DMA "
                         "allocation for %s (kr=0x%x)", backend->bdf, kr);
            apple_dma_track(backend);
            return false;
        }
    }
    if (backend->claimed) {
        kr = apple_dext_client_release_claim(backend->connection);
        if (kr != KERN_SUCCESS) {
            error_report("vfio-apple: failed to release diagnostic claim "
                         "for %s (kr=0x%x)", backend->bdf, kr);
            apple_dma_track(backend);
            return false;
        }
        backend->claimed = false;
    }
    apple_dext_client_disconnect(backend->connection);
    backend->connection = IO_OBJECT_NULL;
    return false;
}

static char *apple_dma_get_bdf(Object *obj, Error **errp)
{
    return g_strdup(MEMORY_BACKEND_APPLE_DMA(obj)->bdf);
}

static void apple_dma_set_bdf(Object *obj, const char *value, Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);

    if (host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "bdf cannot be changed after allocation");
        return;
    }
    g_free(backend->bdf);
    backend->bdf = g_strdup(value);
}

static char *apple_dma_get_host_root(Object *obj, Error **errp)
{
    return g_strdup(MEMORY_BACKEND_APPLE_DMA(obj)->host_root);
}

static void apple_dma_set_host_root(Object *obj, const char *value,
                                    Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);

    if (host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "host-root cannot be changed after allocation");
        return;
    }
    g_free(backend->host_root);
    backend->host_root = g_strdup(value);
}

static bool apple_dma_get_diagnostic_ram_device(Object *obj, Error **errp)
{
    return MEMORY_BACKEND_APPLE_DMA(obj)->diagnostic_ram_device;
}

static void apple_dma_set_diagnostic_ram_device(Object *obj, bool value,
                                                 Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);

    if (host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "diagnostic-ram-device cannot be changed after "
                   "allocation");
        return;
    }
    backend->diagnostic_ram_device = value;
}

static void apple_dma_get_alignment(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint64_t value = MEMORY_BACKEND_APPLE_DMA(obj)->alignment;

    visit_type_size(v, name, &value, errp);
}

static void apple_dma_set_alignment(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);
    uint64_t value;

    if (host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "alignment cannot be changed after allocation");
        return;
    }
    if (visit_type_size(v, name, &value, errp)) {
        backend->alignment = value;
    }
}

static void apple_dma_get_dma_address(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);
    uint64_t value = backend->dma_address;

    visit_type_uint64(v, name, &value, errp);
}

static void apple_dma_get_mapping_size(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);
    uint64_t value = backend->mapping_size;

    visit_type_uint64(v, name, &value, errp);
}

static void apple_dma_get_test_offset(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    uint64_t value = MEMORY_BACKEND_APPLE_DMA(obj)->test_offset;

    visit_type_uint64(v, name, &value, errp);
}

static void apple_dma_set_test_offset(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);
    uint64_t value;

    if (!host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "test-offset is available only after allocation");
        return;
    }
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!QEMU_IS_ALIGNED(value, sizeof(uint64_t)) ||
        value > MEMORY_BACKEND(obj)->size ||
        sizeof(uint64_t) > MEMORY_BACKEND(obj)->size - value) {
        error_setg(errp, "test-offset must select an aligned 8-byte range "
                   "inside the allocation");
        return;
    }
    backend->test_offset = value;
}

static void apple_dma_get_test_value(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);
    uint64_t value = 0;
    uint8_t *ptr;

    if (!host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "test-value is available only after allocation");
        return;
    }
    ptr = memory_region_get_ram_ptr(&MEMORY_BACKEND(obj)->mr);
    memcpy(&value, ptr + backend->test_offset, sizeof(value));
    visit_type_uint64(v, name, &value, errp);
}

static void apple_dma_set_test_value(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);
    uint64_t value;
    uint8_t *ptr;

    if (!host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "test-value is available only after allocation");
        return;
    }
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    ptr = memory_region_get_ram_ptr(&MEMORY_BACKEND(obj)->mr);
    memcpy(ptr + backend->test_offset, &value, sizeof(value));
}

static void apple_dma_backend_init(Object *obj)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);

    backend->alignment = APPLE_DMA_DEFAULT_ALIGNMENT;
    backend->connection = IO_OBJECT_NULL;
    backend->test_offset = 0;
    backend->bdf = g_strdup("");
}

static void apple_dma_backend_finalize(Object *obj)
{
    AppleDMAMemoryBackend *backend = MEMORY_BACKEND_APPLE_DMA(obj);

    g_free(backend->bdf);
    g_free(backend->host_root);
}

static void apple_dma_backend_class_init(ObjectClass *oc, const void *data)
{
    HostMemoryBackendClass *bc = MEMORY_BACKEND_CLASS(oc);

    bc->alloc = apple_dma_backend_alloc;
    object_class_property_add_str(oc, "bdf",
                                  apple_dma_get_bdf, apple_dma_set_bdf);
    object_class_property_add_str(oc, "host-root",
                                  apple_dma_get_host_root,
                                  apple_dma_set_host_root);
    object_class_property_add_bool(oc, "diagnostic-ram-device",
                                  apple_dma_get_diagnostic_ram_device,
                                  apple_dma_set_diagnostic_ram_device);
    object_class_property_add(oc, "alignment", "int",
                              apple_dma_get_alignment,
                              apple_dma_set_alignment,
                              NULL, NULL);
    object_class_property_add(oc, "dma-address", "uint64",
                              apple_dma_get_dma_address,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "cpu-mapping-size", "uint64",
                              apple_dma_get_mapping_size,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "test-offset", "uint64",
                              apple_dma_get_test_offset,
                              apple_dma_set_test_offset,
                              NULL, NULL);
    object_class_property_add(oc, "test-value", "uint64",
                              apple_dma_get_test_value,
                              apple_dma_set_test_value,
                              NULL, NULL);
}

static const TypeInfo apple_dma_backend_info = {
    .name = TYPE_MEMORY_BACKEND_APPLE_DMA,
    .parent = TYPE_MEMORY_BACKEND,
    .instance_size = sizeof(AppleDMAMemoryBackend),
    .instance_init = apple_dma_backend_init,
    .instance_finalize = apple_dma_backend_finalize,
    .class_size = sizeof(HostMemoryBackendClass),
    .class_init = apple_dma_backend_class_init,
};

static void apple_dma_backend_register_types(void)
{
    type_register_static(&apple_dma_backend_info);
}

type_init(apple_dma_backend_register_types);
