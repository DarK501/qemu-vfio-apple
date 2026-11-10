/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * C client implementation for communicating with the VFIOUserPCIDriver dext
 * via IOKit IOUserClient.
 *
 * Copyright (c) 2026 Scott J. Goldman
 */

#include "qemu/osdep.h"

#include "apple-dext-client.h"
#include "qemu/error-report.h"
#include "qemu/log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    kSelectorGetIdentity        = 0,
    kSelectorClaim              = 1,
    kSelectorTerminate          = 2,
    kSelectorAllocateDMABuffer  = 3,
    kSelectorFreeDMABuffer      = 4,
    kSelectorRegisterDMARegion  = 5,
    kSelectorUnregisterDMARegion = 6,
    kSelectorProbeDMARegion     = 7,
    kSelectorConfigRead         = 8,
    kSelectorConfigWrite        = 9,
    kSelectorGetBARInfo         = 10,
    kSelectorMMIORead           = 11,
    kSelectorMMIOWrite          = 12,
    kSelectorSetupInterrupts    = 13,
    kSelectorCheckInterrupt     = 14,
    kSelectorWaitInterrupt      = 15,
    kSelectorSetIRQMask         = 16,
    kSelectorResetDevice        = 17,
    kSelectorReleaseClaim       = 20,
};

/*
 * Keep this in sync with PCIDriverKit BAR type encoding. Bit 3 indicates
 * prefetchability for memory BARs.
 */
#define APPLE_DEXT_BAR_PREFETCHABLE_MASK 0x08
#ifndef kIOMapWriteCombineCache
#define kIOMapWriteCombineCache 0x00000400
#endif

static bool
dext_service_matches_class(io_service_t service, const char *className)
{
    bool match = false;
    CFTypeRef ref;

    ref = IORegistryEntryCreateCFProperty(service, CFSTR("IOUserClass"),
                                          kCFAllocatorDefault, 0);
    if (ref == NULL) {
        return false;
    }

    if (CFGetTypeID(ref) == CFStringGetTypeID()) {
        CFStringRef expected = CFStringCreateWithCString(
            kCFAllocatorDefault, className, kCFStringEncodingUTF8);
        if (expected != NULL) {
            match = CFStringCompare((CFStringRef)ref, expected, 0)
                    == kCFCompareEqualTo;
            CFRelease(expected);
        }
    }
    CFRelease(ref);
    return match;
}

static bool
dext_connection_matches_bdf(io_connect_t connection,
                            uint8_t bus, uint8_t device, uint8_t function)
{
    uint64_t output[6] = {0};
    uint32_t outputCount = 6;
    kern_return_t kr;

    kr = IOConnectCallMethod(connection, kSelectorGetIdentity,
                             NULL, 0, NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS || outputCount < 3) {
        return false;
    }

    return (uint8_t)output[0] == bus &&
           (uint8_t)output[1] == device &&
           (uint8_t)output[2] == function;
}

/*
 * Walk up the IOService plane from `start` to the topmost IOPCIDevice
 * ancestor (the host PCI root) and copy its registry-entry name into
 * `out` (NUL-terminated, truncated to `out_size`).  Returns true on
 * success, false if no IOPCIDevice ancestor exists or `out` would be
 * empty.
 *
 * Each VFIOUserPCIDriver IOUserService entry sits as a child of the
 * IOPCIDevice it matched against, which itself hangs off intermediate
 * bridges and finally a root-port IOPCIDevice (e.g. "pci-bridge0",
 * "pcic0-bridge").  That root-port name is what users see in System
 * Information and is the disambiguator we expose as `host-root=`.
 */
