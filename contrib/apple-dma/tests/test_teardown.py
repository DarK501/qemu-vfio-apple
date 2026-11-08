#!/usr/bin/env python3
"""Exercise the QEMU companion's teardown code with a stub backend."""

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

typedef int kern_return_t;
typedef void *gpointer;
enum { KERN_SUCCESS = 0, KERN_FAILURE = 1, IO_OBJECT_NULL = 0,
       DMA_DIRECTION_FROM_DEVICE = 1, APPLE_DMA_S_OK = 0,
       APPLE_DMA_S_IOERR = 1 };
typedef struct AppleDMAMapping {
    uint64_t id;
    void *host_addr;
    uint64_t mapped_len, size;
    bool ownership_unknown;
} AppleDMAMapping;
typedef struct GHashTable {
    AppleDMAMapping *entries[2];
} GHashTable;
typedef struct GHashTableIter {
    GHashTable *table;
    int next, current;
} GHashTableIter;
typedef struct AppleDMAState {
    GHashTable *dma_mappings;
    int dext_conn;
    bool discards_disabled;
    unsigned apple_host_bus, apple_host_device, apple_host_function;
    char *apple_host_root;
} AppleDMAState;
typedef AppleDMAState PCIDevice;
#define APPLE_DMA_PCI(p) (p)
#define error_report(...) ((void)0)
#define g_clear_pointer(p, destroy) do { \
    if (*(p)) { destroy(*(p)); *(p) = NULL; } \
} while (0)
static int unmaps, unregisters, releases, discard_enables, destroys;
static uint64_t failing_id;
static int address_space_memory;
static void g_hash_table_iter_init(GHashTableIter *it, GHashTable *table)
{
    it->table = table;
    it->next = 0;
}
static bool g_hash_table_iter_next(GHashTableIter *it, gpointer *key,
                                   gpointer *value)
{
    while (it->next < 2) {
        it->current = it->next++;
        if (it->table->entries[it->current]) {
            *key = *value = it->table->entries[it->current];
            return true;
        }
    }
    return false;
}
static void g_hash_table_iter_remove(GHashTableIter *it)
{
    it->table->entries[it->current] = NULL;
}
static void g_hash_table_destroy(GHashTable *table) { (void)table; destroys++; }
static AppleDMAMapping *g_hash_table_lookup(GHashTable *table, uint64_t *id)
{
    for (int i = 0; i < 2; i++) {
        if (table->entries[i] && table->entries[i]->id == *id) {
            return table->entries[i];
        }
    }
    return NULL;
}
static void g_hash_table_remove(GHashTable *table, uint64_t *id)
{
    for (int i = 0; i < 2; i++) {
        if (table->entries[i] && table->entries[i]->id == *id) {
            table->entries[i] = NULL;
            return;
        }
    }
    assert(false);
}
static void dma_memory_unmap(void *as, void *addr, uint64_t len, int dir,
                             uint64_t size)
{
    (void)addr;
    assert(as == &address_space_memory && len == 4096 && size == 4096);
    assert(dir == DMA_DIRECTION_FROM_DEVICE);
    unmaps++;
}
static int ram_block_discard_disable(bool state)
{
    assert(!state);
    discard_enables++;
    return 0;
}
static kern_return_t unregister_dma(int conn, uint64_t id)
{
    assert(conn == 7);
    unregisters++;
    return id == failing_id ? KERN_FAILURE : KERN_SUCCESS;
}
static void apple_vfio_dext_release_dma(unsigned bus, unsigned dev, unsigned fn,
                                        const char *root, int conn)
{
    (void)bus; (void)dev; (void)fn; (void)root;
    assert(conn == 7);
    releases++;
}
static const struct Backend {
    kern_return_t (*unregister_dma)(int, uint64_t);
} backend = { unregister_dma };
static const struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
static void run_unmap(void)
{
    AppleDMAMapping mapping = {
        .id = 0, .mapped_len = 4096, .size = 4096,
        .ownership_unknown = true,
    };
    GHashTable table = { .entries = { &mapping, NULL } };
    AppleDMAState state = { .dma_mappings = &table, .dext_conn = 7 };
    unmaps = unregisters = 0;
    failing_id = 1;
    assert(apple_dma_backend_unmap(&state, 0, 4096) == APPLE_DMA_S_IOERR);
    assert(!unregisters && !unmaps && table.entries[0] == &mapping);
    mapping.ownership_unknown = false;
    assert(apple_dma_backend_unmap(&state, 0, 2048) == APPLE_DMA_S_IOERR);
    assert(!unregisters && !unmaps);
    failing_id = 0;
    assert(apple_dma_backend_unmap(&state, 0, 4096) == APPLE_DMA_S_IOERR);
    assert(unregisters == 1 && !unmaps && table.entries[0] == &mapping);
    failing_id = 1;
    assert(apple_dma_backend_unmap(&state, 0, 4096) == APPLE_DMA_S_OK);
    assert(unregisters == 2 && unmaps == 1 && table.entries[0] == NULL);
}
static void run(uint64_t failed_mapping, bool unknown)
{
    AppleDMAMapping first = { .id = 1, .mapped_len = 4096, .size = 4096 };
    AppleDMAMapping second = { .id = 2, .mapped_len = 4096, .size = 4096 };
    GHashTable table = { .entries = { &first, &second } };
    AppleDMAState state = {
        .dma_mappings = &table, .dext_conn = 7,
        .discards_disabled = true,
    };
    unmaps = unregisters = releases = discard_enables = destroys = 0;
    first.ownership_unknown = unknown && failed_mapping == 1;
    second.ownership_unknown = unknown && failed_mapping == 2;
    failing_id = unknown ? 0 : failed_mapping;
    apple_dma_pci_exit(&state);
    if (failed_mapping) {
        assert(unmaps == 1);
        assert(unregisters == (unknown ? 1 : 2));
        assert(table.entries[0] == (failed_mapping == 1 ? &first : NULL));
        assert(table.entries[1] == (failed_mapping == 2 ? &second : NULL));
        assert(state.dma_mappings == &table);
        assert(state.dext_conn == 7 && state.discards_disabled);
        assert(!releases && !discard_enables && !destroys);
    } else {
        assert(unmaps == 2 && state.dma_mappings == NULL);
        assert(unregisters == 2);
        assert(state.dext_conn == IO_OBJECT_NULL && !state.discards_disabled);
        assert(discard_enables == 1 && destroys == 1);
        assert(releases == 1);
    }
}
int main(void)
{
    run_unmap();
    run(0, false);
    run(1, false);
    run(2, false);
    run(1, true);
    run(2, true);
    return 0;
}
"""


def main():
    source = SOURCE.read_text()
    unmap_start = source.index("static uint32_t apple_dma_backend_unmap(")
    unmap_end = source.index("\n/* ------------------------------------------------------------------ */",
                             unmap_start)
    start = source.index("static void apple_dma_pci_exit(")
    end = source.index("\nstatic const Property apple_dma_pci_properties", start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "teardown.c"
        binary = pathlib.Path(directory) / "teardown"
        test.write_text(PREFIX + source[unmap_start:unmap_end] +
                        source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
