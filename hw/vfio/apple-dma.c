/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Apple DMA mapping PCI device
 *
 * A simple PCI device that receives batched DMA map/unmap requests from
 * the guest via a shared command page + doorbell register, resolves guest
 * physical addresses to host virtual addresses, and registers them with
 * the macOS DriverKit dext for host-IOMMU mapping.
 *
 * Protocol:
 *   1. Guest allocates a command page and request/response buffers in RAM.
 *   2. Guest writes the command page GPA to BAR registers (one-time setup).
 *   3. Per batch: guest fills the command page and request buffer (no VMEXIT),
 *      then writes the doorbell register (single VMEXIT triggers processing).
 *   4. Device reads the command page, processes all entries, writes responses
 *      and status back to guest RAM before the doorbell write returns.
 *
 * Copyright (c) 2026 Scott J. Goldman
 */

#include "qemu/osdep.h"

#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/vfio/apple-host-backend.h"
#include "hw/vfio/apple.h"
#include "contrib/apple-dma/apple-dma-protocol.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "system/memory.h"

#include "hw/pci/pci.h"

/* BAR0 register offsets */
#define APPLE_DMA_REG_VERSION       0x00    /* R:  protocol version */
#define APPLE_DMA_REG_MANAGED_BDF   0x04    /* R:  guest BDF this maps for */
#define APPLE_DMA_REG_MAX_ENTRIES   0x08    /* R:  max entries per batch */
#define APPLE_DMA_REG_STATUS        0x0C    /* R:  result of last doorbell */
#define APPLE_DMA_REG_CMD_GPA_LO    0x10    /* W:  command page GPA [31:0] */
#define APPLE_DMA_REG_CMD_GPA_HI    0x14    /* W:  command page GPA [63:32] */
#define APPLE_DMA_REG_DOORBELL      0x18    /* W:  any write triggers batch */
#define APPLE_DMA_REG_FEATURES      0x1C    /* R:  optional extensions */
#define APPLE_DMA_BAR_SIZE          0x1000  /* page-aligned */

#define APPLE_DMA_VERSION           APPLE_DMA_PROTOCOL_VERSION
#define APPLE_DMA_MAX_ENTRIES       4096

/* Command types (in command page) */
#define APPLE_DMA_CMD_MAP           1
#define APPLE_DMA_CMD_UNMAP         2

/* Status codes */
#define APPLE_DMA_S_OK              0
#define APPLE_DMA_S_IOERR           1
#define APPLE_DMA_S_INVAL           3

/*
 * Command page layout (in guest RAM, 32 bytes):
 *
 *   0x00  uint32_t  type         MAP=1, UNMAP=2
 *   0x04  uint32_t  count        number of entries
 *   0x08  uint32_t  status       (written by device) 0=OK
 *   0x0C  uint32_t  reserved
 *   0x10  uint64_t  req_gpa      GPA of request entries array
 *   0x18  uint64_t  resp_gpa     GPA of response entries array
 */
#define CMD_OFF_TYPE      0x00
#define CMD_OFF_COUNT     0x04
#define CMD_OFF_STATUS    0x08
#define CMD_OFF_REQ_GPA   0x10
#define CMD_OFF_RESP_GPA  0x18
#define CMD_PAGE_SIZE     0x20

/*
 * Map request entry (16 bytes):
 *   uint64_t gpa, uint32_t len, uint32_t flags
 *
 * Map response entry is AppleDMAMapResult (mapping token, count, status,
 * followed by ordered address/length/offset segments).
 *
 * Unmap request entry (16 bytes):
 *   uint64_t id, uint64_t size
 *
 * Unmap response entry (16 bytes):
 *   uint64_t id, uint32_t status, uint32_t reserved
 */

typedef struct AppleDMAMapReq {
    uint64_t gpa;
    uint32_t len;
    uint32_t flags;
} QEMU_PACKED AppleDMAMapReq;

typedef struct AppleDMAUnmapReq {
    uint64_t id;
    uint64_t size;
} QEMU_PACKED AppleDMAUnmapReq;

typedef struct AppleDMAUnmapResp {
    uint64_t id;
    uint32_t status;
    uint32_t reserved;
} QEMU_PACKED AppleDMAUnmapResp;

typedef struct AppleDMAMapping {
    uint64_t id;
    void *host_addr;
    hwaddr mapped_len;
    uint64_t size;
    bool ownership_unknown;
} AppleDMAMapping;

