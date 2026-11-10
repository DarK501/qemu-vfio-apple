#!/usr/bin/env python3
"""Verify overlapping DMA addresses have an unambiguous original backing."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t dma_addr_t;
struct list_head { struct list_head *next, *prev; };
struct dma_window { dma_addr_t dma_addr; size_t size; };
struct apple_dma_map_entry {
    struct list_head node;
    dma_addr_t mapped_dma, orig_dma;
    size_t size;
    struct dma_window *window;
};
struct apple_dma_dev {
    int maps_lock, map_count;
    int64_t map_bytes;
    struct list_head maps;
};
#define max(a, b) ((a) > (b) ? (a) : (b))
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define entry(ptr) ((struct apple_dma_map_entry *)((char *)(ptr) - \
                   offsetof(struct apple_dma_map_entry, node)))
#define list_for_each_entry(e, head, member) \
    for (struct list_head *p = (head)->next; \
         p != (head) && (((e) = entry(p)), 1); p = p->next)
static void list_add_tail(struct list_head *node, struct list_head *head)
{
    node->prev = head->prev; node->next = head;
    head->prev->next = node; head->prev = node;
}
static void atomic_inc(int *n) { ++*n; }
static void atomic64_add(size_t n, int64_t *a) { *a += n; }
"""

SUFFIX = r"""
int main(void)
{
    struct apple_dma_dev ad = {0};
    struct apple_dma_map_entry a = {0}, b = {0}, c = {0}, d = {0};
    struct dma_window window = { .dma_addr = 0x8000, .size = 8192 };
    struct dma_window other_window = { .dma_addr = 0x9000, .size = 8192 };
    ad.maps.next = ad.maps.prev = &ad.maps;
    assert(apple_dma_publish_entry(&ad, &a, 0x8000, 0x1000, 4096, &window));
    assert(apple_dma_publish_entry(&ad, &b, 0x8800, 0x1800, 4096, &window));
    assert(!apple_dma_publish_entry(&ad, &c, 0x8800, 0x2800, 4096, &window));
    assert(apple_dma_window_conflicts(&ad, &other_window));
    assert(!apple_dma_window_conflicts(&ad, &window));
    other_window.dma_addr = 0xa000;
    assert(!apple_dma_window_conflicts(&ad, &other_window));
    assert(ad.map_count == 2 && ad.map_bytes == 8192);
    assert(apple_dma_publish_entry(&ad, &c, 0xa000, 0x3000, 4096, &window));
    assert(!apple_dma_publish_entry(&ad, &d, 0x7000, 0x3000, 8192, &window));
    assert(ad.map_count == 3 && ad.map_bytes == 12288);

    struct apple_dma_dev overflow = {0};
    struct apple_dma_map_entry e = {0}, f = {0};
    overflow.maps.next = overflow.maps.prev = &overflow.maps;
    assert(apple_dma_publish_entry(&overflow, &e, 0x2000, UINT64_MAX - 1, 8, &window));
    assert(!apple_dma_publish_entry(&overflow, &f, 0x2002, 0x1000, 1, &window));
    assert(overflow.map_count == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static bool apple_dma_window_conflicts(")
    end = source.index("\nstatic struct apple_dma_map_entry *\napple_dma_take", start)
    for location in ("static dma_addr_t apple_dma_unmap_one(",
                     "if (err_idx >= 0) {",
                     "static void apple_dma_unmap_sg("):
        begin = source.index(location)
        take = source.index("e = apple_dma_take(", begin)
        release = source.index("apple_dma_release_entry(ad, dev, e);", take)
        segment = source[begin:release]
        lock = segment.rfind("mutex_lock(&ad->batch_lock)")
        assert 0 <= lock < segment.index("e = apple_dma_take(")
        assert "mutex_unlock(&ad->batch_lock)" not in segment[lock:]
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
