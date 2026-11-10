#!/usr/bin/env python3
"""Exercise the guest's segmented DMA response and rollback handling."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-dma/linux/apple_dma_main.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include "contrib/apple-dma/apple-dma-protocol.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
typedef uint64_t phys_addr_t;
#define cpu_to_le64(x) (x)
#define cpu_to_le32(x) (x)
#define le64_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
enum { CMD_MAP = 1, CMD_UNMAP = 2, S_OK = 0, S_IOERR = 1, S_INVAL = 3 };
struct device { int unused; };
struct pci_dev { struct device dev; };
struct apple_dma_map_req { u64 gpa; u32 len, flags; };
struct apple_dma_unmap_req { u64 iova, size; };
struct apple_dma_unmap_resp { u64 iova; u32 status, reserved; };
struct apple_dma_dev {
    struct pci_dev *pdev;
    u32 features, next_mapping_id;
    struct apple_dma_map_req map_req;
    AppleDMAMapResult map_resp;
    struct apple_dma_unmap_req unmap_req;
    struct apple_dma_unmap_resp unmap_resp;
};
static AppleDMAMapResult reply;
static u32 map_status, unmap_status, unmap_reply_status;
static u64 unmap_reply_id;
static u32 command;
static int unmap_calls;
static u64 conflict_address;
static jmp_buf panic_env;
#define dev_err(...) ((void)0)
struct dma_window;
static bool apple_dma_range_conflicts(struct apple_dma_dev *ad, u64 address,
                                      u64 size, struct dma_window *except)
{
    (void)ad; (void)size; (void)except;
    return conflict_address && address == conflict_address;
}
static void apple_dma_panic_state(struct device *dev, const char *reason)
{
    (void)dev; (void)reason;
    longjmp(panic_env, 1);
}
static void apple_dma_prepare_cmd(struct apple_dma_dev *ad, u32 type,
                                  void *req, void *resp)
{
    (void)ad; (void)req; (void)resp; command = type;
}
static u32 apple_dma_ring(struct apple_dma_dev *ad)
{
    if (command == CMD_MAP) {
        ad->map_resp = reply;
        return map_status;
    }
    assert(command == CMD_UNMAP);
    unmap_calls++;
    ad->unmap_resp.status = unmap_reply_status;
    ad->unmap_resp.iova = unmap_reply_id;
    return unmap_status;
}
"""