static bool
dext_root_name(io_service_t start, char *out, size_t out_size)
{
    io_registry_entry_t topmost = IO_OBJECT_NULL;
    io_registry_entry_t current;
    io_name_t className;
    kern_return_t kr;

    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    current = start;
    IOObjectRetain(current);

    for (;;) {
        io_registry_entry_t parent = IO_OBJECT_NULL;

        if (IOObjectGetClass(current, className) == KERN_SUCCESS &&
            strcmp(className, "IOPCIDevice") == 0) {
            if (topmost != IO_OBJECT_NULL) {
                IOObjectRelease(topmost);
            }
            IOObjectRetain(current);
            topmost = current;
        }

        kr = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent);
        IOObjectRelease(current);
        if (kr != KERN_SUCCESS || parent == IO_OBJECT_NULL) {
            break;
        }
        current = parent;
    }

    if (topmost == IO_OBJECT_NULL) {
        return false;
    }

    io_name_t entryName;
    kr = IORegistryEntryGetName(topmost, entryName);
    IOObjectRelease(topmost);
    if (kr != KERN_SUCCESS || entryName[0] == '\0') {
        return false;
    }

    strncpy(out, entryName, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

/*
 * One candidate VFIOUserPCIDriver instance whose BDF matches the lookup.
 * We keep the IOUserService entry around (rather than the open connection)
 * so we can re-walk parents and only open the one we actually pick.
 */
typedef struct DextCandidate {
    io_service_t service;       /* retained */
    char         root[128];
} DextCandidate;

#define APPLE_DEXT_MAX_CANDIDATES 16

/*
 * Ask the PCI family to re-probe the device so the dext can attach, as the
 * host app's claim does.  Returns true once the request was accepted.
 */
static bool
dext_request_rescan(uint8_t bus, uint8_t device, uint8_t function)
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOPCIDevice");
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_service_t entry;
    bool requested = false;

    if (matching == NULL ||
        IOServiceGetMatchingServices(kIOMainPortDefault, matching,
                                     &iterator) != KERN_SUCCESS) {
        return false;
    }

    while (!requested && (entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef ref = IORegistryEntryCreateCFProperty(
            entry, CFSTR("pcidebug"), kCFAllocatorDefault, 0);
        if (ref != NULL) {
            char buf[64];
            unsigned b, d, f;
            if (CFGetTypeID(ref) == CFStringGetTypeID() &&
                CFStringGetCString((CFStringRef)ref, buf, sizeof(buf),
                                   kCFStringEncodingUTF8) &&
                sscanf(buf, "%u:%u:%u", &b, &d, &f) == 3 &&
                b == bus && d == device && f == function) {
                requested = IOServiceRequestProbe(entry, 0x80200000) ==
                            KERN_SUCCESS;
            }
            CFRelease(ref);
        }
        IOObjectRelease(entry);
    }
    IOObjectRelease(iterator);
    return requested;
}

static io_connect_t
dext_connect_once(uint8_t bus, uint8_t device, uint8_t function,
                  const char *host_root, char **errp, bool *no_candidates)
{
    CFMutableDictionaryRef matching;
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_connect_t result = IO_OBJECT_NULL;
    io_service_t service;
    kern_return_t kr;
    DextCandidate cands[APPLE_DEXT_MAX_CANDIDATES];
    size_t cand_count = 0;
    int chosen = -1;

    matching = IOServiceMatching("IOUserService");
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }

    kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }

    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        io_connect_t connection = IO_OBJECT_NULL;

        if (!dext_service_matches_class(service, "VFIOUserPCIDriver")) {
            IOObjectRelease(service);
            continue;
        }

        kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
        if (kr != KERN_SUCCESS) {
            IOObjectRelease(service);
            continue;
        }

        bool bdf_match = dext_connection_matches_bdf(connection, bus, device,
                                                     function);
        IOServiceClose(connection);

        if (!bdf_match) {
            IOObjectRelease(service);
            continue;
        }

        if (cand_count >= APPLE_DEXT_MAX_CANDIDATES) {
            IOObjectRelease(service);
            continue;
        }

        DextCandidate *c = &cands[cand_count++];
        c->service = service; /* keep retained */
        if (!dext_root_name(service, c->root, sizeof(c->root))) {
            strncpy(c->root, "?", sizeof(c->root) - 1);
            c->root[sizeof(c->root) - 1] = '\0';
        }
    }

    IOObjectRelease(iterator);

    if (cand_count == 0) {
        if (no_candidates != NULL) {
            *no_candidates = true;
        }
        if (errp != NULL && *errp == NULL) {
        *errp = strdup("no VFIOUserPCIDriver dext instance is bound to "
                       "that BDF — run `qemu-vfio-apple list-devices` to "
                       "see which devices the dext currently claims");
        }
        return IO_OBJECT_NULL;
    }

    if (host_root == NULL || host_root[0] == '\0') {
        if (cand_count == 1) {
            chosen = 0;
        } else if (errp != NULL) {
            /* Build "pcic0-bridge, pcic1-bridge" candidate list. */
            size_t list_cap = 0;
            for (size_t i = 0; i < cand_count; i++) {
                list_cap += strlen(cands[i].root) + 2; /* ", " */
            }
            char *list = (char *)calloc(list_cap + 1, 1);
            if (list != NULL) {
                for (size_t i = 0; i < cand_count; i++) {
                    if (i > 0) {
                        strcat(list, ", ");
                    }
                    strcat(list, cands[i].root);
                }
            }
            const char *fmt =
                "%02x:%02x.%x is claimed by %zu VFIOUserPCIDriver instances "
                "(%s); add host-root=<name> to disambiguate";
            int len = snprintf(NULL, 0, fmt,
                               bus, device, function,
                               cand_count, list ? list : "?");
            if (len > 0) {
                char *msg = (char *)malloc((size_t)len + 1);
                if (msg != NULL) {
                    snprintf(msg, (size_t)len + 1, fmt,
                             bus, device, function,
                             cand_count, list ? list : "?");
                    *errp = msg;
                }
            }
            free(list);
        }
    } else {
        for (size_t i = 0; i < cand_count; i++) {
            if (strcmp(cands[i].root, host_root) == 0) {
                chosen = (int)i;
                break;
            }
        }
        if (chosen < 0 && errp != NULL) {
            const char *fmt =
                "no VFIOUserPCIDriver instance for %02x:%02x.%x under root "
                "'%s' (found under: %s)";
            size_t list_cap = 0;
            for (size_t i = 0; i < cand_count; i++) {
                list_cap += strlen(cands[i].root) + 2;
            }
            char *list = (char *)calloc(list_cap + 1, 1);
            if (list != NULL) {
                for (size_t i = 0; i < cand_count; i++) {
                    if (i > 0) {
                        strcat(list, ", ");
                    }
                    strcat(list, cands[i].root);
                }
            }
            int len = snprintf(NULL, 0, fmt,
                               bus, device, function,
                               host_root, list ? list : "?");
            if (len > 0) {
                char *msg = (char *)malloc((size_t)len + 1);
                if (msg != NULL) {
                    snprintf(msg, (size_t)len + 1, fmt,
                             bus, device, function,
                             host_root, list ? list : "?");
                    *errp = msg;
                }
            }
            free(list);
        }
    }

    if (chosen >= 0) {
        kr = IOServiceOpen(cands[chosen].service, mach_task_self(), 0,
                           &result);
        if (kr != KERN_SUCCESS) {
            result = IO_OBJECT_NULL;
        }
    }

    for (size_t i = 0; i < cand_count; i++) {
        IOObjectRelease(cands[i].service);
    }

    return result;
}

