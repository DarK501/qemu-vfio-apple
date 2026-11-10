#!/usr/bin/env python3
"""Verify that prepared DriverKit DMA mappings are bidirectional."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
enum { kIOMemoryDirectionIn = 1, kIOMemoryDirectionOut = 2,
       kIOMemoryDirectionOutIn = 3 };
"""
SUFFIX = r"""
int main()
{
    assert(!vfio_user_dma_is_bidirectional(0));
    assert(!vfio_user_dma_is_bidirectional(kIOMemoryDirectionIn));
    assert(!vfio_user_dma_is_bidirectional(kIOMemoryDirectionOut));
    assert(vfio_user_dma_is_bidirectional(kIOMemoryDirectionOutIn));
    assert(vfio_user_dma_is_bidirectional(kIOMemoryDirectionOutIn | 16));
}
"""

def main():
    source = SOURCE.read_text()
    start = source.index("static bool\nvfio_user_dma_is_bidirectional(")
    end = source.index("\nenum {", start)
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.cpp"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