SUFFIX = r"""
static void prepare(void)
{
    memset(&reply, 0, sizeof(reply));
    map_status = unmap_status = unmap_reply_status = S_OK;
    unmap_reply_id = 0x1000;
    unmap_calls = 0;
    conflict_address = 0;
    reply.status = APPLE_DMA_MAP_STATUS_OK;
    reply.mapping_id = 0x1000;
    reply.segment_count = 2;
    reply.segments[0] = (AppleDMASegment){0x9000, 2048, 0};
    reply.segments[1] = (AppleDMASegment){0x9800, 2048, 2048};
}
static int map(struct apple_dma_dev *ad, u64 *token, dma_addr_t *dma, u32 *len)
{
    return apple_dma_device_map(ad, 0x1000, 4096, token, dma, len);
}
int main(void)
{
    struct pci_dev pdev = {0};
    struct apple_dma_dev ad = { .pdev = &pdev };
    u64 token = 0;
    dma_addr_t dma = 0;
    u32 len = 0;

    prepare();
    assert(map(&ad, &token, &dma, &len) == 0);
    assert(token == 0x1000 && dma == 0x9000 && len == 4096 && !unmap_calls);
    assert(apple_dma_device_unmap(&ad, token, len) == 0);
    assert(unmap_calls == 1 && ad.unmap_req.iova == token &&
           ad.unmap_req.size == len);

    prepare();
    reply.segments[1].address = 0xb000;
    assert(map(&ad, &token, &dma, &len) == -ERANGE && unmap_calls == 1);
    assert(ad.unmap_req.iova == 0x1000 && ad.unmap_req.size == 4096);

    prepare();
    reply.segments[1].address = 0xb000;
    conflict_address = 0xb000;
    if (!setjmp(panic_env)) {
        (void)map(&ad, &token, &dma, &len);
        assert(!"overlapping fragmented response must not be rolled back");
    }
    assert(unmap_calls == 0);

    prepare();
    map_status = S_IOERR;
    conflict_address = 0x9000;
    if (!setjmp(panic_env)) {
        (void)map(&ad, &token, &dma, &len);
        assert(!"failed batch must not roll back an overlapping mapping");
    }
    assert(unmap_calls == 0);

    prepare();
    reply.segment_count = APPLE_DMA_MAX_SEGMENTS + 1;
    conflict_address = 0x9800;
    if (!setjmp(panic_env)) {
        (void)map(&ad, &token, &dma, &len);
        assert(!"malformed count must not hide a reported overlap");
    }
    assert(unmap_calls == 0);

    prepare();
    reply.segments[1].offset = 2049;
    assert(map(&ad, &token, &dma, &len) == -EIO && unmap_calls == 1);
    prepare();
    reply.segments[1].length = 2047;
    assert(map(&ad, &token, &dma, &len) == -EIO && unmap_calls == 1);
    prepare();
    reply.segment_count = APPLE_DMA_MAX_SEGMENTS + 1;
    assert(map(&ad, &token, &dma, &len) == -EIO && unmap_calls == 1);
    prepare();
    reply.status = APPLE_DMA_MAP_STATUS_ERROR;
    map_status = S_IOERR;
    assert(map(&ad, &token, &dma, &len) == -EIO && !unmap_calls);
    prepare();
    reply.status = APPLE_DMA_MAP_STATUS_RETAINED;
    map_status = S_IOERR;
    assert(map(&ad, &token, &dma, &len) == -EIO && unmap_calls == 1);

    prepare();
    reply.segments[1].address = 0xb000;
    unmap_status = S_IOERR;
    if (!setjmp(panic_env)) {
        (void)map(&ad, &token, &dma, &len);
        assert(!"failed rollback must be fatal");
    }
    assert(unmap_calls == 1);

    prepare();
    unmap_reply_id = 0x1001;
    assert(apple_dma_device_unmap(&ad, 0x1000, 4096) == -EIO);

    ad.features = APPLE_DMA_FEATURE_MAPPING_ID;
    prepare();
    reply.mapping_id = APPLE_DMA_MAP_ID_PREFIX | 1;
    assert(map(&ad, &token, &dma, &len) == 0);
    assert(token == reply.mapping_id &&
           ad.map_req.flags == (APPLE_DMA_MAP_FLAG_MAPPING_ID | 1));
    prepare();
    reply.mapping_id = APPLE_DMA_MAP_ID_PREFIX | 2;
    unmap_reply_id = reply.mapping_id;
    reply.segments[1].address = 0xb000;
    assert(map(&ad, &token, &dma, &len) == -ERANGE);
    assert(unmap_calls == 1 && ad.unmap_req.iova == reply.mapping_id);
    prepare();
    reply.mapping_id = APPLE_DMA_MAP_ID_PREFIX | 3;
    reply.status = APPLE_DMA_MAP_STATUS_RETAINED;
    unmap_reply_id = reply.mapping_id;
    assert(map(&ad, &token, &dma, &len) == -EIO && unmap_calls == 1);
    prepare();
    reply.mapping_id = 0x1000;
    if (!setjmp(panic_env)) {
        (void)map(&ad, &token, &dma, &len);
        assert(!"wrong identity must not unmap another mapping");
    }
    assert(unmap_calls == 0);
    ad.next_mapping_id = APPLE_DMA_MAP_ID_MASK;
    prepare();
    assert(map(&ad, &token, &dma, &len) == -ENOSPC && !unmap_calls);
    return 0;
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static int apple_dma_device_unmap(")
    end = source.index("\n/* ------------------------------------------------------------------ */",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "guest_protocol.c"
        binary = pathlib.Path(directory) / "guest_protocol"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(test), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