io_connect_t
apple_dext_client_connect_with_root(uint8_t bus, uint8_t device, uint8_t function,
                             const char *host_root, char **errp)
{
    bool no_candidates = false;
    io_connect_t conn = dext_connect_once(bus, device, function, host_root,
                                          errp, &no_candidates);

    if (conn != IO_OBJECT_NULL || !no_candidates ||
        !dext_request_rescan(bus, device, function)) {
        return conn;
    }

    /* Rescan accepted: wait for the dext to attach, then retry. */
    for (int i = 0; i < 100 && conn == IO_OBJECT_NULL; i++) {
        usleep(50000);
        bool none = false;
        char *retry_err = NULL;
        conn = dext_connect_once(bus, device, function, host_root,
                                 &retry_err, &none);
        if (conn == IO_OBJECT_NULL && !none) {
            /* attached but another failure: report it */
            if (errp != NULL) {
                free(*errp);
                *errp = retry_err;
                retry_err = NULL;
            }
            break;
        }
        free(retry_err);
    }
    return conn;
}

io_connect_t
apple_dext_client_connect(uint8_t bus, uint8_t device, uint8_t function)
{
    return apple_dext_client_connect_with_root(bus, device, function, NULL, NULL);
}

kern_return_t
apple_dext_client_disconnect(io_connect_t connection)
{
    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }
    return IOServiceClose(connection);
}

kern_return_t
apple_dext_client_claim(io_connect_t connection)
{
    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection,
                               kSelectorClaim,
                               NULL, 0, NULL, 0,
                               NULL, NULL, NULL, NULL);
}