#define TYPE_APPLE_DMA_PCI "apple-dma-pci"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDMAState, APPLE_DMA_PCI)

struct AppleDMAState {
    PCIDevice parent_obj;

    MemoryRegion bar;

    /* Configuration (set via properties) */
    uint32_t managed_bdf;
    /*
     * Optional: the managed device. When set, the BDF reported to the guest
     * is computed live, because a device behind a bridge only gets its bus
     * number once firmware has enumerated it.
     */
    PCIDevice *managed_dev;
    uint32_t max_entries;
    uint32_t apple_host_bus;
    uint32_t apple_host_device;
    uint32_t apple_host_function;
    char *apple_host_root;

    /* Runtime state */
    uint64_t cmd_gpa;
    uint32_t last_status;
    io_connect_t dext_conn;
    bool discards_disabled;
    GHashTable *dma_mappings;
};

/* ------------------------------------------------------------------ */
/* DMA backend operations                                              */
/* ------------------------------------------------------------------ */

static uint32_t apple_dma_backend_map(AppleDMAState *s, uint64_t gpa,
                                      uint64_t id,
                                      uint32_t size,
                                      AppleDMAMapResult *out_result)
{
    hwaddr map_len = size;
    void *hva;
    ram_addr_t ram_offset;
    AppleDMAMapResult result = {0};
    AppleDMAMapping *mapping;
    AppleDMARegistrationState registration;
    const char *root = (s->apple_host_root && s->apple_host_root[0]) ?
        s->apple_host_root : NULL;

    if (s->dext_conn == IO_OBJECT_NULL ||
        !apple_vfio_dext_dma_owner_attached(
            s->apple_host_bus, s->apple_host_device, s->apple_host_function,
            root, s->dext_conn) ||
        g_hash_table_lookup(s->dma_mappings, &id)) {
        error_report("apple-dma: cannot map GPA 0x%" PRIx64
                     " without a live VFIO owner or over an existing map",
                     gpa);
        return APPLE_DMA_MAP_STATUS_ERROR;
    }

    mapping = g_try_new0(AppleDMAMapping, 1);
    if (!mapping) {
        return APPLE_DMA_MAP_STATUS_ERROR;
    }

    hva = dma_memory_map(&address_space_memory, gpa, &map_len,
                         DMA_DIRECTION_FROM_DEVICE,
                         MEMTXATTRS_UNSPECIFIED);
    if (!hva || map_len < size || !memory_region_from_host(hva, &ram_offset)) {
        if (hva) {
            dma_memory_unmap(&address_space_memory, hva, map_len,
                             DMA_DIRECTION_FROM_DEVICE, 0);
        }
        error_report("apple-dma: GPA 0x%" PRIx64
                     " size=%" PRIu32 " is not directly mapped writable RAM",
                     gpa, size);
        g_free(mapping);
        return APPLE_DMA_MAP_STATUS_ERROR;
    }

    /*
     * Keep the AddressSpace mapping alive until DriverKit completes DMA.
     * The mapping ID is not a bus address; only the returned segments
     * contain addresses the physical device may use.
     */
    kern_return_t kr = apple_host_backend()->register_dma(
        s->dext_conn, id, (uint64_t)hva, size, &result, &registration);
    if (kr != KERN_SUCCESS) {
        if (registration != APPLE_DMA_REGISTRATION_NONE) {
            mapping->id = id;
            mapping->host_addr = hva;
            mapping->mapped_len = map_len;
            mapping->size = size;
            mapping->ownership_unknown =
                registration == APPLE_DMA_REGISTRATION_UNKNOWN;
            g_hash_table_insert(s->dma_mappings, &mapping->id, mapping);
            out_result->mapping_id = id;
            error_report("apple-dma: registration failed for GPA 0x%" PRIx64
                         " (kr=0x%x, ownership %s); retaining guest backing",
                         gpa, kr, mapping->ownership_unknown ?
                         "unknown" : "confirmed");
            return APPLE_DMA_MAP_STATUS_RETAINED;
        }
        dma_memory_unmap(&address_space_memory, hva, map_len,
                         DMA_DIRECTION_FROM_DEVICE, 0);
        g_free(mapping);
        error_report("apple-dma: register_dma failed or returned incomplete "
                     "coverage gpa=0x%" PRIx64 " size=%" PRIu32 " kr=0x%x",
                     gpa, size, kr);
        return APPLE_DMA_MAP_STATUS_ERROR;
    }

    mapping->id = id;
    mapping->host_addr = hva;
    mapping->mapped_len = map_len;
    mapping->size = size;
    g_hash_table_insert(s->dma_mappings, &mapping->id, mapping);
    *out_result = result;

    return APPLE_DMA_MAP_STATUS_OK;
}

