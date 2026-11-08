#!/usr/bin/env python3
"""Exercise QEMU's shared DriverKit connection ownership with stub GLib."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int io_connect_t;
typedef int kern_return_t;
typedef char gchar;
enum { IO_OBJECT_NULL = 0, KERN_SUCCESS = 0, kIOReturnStillOpen = 8 };
typedef struct AppleVFIOSharedDext {
    io_connect_t conn;
    uint32_t refs;
    uint32_t managed_bdf;
    bool owner_attached;
    bool dma_attached;
} AppleVFIOSharedDext;
typedef struct GHashTable {
    char *keys[8];
    void *values[8];
} GHashTable;
static GHashTable *apple_vfio_shared_dexts;
static int disconnects, releases, errors, release_result, close_result;
static int pending_releases, sleeps;
#define g_new0(type, count) ((type *)calloc((count), sizeof(type)))
#define g_free free
#define g_str_hash NULL
#define g_str_equal NULL
#define error_report(...) (errors++)
static void g_usleep(unsigned long usec) { assert(usec == 10000); sleeps++; }
static gchar *g_strdup_printf(const char *fmt, ...)
{
    char buffer[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    assert(n >= 0 && (size_t)n < sizeof(buffer));
    char *result = malloc((size_t)n + 1);
    assert(result);
    memcpy(result, buffer, (size_t)n + 1);
    return result;
}
static GHashTable *g_hash_table_new_full(void *hash, void *equal,
                                         void (*keyfree)(void *),
                                         void (*valuefree)(void *))
{
    (void)hash; (void)equal; (void)keyfree; (void)valuefree;
    return calloc(1, sizeof(GHashTable));
}
static void *g_hash_table_lookup(GHashTable *t, const char *key)
{
    for (int i = 0; i < 8; i++) {
        if (t->keys[i] && !strcmp(t->keys[i], key)) {
            return t->values[i];
        }
    }
    return NULL;
}
static void g_hash_table_insert(GHashTable *t, char *key, void *value)
{
    for (int i = 0; i < 8; i++) {
        if (!t->keys[i]) {
            t->keys[i] = key;
            t->values[i] = value;
            return;
        }
    }
    assert(false);
}
static void g_hash_table_remove(GHashTable *t, const char *key)
{
    for (int i = 0; i < 8; i++) {
        if (t->keys[i] && !strcmp(t->keys[i], key)) {
            free(t->keys[i]);
            free(t->values[i]);
            t->keys[i] = NULL;
            t->values[i] = NULL;
            return;
        }
    }
    assert(false);
}
static int disconnect(int conn) {
    assert(conn == 7 || conn == 8); disconnects++; return close_result;
}
static int release_claim(int conn) {
    assert(conn == 7 || conn == 8); releases++;
    if (pending_releases > 0) {
        pending_releases--;
        return kIOReturnStillOpen;
    }
    return release_result;
}
static const struct Client {
    int (*disconnect)(int);
    int (*release_claim)(int);
} client = { disconnect, release_claim };
static const struct Backend { const struct Client *client; } backend = { &client };
static const struct Backend *apple_host_backend(void) { return &backend; }
"""

SUFFIX = r"""
int main(void)
{
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, NULL, 0x123) == 0);
    assert(apple_vfio_dext_publish(1, 2, 0, NULL, 0x123, 7));
    assert(!apple_vfio_dext_publish(1, 2, 0, NULL, 0x123, 8));
    assert(apple_vfio_dext_publish(1, 2, 0, "root", 0x456, 8));
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, NULL, 0x456) == 0);
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, "root", 0x123) == 0);
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, NULL, 0x123) == 7);
    assert(apple_vfio_dext_dma_owner_attached(1, 2, 0, NULL, 7));
    assert(!apple_vfio_dext_dma_owner_attached(1, 2, 0, NULL, 8));
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, NULL, 0x123) == 0);
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, "root", 0x456) == 8);

    apple_vfio_dext_release_dma(1, 2, 0, NULL, 8);
    assert(errors == 1 && disconnects == 0);
    apple_vfio_dext_release(1, 2, 0, NULL, 7);
    assert(disconnects == 0);
    assert(!apple_vfio_dext_dma_owner_attached(1, 2, 0, NULL, 7));
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, NULL, 0x123) == 0);
    apple_vfio_dext_release(1, 2, 0, NULL, 7);
    assert(errors == 2 && disconnects == 0);
    assert(!apple_vfio_dext_publish(1, 2, 0, NULL, 0x123, 7));
    apple_vfio_dext_release_dma(1, 2, 0, NULL, 7);
    assert(disconnects == 1 && releases == 1);
    assert(apple_vfio_dext_publish(1, 2, 0, NULL, 0x999, 7));
    apple_vfio_dext_release_dma(1, 2, 0, "root", 8);
    assert(disconnects == 1);
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, "root", 0x456) == 8);
    apple_vfio_dext_release_dma(1, 2, 0, "root", 8);
    apple_vfio_dext_release(1, 2, 0, "root", 8);
    apple_vfio_dext_release(1, 2, 0, NULL, 7);
    assert(disconnects == 3 && releases == 3);
    assert(apple_vfio_dext_publish(1, 2, 0, NULL, 0x999, 7));
    release_result = 9;
    apple_vfio_dext_release(1, 2, 0, NULL, 7);
    assert(releases == 4 && disconnects == 3 && errors == 3);
    assert(!apple_vfio_dext_publish(1, 2, 0, NULL, 0x999, 7));
    assert(apple_vfio_dext_acquire_dma(1, 2, 0, NULL, 0x999) == 0);
    release_result = 0;
    close_result = 10;
    assert(!apple_vfio_dext_close_connection(8, true));
    assert(releases == 5 && disconnects == 4 && errors == 4);
    close_result = 0;
    assert(apple_vfio_dext_close_connection(8, false));
    assert(releases == 5 && disconnects == 5);
    pending_releases = 2;
    assert(apple_vfio_dext_close_connection(8, true));
    assert(releases == 8 && disconnects == 6 && sleeps == 2);
    pending_releases = 100;
    assert(!apple_vfio_dext_close_connection(8, true));
    assert(releases == 108 && disconnects == 6 && sleeps == 101 && errors == 5);
    free(apple_vfio_shared_dexts);
}
"""


def main():
    source = SOURCE.read_text()
    key_start = source.index("static gchar *apple_vfio_dext_key(")
    key_end = source.index("\nstatic inline AppleVFIOContainer", key_start)
    start = source.index("bool apple_vfio_dext_close_connection(")
    end = source.index("\n/* ------------------------------------------------------------------ */",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "connection.c"
        binary = pathlib.Path(directory) / "connection"
        test.write_text(PREFIX + source[key_start:key_end] +
                        source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