kern_return_t
apple_dext_client_release_claim(io_connect_t connection)
{
    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection, kSelectorReleaseClaim,
                               NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
}

kern_return_t
apple_dext_client_register_dma(io_connect_t connection,
                            uint64_t iova,
                            uint64_t client_va,
                            uint64_t size,
                            AppleDMAMapResult *out_result,
                            AppleDMARegistrationState *out_state)
{
    uint64_t input[3] = { iova, client_va, size };
    size_t outputSize = sizeof(*out_result);
    kern_return_t kr;

    if (out_state == NULL) {
        return kIOReturnBadArgument;
    }
    *out_state = APPLE_DMA_REGISTRATION_NONE;
    if (connection == IO_OBJECT_NULL || out_result == NULL) {
        return kIOReturnBadArgument;
    }

    *out_state = APPLE_DMA_REGISTRATION_UNKNOWN;
    kr = IOConnectCallMethod(connection,
                             kSelectorRegisterDMARegion,
                             input, 3,
                             NULL, 0,
                             NULL, NULL,
                             out_result, &outputSize);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    if (outputSize != sizeof(*out_result) ||
        out_result->mapping_id != iova) {
        return kIOReturnNotAligned;
    }
    if (out_result->status == APPLE_DMA_MAP_STATUS_ERROR &&
        out_result->segment_count == 0) {
        *out_state = APPLE_DMA_REGISTRATION_NONE;
        return kIOReturnError;
    }
    if (out_result->status == APPLE_DMA_MAP_STATUS_RETAINED &&
        out_result->segment_count == 0) {
        *out_state = APPLE_DMA_REGISTRATION_OWNED;
        return kIOReturnStillOpen;
    }

    if (out_result->status != APPLE_DMA_MAP_STATUS_OK ||
        out_result->segment_count == 0 ||
        out_result->segment_count > APPLE_DMA_MAX_SEGMENTS) {
        return kIOReturnNotAligned;
    }

    uint64_t covered = 0;
    for (uint32_t i = 0; i < out_result->segment_count; i++) {
        AppleDMASegment *segment = &out_result->segments[i];

        if (!segment->length || covered > size ||
            segment->offset != covered ||
            segment->length > size - covered ||
            segment->length - 1 > UINT64_MAX - segment->address) {
            return kIOReturnNotAligned;
        }
        covered += segment->length;
    }
    if (covered != size) {
        return kIOReturnNotAligned;
    }

    *out_state = APPLE_DMA_REGISTRATION_OWNED;
    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_allocate_dma_buffer(io_connect_t connection,
                               uint64_t size,
                               uint64_t alignment,
                               uint64_t *out_bus_addr,
                               uint64_t *out_bus_len,
                               mach_vm_address_t *out_addr,
                               mach_vm_size_t *out_size)
{
    uint64_t input[2] = { size, alignment };
    uint64_t output[6] = {0};
    uint32_t outputCount = 6;
    kern_return_t kr;
    mach_vm_address_t addr = 0;
    mach_vm_size_t map_size = 0;
    bool mapped = false;

    if (connection == IO_OBJECT_NULL || size == 0 ||
        ((out_addr == NULL) != (out_size == NULL))) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorAllocateDMABuffer,
                             input, 2,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    /* A successful allocation must be explicitly released on later errors. */
    if (outputCount != 6 || output[0] != size || output[2] == 0 ||
        output[2] > 32 || output[4] == 0 || output[4] > size) {
        error_report("vfio-apple: invalid DMA allocation reply");
        kr = kIOReturnError;
        goto rollback;
    }

    if (out_addr != NULL && out_size != NULL) {
        /* Memory type 0 = kVFIOUserPCIDriverUserClientMemoryTypeDMABuffer */
        kr = IOConnectMapMemory64(connection, 0, mach_task_self(),
                                  &addr, &map_size, kIOMapAnywhere);
        if (kr != KERN_SUCCESS) {
            addr = 0;
            goto rollback;
        }
        mapped = true;
        if (addr == 0 || map_size < size) {
            error_report("vfio-apple: invalid DMA buffer CPU mapping "
                         "(address=0x%" PRIx64 ", size=%" PRIu64 ")",
                         (uint64_t)addr, (uint64_t)map_size);
            kr = kIOReturnError;
            goto rollback;
        }
        *out_addr = addr;
        *out_size = map_size;
    }

    if (out_bus_addr != NULL) {
        *out_bus_addr = output[3];
    }
    if (out_bus_len != NULL) {
        *out_bus_len = output[4];
    }
    return kIOReturnSuccess;

rollback: {
        kern_return_t cleanup;
        if (mapped && addr == 0) {
            cleanup = IOConnectUnmapMemory64(connection, 0, mach_task_self(),
                                            addr);
            if (cleanup == KERN_SUCCESS) {
                cleanup = apple_dext_client_free_dma_buffer(connection, 0);
            }
        } else {
            cleanup = apple_dext_client_free_dma_buffer(connection, addr);
        }
        if (cleanup != KERN_SUCCESS) {
            error_report("vfio-apple: DMA allocation rollback failed "
                         "(allocation error=0x%x, cleanup error=0x%x); "
                         "device shutdown is not guaranteed safe", kr, cleanup);
            return cleanup;
        }
        return kr;
    }
}

kern_return_t
apple_dext_client_free_dma_buffer(io_connect_t connection,
                           mach_vm_address_t addr)
{
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    if (addr != 0) {
        kr = IOConnectUnmapMemory64(connection, 0, mach_task_self(), addr);
        if (kr != KERN_SUCCESS) {
            return kr;
        }
    }
    return IOConnectCallMethod(connection, kSelectorFreeDMABuffer,
                               NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
}

kern_return_t
apple_dext_client_unregister_dma(io_connect_t connection,
                              uint64_t iova)
{
    uint64_t input[1] = { iova };

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection,
                               kSelectorUnregisterDMARegion,
                               input, 1,
                               NULL, 0,
                               NULL, NULL,
                               NULL, NULL);
}

kern_return_t
apple_dext_client_probe_dma(io_connect_t connection,
                         uint64_t iova,
                         uint64_t offset,
                         uint64_t *out_word)
{
    uint64_t input[2] = { iova, offset };
    uint64_t output[1] = {0};
    uint32_t outputCount = 1;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL || out_word == NULL) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorProbeDMARegion,
                             input, 2,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    *out_word = output[0];
    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_config_read(io_connect_t connection,
                           uint64_t offset,
                           uint64_t width,
                           uint64_t *out_value)
{
    uint64_t input[2] = { offset, width };
    uint64_t output[1] = {0};
    uint32_t outputCount = 1;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL || out_value == NULL) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorConfigRead,
                             input, 2,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    *out_value = output[0];
    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_config_write(io_connect_t connection,
                            uint64_t offset,
                            uint64_t width,
                            uint64_t value)
{
    uint64_t input[3] = { offset, width, value };

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection,
                               kSelectorConfigWrite,
                               input, 3,
                               NULL, 0,
                               NULL, NULL,
                               NULL, NULL);
}