static uint32_t apple_dma_backend_unmap(AppleDMAState *s, uint64_t id,
                                        uint64_t size)
{
    AppleDMAMapping *mapping = g_hash_table_lookup(s->dma_mappings, &id);
    kern_return_t kr;

    if (!mapping || mapping->size != size ||
        s->dext_conn == IO_OBJECT_NULL) {
        error_report("apple-dma: cannot unmap id=0x%" PRIx64
                     " size=%" PRIu64 " (mapping %s)",
                     id, size, mapping ? "size mismatch" : "not found");
        return APPLE_DMA_S_IOERR;
    }

    if (mapping->ownership_unknown) {
        error_report("apple-dma: cannot safely unregister id=0x%" PRIx64
                     " with unknown ownership; retaining guest backing", id);
        return APPLE_DMA_S_IOERR;
    }

    kr = apple_host_backend()->unregister_dma(s->dext_conn, id);
    if (kr != KERN_SUCCESS) {
        error_report("apple-dma: unregister_dma failed id=0x%" PRIx64
                     " kr=0x%x; retaining guest backing",
                     id, kr);
        return APPLE_DMA_S_IOERR;
    }

    dma_memory_unmap(&address_space_memory, mapping->host_addr,
                     mapping->mapped_len, DMA_DIRECTION_FROM_DEVICE,
                     mapping->size);
    g_hash_table_remove(s->dma_mappings, &id);
    return APPLE_DMA_S_OK;
}

/* ------------------------------------------------------------------ */
/* Doorbell — process a batch from the command page                    */
/* ------------------------------------------------------------------ */

static bool apple_dma_request_mapping_id(uint64_t gpa, uint32_t flags,
                                         uint64_t *id)
{
    if (!flags) {
        *id = gpa;
        return true;
    }
    if (!(flags & APPLE_DMA_MAP_FLAG_MAPPING_ID) ||
        !(flags & APPLE_DMA_MAP_ID_MASK)) {
        return false;
    }
    *id = APPLE_DMA_MAP_ID_PREFIX | (flags & APPLE_DMA_MAP_ID_MASK);
    return true;
}

static void apple_dma_handle_map(AppleDMAState *s, uint64_t req_gpa,
                                 uint64_t resp_gpa, uint32_t count)
{
    AddressSpace *as = &address_space_memory;
    hwaddr req_len = count * sizeof(AppleDMAMapReq);
    hwaddr resp_len = count * sizeof(AppleDMAMapResult);
    AppleDMAMapReq *reqs;
    AppleDMAMapResult *resps;
    uint32_t i;
    bool ok = true;

    reqs = dma_memory_map(as, req_gpa, &req_len, DMA_DIRECTION_TO_DEVICE,
                          MEMTXATTRS_UNSPECIFIED);
    if (!reqs || req_len < count * sizeof(AppleDMAMapReq)) {
        if (reqs) {
            dma_memory_unmap(as, reqs, req_len, DMA_DIRECTION_TO_DEVICE, 0);
        }
        s->last_status = APPLE_DMA_S_INVAL;
        return;
    }

    resps = dma_memory_map(as, resp_gpa, &resp_len, DMA_DIRECTION_FROM_DEVICE,
                           MEMTXATTRS_UNSPECIFIED);
    if (!resps || resp_len < count * sizeof(AppleDMAMapResult)) {
        if (resps) {
            dma_memory_unmap(as, resps, resp_len,
                             DMA_DIRECTION_FROM_DEVICE, 0);
        }
        dma_memory_unmap(as, reqs, req_len, DMA_DIRECTION_TO_DEVICE, 0);
        s->last_status = APPLE_DMA_S_INVAL;
        return;
    }

    memset(resps, 0, count * sizeof(*resps));
    for (i = 0; i < count; i++) {
        uint64_t gpa = le64_to_cpu(reqs[i].gpa);
        uint32_t flags = le32_to_cpu(reqs[i].flags);
        uint64_t id;
        AppleDMAMapResult *resp = &resps[i];
        uint32_t status;

        if (!apple_dma_request_mapping_id(gpa, flags, &id)) {
            resp->status = cpu_to_le32(APPLE_DMA_MAP_STATUS_ERROR);
            ok = false;
            continue;
        }
        status = apple_dma_backend_map(s, gpa, id,
                                       le32_to_cpu(reqs[i].len), resp);

        if (status == APPLE_DMA_MAP_STATUS_OK) {
            resp->mapping_id = cpu_to_le64(resp->mapping_id);
            resp->segment_count = cpu_to_le32(resp->segment_count);
            resp->status = cpu_to_le32(APPLE_DMA_S_OK);
            for (uint32_t j = 0; j < le32_to_cpu(resp->segment_count); j++) {
                resp->segments[j].address =
                    cpu_to_le64(resp->segments[j].address);
                resp->segments[j].length =
                    cpu_to_le64(resp->segments[j].length);
                resp->segments[j].offset =
                    cpu_to_le64(resp->segments[j].offset);
            }
        } else {
            resp->mapping_id = cpu_to_le64(resp->mapping_id);
            resp->status = cpu_to_le32(status);
            ok = false;
        }
    }

    s->last_status = ok ? APPLE_DMA_S_OK : APPLE_DMA_S_IOERR;
    dma_memory_unmap(as, resps, resp_len, DMA_DIRECTION_FROM_DEVICE,
                     resp_len);
    dma_memory_unmap(as, reqs, req_len, DMA_DIRECTION_TO_DEVICE, 0);
}

