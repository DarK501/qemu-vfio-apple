#!/usr/bin/env python3
"""Exercise the companion's map ownership and VFIO-owner checks."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dma.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "contrib/apple-dma/apple-dma-protocol.h"

typedef int kern_return_t;
typedef uint64_t hwaddr;
typedef uint64_t ram_addr_t;
typedef struct AppleDMAMapping {
    uint64_t id;
    void *host_addr;
    hwaddr mapped_len;
    uint64_t size;
    bool ownership_unknown;
    struct AppleDMAMapping *next;
} AppleDMAMapping;
typedef enum AppleDMARegistrationState {
    APPLE_DMA_REGISTRATION_NONE,
    APPLE_DMA_REGISTRATION_OWNED,
    APPLE_DMA_REGISTRATION_UNKNOWN,
} AppleDMARegistrationState;
typedef struct AppleDMAState {
    int dext_conn;
    unsigned apple_host_bus, apple_host_device, apple_host_function;
    char *apple_host_root;
    AppleDMAMapping *dma_mappings;
} AppleDMAState;
enum { KERN_SUCCESS = 0, IO_OBJECT_NULL = 0,
       DMA_DIRECTION_FROM_DEVICE = 1, MEMTXATTRS_UNSPECIFIED = 0,
       APPLE_DMA_S_IOERR = 1, APPLE_DMA_S_OK = 0 };
#define error_report(...) ((void)0)
#define g_try_new0(type, count) ((type *)calloc((count), sizeof(type)))
#define g_free free
static int address_space_memory, mapped_byte;
static bool owner = true, direct = true, short_map;
static int register_calls, unmaps, ret;
static AppleDMARegistrationState response_state;
static bool apple_vfio_dext_dma_owner_attached(unsigned bus, unsigned dev,
    unsigned fn, const char *root, int conn)
{
    (void)bus; (void)dev; (void)fn; (void)root;
    assert(conn == 7);
    return owner;
}
static AppleDMAMapping *g_hash_table_lookup(AppleDMAMapping *mapping,
                                           const uint64_t *id)
{
    for (; mapping; mapping = mapping->next)
        if (mapping->id == *id) return mapping;
    return NULL;
}
static AppleDMAMapping **current_slot;
static void insert_mapping(AppleDMAMapping *table, uint64_t *key,
                           AppleDMAMapping *value)
{
    (void)table;
    assert(*key == value->id);
    value->next = *current_slot;
    *current_slot = value;
}
#define g_hash_table_insert insert_mapping
static void remove_mapping(AppleDMAMapping *table, const uint64_t *id)
{
    (void)table;
    AppleDMAMapping **item = current_slot;
    while (*item && (*item)->id != *id) item = &(*item)->next;
    assert(*item);
    AppleDMAMapping *old = *item;
    *item = old->next;
    free(old);
}
#define g_hash_table_remove remove_mapping
static void *dma_memory_map(void *as, uint64_t gpa, hwaddr *len, int dir,
                            int attrs)
{
    assert(as == &address_space_memory && gpa == 0 &&
           (*len == 4096 || *len == 8192));
    assert(dir == DMA_DIRECTION_FROM_DEVICE && attrs == MEMTXATTRS_UNSPECIFIED);
    if (short_map) {
        *len = 2048;
    }
    return &mapped_byte;
}
static bool memory_region_from_host(void *address, ram_addr_t *offset)
{
    assert(address == &mapped_byte);
    *offset = 0;
    return direct;
}
static void dma_memory_unmap(void *as, void *addr, hwaddr len, int dir,
                             uint64_t accessed)
{
    assert(as == &address_space_memory && addr == &mapped_byte);
    assert((len == 4096 || len == 2048 || len == 8192) &&
           dir == DMA_DIRECTION_FROM_DEVICE &&
           (accessed == 0 || accessed == len));
    unmaps++;
}
static kern_return_t register_dma(int conn, uint64_t id, uint64_t address,
                                  uint64_t size, AppleDMAMapResult *result,
                                  AppleDMARegistrationState *state)
{
    assert(conn == 7 && address == (uint64_t)&mapped_byte);
    assert(size == 4096 || size == 8192);
    register_calls++;
    *state = response_state;
    result->mapping_id = id;
    result->segment_count = 1;
    result->segments[0].address = 0x9000;
    result->segments[0].length = size;
    return ret;
}
static kern_return_t unregister_dma(int conn, uint64_t id)
{
    assert(conn == 7 && g_hash_table_lookup(*current_slot, &id));
    return 0;
}
static const struct Backend {
    kern_return_t (*register_dma)(int, uint64_t, uint64_t, uint64_t,
                                   AppleDMAMapResult *,
                                   AppleDMARegistrationState *);
    kern_return_t (*unregister_dma)(int, uint64_t);
} backend = { register_dma, unregister_dma };
static const struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
static void run(int status, AppleDMARegistrationState state, uint32_t expected)
{
    AppleDMAState s = { .dext_conn = 7 };
    AppleDMAMapResult result = {0};
    current_slot = &s.dma_mappings;
    register_calls = unmaps = 0;
    ret = status;
    response_state = state;
    assert(apple_dma_backend_map(&s, 0, 0, 4096, &result) == expected);
    if (!owner || !direct || short_map) {
        assert(!register_calls && !s.dma_mappings);
        assert(unmaps == (owner ? 1 : 0));
    } else {
        assert(register_calls == 1);
        if (expected == APPLE_DMA_MAP_STATUS_ERROR) {
            assert(!s.dma_mappings && unmaps == 1);
        } else {
            assert(s.dma_mappings && s.dma_mappings->id == 0 && !unmaps);
            assert(s.dma_mappings->ownership_unknown ==
                   (state == APPLE_DMA_REGISTRATION_UNKNOWN));
            assert(result.mapping_id == 0);
            assert(apple_dma_backend_map(&s, 0, 0, 4096, &result) ==
                   APPLE_DMA_MAP_STATUS_ERROR);
            assert(register_calls == 1);
            free(s.dma_mappings);
        }
    }
}
int main(void)
{
    AppleDMAState s = { .dext_conn = 7 };
    AppleDMAMapResult first = {0}, second = {0};
    uint64_t id1 = APPLE_DMA_MAP_ID_PREFIX | 1;
    uint64_t id2 = APPLE_DMA_MAP_ID_PREFIX | 2;
    owner = false;
    run(0, APPLE_DMA_REGISTRATION_OWNED, APPLE_DMA_MAP_STATUS_ERROR);
    owner = true;
    direct = false;
    run(0, APPLE_DMA_REGISTRATION_OWNED, APPLE_DMA_MAP_STATUS_ERROR);
    direct = true;
    short_map = true;
    run(0, APPLE_DMA_REGISTRATION_OWNED, APPLE_DMA_MAP_STATUS_ERROR);
    short_map = false;
    run(1, APPLE_DMA_REGISTRATION_NONE, APPLE_DMA_MAP_STATUS_ERROR);
    run(1, APPLE_DMA_REGISTRATION_UNKNOWN, APPLE_DMA_MAP_STATUS_RETAINED);
    run(1, APPLE_DMA_REGISTRATION_OWNED, APPLE_DMA_MAP_STATUS_RETAINED);
    run(0, APPLE_DMA_REGISTRATION_OWNED, APPLE_DMA_MAP_STATUS_OK);
    current_slot = &s.dma_mappings;
    assert(apple_dma_backend_map(&s, 0, id1, 4096, &first) == APPLE_DMA_MAP_STATUS_OK);
    assert(apple_dma_backend_map(&s, 0, id2, 8192, &second) == APPLE_DMA_MAP_STATUS_OK);
    assert(first.mapping_id == id1 && second.mapping_id == id2);
    assert(apple_dma_backend_unmap(&s, id1, 4096) == APPLE_DMA_S_OK);
    assert(g_hash_table_lookup(s.dma_mappings, &id2));
    assert(apple_dma_backend_unmap(&s, id2, 8192) == APPLE_DMA_S_OK);
    assert(!s.dma_mappings);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static uint32_t apple_dma_backend_map(")
    end = source.index("\n/* ------------------------------------------------------------------ */", start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "mapping.c"
        binary = pathlib.Path(directory) / "mapping"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(test), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