kern_return_t
apple_dext_client_config_read_block(io_connect_t connection,
                                 uint64_t offset,
                                 void *buf,
                                 size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    uint64_t pos = offset;
    size_t remaining = len;

    if (connection == IO_OBJECT_NULL || buf == NULL) {
        return kIOReturnBadArgument;
    }

    while (remaining >= 4) {
        uint64_t val = 0;
        uint32_t dword;
        kern_return_t kr;

        kr = apple_dext_client_config_read(connection, pos, 4, &val);
        if (kr != KERN_SUCCESS) {
            return kr;
        }
        dword = (uint32_t)val;
        memcpy(dst, &dword, 4);
        dst += 4;
        pos += 4;
        remaining -= 4;
    }

    while (remaining > 0) {
        uint64_t val = 0;
        kern_return_t kr;

        kr = apple_dext_client_config_read(connection, pos, 1, &val);
        if (kr != KERN_SUCCESS) {
            return kr;
        }
        *dst = (uint8_t)val;
        dst++;
        pos++;
        remaining--;
    }

    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_get_bar_info(io_connect_t connection,
                            uint8_t bar,
                            uint8_t *out_mem_idx,
                            uint64_t *out_size,
                            uint8_t *out_type)
{
    uint64_t input[1] = { bar };
    uint64_t output[3] = {0};
    uint32_t outputCount = 3;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorGetBARInfo,
                             input, 1,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    if (out_mem_idx != NULL) {
        *out_mem_idx = (uint8_t)output[0];
    }
    if (out_size != NULL) {
        *out_size = output[1];
    }
    if (out_type != NULL) {
        *out_type = (uint8_t)output[2];
    }

    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_map_bar(io_connect_t connection,
                       uint8_t bar,
                       mach_vm_address_t *out_addr,
                       mach_vm_size_t *out_size,
                       uint8_t *out_type)
{
    uint64_t bar_size = 0;
    uint8_t bar_type = 0;
    uint32_t mem_type;
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    IOOptionBits opts = kIOMapAnywhere;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL || out_addr == NULL || out_size == NULL) {
        return kIOReturnBadArgument;
    }

    kr = apple_dext_client_get_bar_info(connection, bar, NULL,
                                     &bar_size, &bar_type);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    /*
     * The memory type for IOConnectMapMemory64 must match the dext's
     * CopyClientMemoryForType expectation:
     * kVFIOUserPCIDriverUserClientMemoryTypeBAR0 (= 1) plus the BAR index.
     * This is NOT the same as the PCIDriverKit internal memoryIndex returned
     * by GetBARInfo.
     */
    mem_type = 1 + (uint32_t)bar;

    /*
     * Diagnostic: VFIO_APPLE_BAR_CACHE=default maps prefetchable BARs with
     * the default cache mode instead of write-combining.
     */
    if ((bar_type & APPLE_DEXT_BAR_PREFETCHABLE_MASK) &&
        g_strcmp0(getenv("VFIO_APPLE_BAR_CACHE"), "default") != 0) {
        opts |= kIOMapWriteCombineCache;
    }

    kr = IOConnectMapMemory64(connection, mem_type, mach_task_self(),
                              &addr, &size, opts);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    *out_addr = addr;
    *out_size = size;
    if (out_type != NULL) {
        *out_type = bar_type;
    }
    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_unmap_bar(io_connect_t connection,
                         uint8_t bar,
                         mach_vm_address_t addr)
{
    uint32_t mem_type = 1 + (uint32_t)bar;

    if (connection == IO_OBJECT_NULL || addr == 0) {
        return kIOReturnBadArgument;
    }

    return IOConnectUnmapMemory64(connection, mem_type, mach_task_self(), addr);
}

kern_return_t
apple_dext_client_mmio_read(io_connect_t connection,
                         uint8_t mem_idx,
                         uint64_t offset,
                         uint64_t width,
                         uint64_t *out_value)
{
    uint64_t input[3] = { mem_idx, offset, width };
    uint64_t output[1] = {0};
    uint32_t outputCount = 1;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL || out_value == NULL) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorMMIORead,
                             input, 3,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    *out_value = output[0];
    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_mmio_write(io_connect_t connection,
                          uint8_t mem_idx,
                          uint64_t offset,
                          uint64_t width,
                          uint64_t value)
{
    uint64_t input[4] = { mem_idx, offset, width, value };

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection,
                               kSelectorMMIOWrite,
                               input, 4,
                               NULL, 0,
                               NULL, NULL,
                               NULL, NULL);
}

kern_return_t
apple_dext_client_setup_interrupts(io_connect_t connection,
                                uint32_t *out_num_vectors)
{
    uint64_t output[1] = {0};
    uint32_t outputCount = 1;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorSetupInterrupts,
                             NULL, 0,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }

    if (outputCount != 1 || output[0] == 0 || output[0] > 256) {
        return kIOReturnBadArgument;
    }

    if (out_num_vectors != NULL) {
        *out_num_vectors = (uint32_t)output[0];
    }

    return kIOReturnSuccess;
}

