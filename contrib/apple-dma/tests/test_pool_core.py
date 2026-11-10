#!/usr/bin/env python3
"""Exercise the guest pool page bookkeeping in userspace."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
HEADER_DIR = ROOT / "contrib/apple-dma/pool"

PROGRAM = r"""
#include <assert.h>
#include <stdlib.h>
#include "apple_dma_pool_core.h"

#define NPAGES 64

int main(void)
{
    struct pool_core p = { calloc(NPAGES, sizeof(struct pool_slot)), NPAGES };
    long a, b, c;

    assert(pool_pages_for(0, 1) == 1);
    assert(pool_pages_for(0xfff, 2) == 2);
    assert(pool_pages_for(0x800, 0x1000) == 2);

    assert(pool_find(&p, 0, 1) < 0);
    assert(pool_find(&p, 1, 3) < 0);
    assert(pool_find(&p, NPAGES + 1, 1) < 0);

    /* Unaligned stream mapping first, so the aligned one must skip ahead. */
    a = pool_find(&p, 3, 1);
    assert(a == 0);
    pool_claim(&p, a, 3, POOL_STREAM);
    b = pool_find(&p, 8, 8);
    assert(b == 8 && b % 8 == 0);
    pool_claim(&p, b, 8, POOL_COHERENT);

    /* Release validates kind and the range start. */
    assert(pool_release(&p, b, POOL_STREAM) == 0);
    assert(pool_release(&p, b + 1, POOL_COHERENT) == 0);
    assert(pool_release(&p, NPAGES, POOL_COHERENT) == 0);
    assert(pool_release(&p, a, POOL_STREAM) == 3);
    assert(pool_release(&p, a, POOL_STREAM) == 0);

    /* Freed pages are reused; no free range is lost. */
    c = pool_find(&p, 8, 1);
    assert(c == 0 || c == 16);
    pool_claim(&p, c, 8, POOL_STREAM);
    assert(pool_release(&p, c, POOL_STREAM) == 8);
    assert(pool_release(&p, b, POOL_COHERENT) == 8);

    /* Exhaustion and complete recovery. */
    assert(pool_find(&p, NPAGES, 1) == 0);
    pool_claim(&p, 0, NPAGES, POOL_COHERENT);
    assert(pool_find(&p, 1, 1) < 0);
    assert(pool_release(&p, 0, POOL_COHERENT) == NPAGES);
    assert(pool_find(&p, NPAGES, NPAGES) == 0);
    free(p.slots);
    return 0;
}
"""


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        source = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        source.write_text(PROGRAM)
        subprocess.run(
            ["gcc", "-Wall", "-Wextra", "-Werror", f"-I{HEADER_DIR}",
             str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("pool core bookkeeping test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