static void apple_dma_handle_unmap(AppleDMAState *s, uint64_t req_gpa,
                                   uint64_t resp_gpa, uint32_t count)
{
    AddressSpace *as = &address_space_memory;
    hwaddr req_len = count * sizeof(AppleDMAUnmapReq);
    hwaddr resp_len = count * sizeof(AppleDMAUnmapResp);
    AppleDMAUnmapReq *reqs;
    AppleDMAUnmapResp *resps;
    uint32_t i;
    bool ok = true;

    reqs = dma_memory_map(as, req_gpa, &req_len, DMA_DIRECTION_TO_DEVICE,
                          MEMTXATTRS_UNSPECIFIED);
    if (!reqs || req_len < count * sizeof(AppleDMAUnmapReq)) {
        if (reqs) {
            dma_memory_unmap(as, reqs, req_len, DMA_DIRECTION_TO_DEVICE, 0);
        }
        s->last_status = APPLE_DMA_S_INVAL;
        return;
    }

    resps = dma_memory_map(as, resp_gpa, &resp_len, DMA_DIRECTION_FROM_DEVICE,
                           MEMTXATTRS_UNSPECIFIED);
    if (!resps || resp_len < count * sizeof(AppleDMAUnmapResp)) {
        if (resps) {
            dma_memory_unmap(as, resps, resp_len,
                             DMA_DIRECTION_FROM_DEVICE, 0);
        }
        dma_memory_unmap(as, reqs, req_len, DMA_DIRECTION_TO_DEVICE, 0);
        s->last_status = APPLE_DMA_S_INVAL;
        return;
    }

    for (i = 0; i < count; i++) {
        uint64_t id = le64_to_cpu(reqs[i].id);
        uint64_t size = le64_to_cpu(reqs[i].size);
        uint32_t status = apple_dma_backend_unmap(s, id, size);

        resps[i].id = cpu_to_le64(id);
        resps[i].status = cpu_to_le32(status);
        if (status != APPLE_DMA_S_OK) {
            ok = false;
        }
    }

    s->last_status = ok ? APPLE_DMA_S_OK : APPLE_DMA_S_IOERR;
    dma_memory_unmap(as, resps, resp_len, DMA_DIRECTION_FROM_DEVICE,
                     resp_len);
    dma_memory_unmap(as, reqs, req_len, DMA_DIRECTION_TO_DEVICE, 0);
}