kern_return_t
apple_dext_client_reset_device(io_connect_t connection)
{
    if (connection == IO_OBJECT_NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection,
                               kSelectorResetDevice,
                               NULL, 0, NULL, 0,
                               NULL, NULL, NULL, NULL);
}

kern_return_t
apple_dext_client_set_irq_mask(io_connect_t connection, const uint64_t mask[4])
{
    if (connection == IO_OBJECT_NULL || mask == NULL) {
        return kIOReturnBadArgument;
    }

    return IOConnectCallMethod(connection,
                               kSelectorSetIRQMask,
                               mask, 4,
                               NULL, 0,
                               NULL, NULL,
                               NULL, NULL);
}

kern_return_t
apple_dext_client_read_pending_irqs(io_connect_t connection, uint64_t pending[4])
{
    uint64_t output[4] = {0};
    uint32_t outputCount = 4;
    kern_return_t kr;
    uint32_t i;

    if (connection == IO_OBJECT_NULL || pending == NULL) {
        return kIOReturnBadArgument;
    }

    kr = IOConnectCallMethod(connection,
                             kSelectorCheckInterrupt,
                             NULL, 0,
                             NULL, 0,
                             output, &outputCount,
                             NULL, NULL);
    if (kr != KERN_SUCCESS) {
        return kr;
    }
    if (outputCount != 4) {
        return kIOReturnBadArgument;
    }

    for (i = 0; i < 4; i++) {
        pending[i] = output[i];
    }

    return kIOReturnSuccess;
}

