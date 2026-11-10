#!/usr/bin/env python3
"""Verify optional ID negotiation preserves legacy GPA-token requests."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dma.c"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "contrib/apple-dma/apple-dma-protocol.h"
"""
SUFFIX = r"""
int main(void)
{
    uint64_t id = 0;
    assert(apple_dma_request_mapping_id(0x1234, 0, &id) && id == 0x1234);
    assert(!apple_dma_request_mapping_id(0x1234, 1, &id));
    assert(!apple_dma_request_mapping_id(0x1234, APPLE_DMA_MAP_FLAG_MAPPING_ID, &id));
    assert(apple_dma_request_mapping_id(0x1234, APPLE_DMA_MAP_FLAG_MAPPING_ID | 1, &id));
    assert(id == (APPLE_DMA_MAP_ID_PREFIX | 1));
    assert(apple_dma_request_mapping_id(0x1234, UINT32_MAX, &id));
    assert(id == (APPLE_DMA_MAP_ID_PREFIX | APPLE_DMA_MAP_ID_MASK));
}
"""

def main():
    source = SOURCE.read_text()
    start = source.index("static bool apple_dma_request_mapping_id(")
    end = source.index("static void apple_dma_handle_map(", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