static void apple_dma_doorbell(AppleDMAState *s)
{
    AddressSpace *as = &address_space_memory;
    uint8_t cmd_buf[CMD_PAGE_SIZE];
    uint32_t type, count;
    uint64_t req_gpa, resp_gpa;
    uint32_t le_status;

    if (!s->cmd_gpa) {
        s->last_status = APPLE_DMA_S_INVAL;
        return;
    }

    if (dma_memory_read(as, s->cmd_gpa, cmd_buf, CMD_PAGE_SIZE,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        s->last_status = APPLE_DMA_S_INVAL;
        return;
    }

    type = ldl_le_p(cmd_buf + CMD_OFF_TYPE);
    count = ldl_le_p(cmd_buf + CMD_OFF_COUNT);
    req_gpa = ldq_le_p(cmd_buf + CMD_OFF_REQ_GPA);
    resp_gpa = ldq_le_p(cmd_buf + CMD_OFF_RESP_GPA);

    if (!count || count > s->max_entries || !req_gpa || !resp_gpa) {
        s->last_status = APPLE_DMA_S_INVAL;
        goto write_status;
    }

    switch (type) {
    case APPLE_DMA_CMD_MAP:
        apple_dma_handle_map(s, req_gpa, resp_gpa, count);
        break;
    case APPLE_DMA_CMD_UNMAP:
        apple_dma_handle_unmap(s, req_gpa, resp_gpa, count);
        break;
    default:
        s->last_status = APPLE_DMA_S_INVAL;
        break;
    }

write_status:
    le_status = cpu_to_le32(s->last_status);
    dma_memory_write(as, s->cmd_gpa + CMD_OFF_STATUS, &le_status, 4,
                     MEMTXATTRS_UNSPECIFIED);
}

/* ------------------------------------------------------------------ */
/* MMIO BAR handlers                                                   */
/* ------------------------------------------------------------------ */

static uint64_t apple_dma_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleDMAState *s = opaque;

    switch (addr) {
    case APPLE_DMA_REG_VERSION:
        return APPLE_DMA_VERSION;
    case APPLE_DMA_REG_MANAGED_BDF:
        if (s->managed_dev) {
            return PCI_BUILD_BDF(pci_dev_bus_num(s->managed_dev),
                                 s->managed_dev->devfn);
        }
        return s->managed_bdf;
    case APPLE_DMA_REG_MAX_ENTRIES:
        return s->max_entries;
    case APPLE_DMA_REG_STATUS:
        return s->last_status;
    case APPLE_DMA_REG_FEATURES:
        return APPLE_DMA_FEATURE_MAPPING_ID;
    default:
        return 0;
    }
}