struct AppleDextInterruptNotify {
    io_connect_t connection;
    IONotificationPortRef notifyPort;
    mach_port_t machPort;
    dispatch_queue_t dispatchQueue;
    void (*handler_fn)(void *opaque);
    void *opaque;
};

static void
apple_dext_client_async_callback(void *refcon, IOReturn result,
                          void **args, uint32_t numArgs)
{
    AppleDextInterruptNotify *notify = refcon;

    if (result == kIOReturnSuccess && notify->handler_fn) {
        notify->handler_fn(notify->opaque);
    } else if (result != kIOReturnSuccess) {
        error_report("vfio-apple: asynchronous interrupt notification failed "
                     "(kr=0x%x); future interrupts may be lost", result);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vfio-apple: asynchronous interrupt notification failed "
                      "(kr=0x%x); future interrupts may be lost\n", result);
    }
}

static kern_return_t
apple_dext_client_interrupt_notify_arm(AppleDextInterruptNotify *notify)
{
    uint64_t asyncRef[kIOAsyncCalloutCount];

    asyncRef[kIOAsyncCalloutFuncIndex] =
        (uint64_t)(uintptr_t)apple_dext_client_async_callback;
    asyncRef[kIOAsyncCalloutRefconIndex] =
        (uint64_t)(uintptr_t)notify;

    return IOConnectCallAsyncMethod(notify->connection,
                                    kSelectorWaitInterrupt,
                                    notify->machPort,
                                    asyncRef, kIOAsyncCalloutCount,
                                    NULL, 0, NULL, 0,
                                    NULL, NULL, NULL, NULL);
}

AppleDextInterruptNotify *
apple_dext_client_interrupt_notify_create(io_connect_t connection,
                                   void (*handler_fn)(void *opaque),
                                   void *opaque)
{
    AppleDextInterruptNotify *notify;
    kern_return_t kr;

    if (connection == IO_OBJECT_NULL || handler_fn == NULL) {
        return NULL;
    }

    notify = g_new0(AppleDextInterruptNotify, 1);
    notify->connection = connection;
    notify->handler_fn = handler_fn;
    notify->opaque = opaque;

    notify->notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (!notify->notifyPort) {
        g_free(notify);
        return NULL;
    }

    notify->dispatchQueue = dispatch_queue_create(
        "org.qemu.vfio-apple.irq-notify", DISPATCH_QUEUE_SERIAL);
    IONotificationPortSetDispatchQueue(notify->notifyPort,
                                       notify->dispatchQueue);
    notify->machPort = IONotificationPortGetMachPort(notify->notifyPort);

    kr = apple_dext_client_interrupt_notify_arm(notify);
    if (kr != KERN_SUCCESS) {
        IONotificationPortDestroy(notify->notifyPort);
        dispatch_release(notify->dispatchQueue);
        g_free(notify);
        return NULL;
    }

    return notify;
}

kern_return_t
apple_dext_client_interrupt_notify_rearm(AppleDextInterruptNotify *notify)
{
    if (!notify) {
        return kIOReturnBadArgument;
    }
    return apple_dext_client_interrupt_notify_arm(notify);
}

void
apple_dext_client_interrupt_notify_destroy(AppleDextInterruptNotify *notify)
{
    if (!notify) {
        return;
    }

    IONotificationPortDestroy(notify->notifyPort);
    dispatch_release(notify->dispatchQueue);
    g_free(notify);
}