static void apple_dma_bar_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    AppleDMAState *s = opaque;

    switch (addr) {
    case APPLE_DMA_REG_CMD_GPA_LO:
        s->cmd_gpa = deposit64(s->cmd_gpa, 0, 32, val);
        break;
    case APPLE_DMA_REG_CMD_GPA_HI:
        s->cmd_gpa = deposit64(s->cmd_gpa, 32, 32, val);
        break;
    case APPLE_DMA_REG_DOORBELL:
        apple_dma_doorbell(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps apple_dma_bar_ops = {
    .read = apple_dma_bar_read,
    .write = apple_dma_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------ */
/* Dext connection                                                     */
/* ------------------------------------------------------------------ */

static bool apple_dma_connect_dext(AppleDMAState *s, Error **errp)
{
    io_connect_t conn;

    const char *root = (s->apple_host_root && s->apple_host_root[0])
                           ? s->apple_host_root : NULL;

    conn = apple_vfio_dext_acquire_dma(
        s->apple_host_bus, s->apple_host_device,
        s->apple_host_function, root, s->managed_bdf);
    if (conn == IO_OBJECT_NULL) {
        error_setg(errp,
                   "apple-dma: no available VFIO owner for host PCI "
                   "%02x:%02x.%x%s%s and guest BDF 0x%x; realize the matching "
                   "vfio-apple-pci first and use only one DMA companion",
                   s->apple_host_bus, s->apple_host_device,
                   s->apple_host_function, root ? "@" : "",
                   root ? root : "", s->managed_bdf);
        return false;
    }

    s->dext_conn = conn;
    return true;
}

/* ------------------------------------------------------------------ */
/* PCI device lifecycle                                                */
/* ------------------------------------------------------------------ */

static void apple_dma_pci_realize(PCIDevice *pdev, Error **errp)
{
    AppleDMAState *s = APPLE_DMA_PCI(pdev);
    int ret;

    if (s->apple_host_bus == UINT32_MAX ||
        s->apple_host_device == UINT32_MAX ||
        s->apple_host_function == UINT32_MAX) {
        error_setg(errp, "apple-dma: requires x-apple-host-bus, "
                   "x-apple-host-device, and x-apple-host-function");
        return;
    }

    if (!s->max_entries) {
        s->max_entries = APPLE_DMA_MAX_ENTRIES;
    }

    /*
     * Discard-based memory management must not assume it can reclaim pages
     * that DriverKit may pin. This does not synchronize concurrent discards
     * such as postcopy migration.
     */
    ret = ram_block_discard_disable(true);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "apple-dma: cannot prevent guest RAM discard");
        return;
    }
    s->discards_disabled = true;
    s->dma_mappings = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            NULL, g_free);

    if (!apple_dma_connect_dext(s, errp)) {
        g_clear_pointer(&s->dma_mappings, g_hash_table_destroy);
        ram_block_discard_disable(false);
        s->discards_disabled = false;
        return;
    }

    memory_region_init_io(&s->bar, OBJECT(s), &apple_dma_bar_ops, s,
                          "apple-dma-bar", APPLE_DMA_BAR_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
}

static void apple_dma_pci_exit(PCIDevice *pdev)
{
    AppleDMAState *s = APPLE_DMA_PCI(pdev);
    bool teardown_failed = false;

    if (s->dma_mappings) {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init(&iter, s->dma_mappings);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            AppleDMAMapping *mapping = value;
            kern_return_t kr = s->dext_conn == IO_OBJECT_NULL ||
                               mapping->ownership_unknown ?
                KERN_FAILURE :
                apple_host_backend()->unregister_dma(s->dext_conn,
                                                     mapping->id);

            if (kr == KERN_SUCCESS) {
                dma_memory_unmap(&address_space_memory, mapping->host_addr,
                                 mapping->mapped_len,
                                 DMA_DIRECTION_FROM_DEVICE,
                                 mapping->size);
                g_hash_table_iter_remove(&iter);
            } else {
                teardown_failed = true;
                error_report("apple-dma: teardown could not unregister DMA "
                             "mapping id=0x%" PRIx64
                             " (kr=0x%x); retaining connection, backing and "
                             "discard inhibition until process exit",
                             mapping->id, kr);
            }
        }
    }

    if (teardown_failed) {
        /*
         * Keep our connection reference and RAM mappings alive. A failed
         * CompleteDMA cannot be treated as permission to close the user
         * client or allow its backing to be discarded.
         */
        return;
    }

    if (s->dext_conn != IO_OBJECT_NULL) {
        const char *root = (s->apple_host_root && s->apple_host_root[0])
                               ? s->apple_host_root : NULL;
        apple_vfio_dext_release_dma(s->apple_host_bus, s->apple_host_device,
                                    s->apple_host_function, root,
                                    s->dext_conn);
        s->dext_conn = IO_OBJECT_NULL;
    }

    g_clear_pointer(&s->dma_mappings, g_hash_table_destroy);
    if (s->discards_disabled) {
        ram_block_discard_disable(false);
        s->discards_disabled = false;
    }
}

static const Property apple_dma_pci_properties[] = {
    DEFINE_PROP_UINT32("managed-bdf", AppleDMAState, managed_bdf, 0),
    DEFINE_PROP_LINK("x-apple-managed-dev", AppleDMAState, managed_dev,
                     TYPE_PCI_DEVICE, PCIDevice *),
    DEFINE_PROP_UINT32("max-entries", AppleDMAState, max_entries, 0),
    DEFINE_PROP_UINT32("x-apple-host-bus", AppleDMAState,
                       apple_host_bus, UINT32_MAX),
    DEFINE_PROP_UINT32("x-apple-host-device", AppleDMAState,
                       apple_host_device, UINT32_MAX),
    DEFINE_PROP_UINT32("x-apple-host-function", AppleDMAState,
                       apple_host_function, UINT32_MAX),
    DEFINE_PROP_STRING("x-apple-host-root", AppleDMAState, apple_host_root),
};

static void apple_dma_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

    pdc->realize = apple_dma_pci_realize;
    pdc->exit = apple_dma_pci_exit;
    pdc->vendor_id = PCI_VENDOR_ID_REDHAT;
    pdc->device_id = PCI_DEVICE_ID_REDHAT_APPLE_DMA;
    pdc->class_id = PCI_CLASS_SYSTEM_OTHER;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, apple_dma_pci_properties);
    dc->desc = "Apple DMA mapping device";
}

static const TypeInfo apple_dma_pci_info = {
    .name = TYPE_APPLE_DMA_PCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AppleDMAState),
    .class_init = apple_dma_pci_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void apple_dma_pci_register(void)
{
    type_register_static(&apple_dma_pci_info);
}

type_init(apple_dma_pci_register)
