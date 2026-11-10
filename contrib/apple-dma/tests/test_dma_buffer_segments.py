#!/usr/bin/env python3
"""Verify DriverKit DMA buffer segment coverage validation."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"
PREFIX = r"""
#include <cassert>
#include <cstdint>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1 };
struct IOAddressSegment { uint64_t address, length; };
"""
SUFFIX = r"""
int main()
{
    IOAddressSegment s[2] = {{0x1000, 4096}, {0x4000, 4096}};
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 8192) == 0);
    assert(vfio_user_validate_dma_buffer_segments(nullptr, 2, 2, 8192));
    assert(vfio_user_validate_dma_buffer_segments(s, 0, 2, 8192));
    assert(vfio_user_validate_dma_buffer_segments(s, 3, 2, 8192));
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 0));
    assert(vfio_user_validate_dma_buffer_segments(s, 1, 2, 8192));
    s[1].length = 0;
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 8192));
    s[1].length = 4097;
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 8192));
    s[1].length = 4096;
    s[1].address = UINT64_MAX;
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 8192));
    s[1].address = 0x1fff;
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 8192));
    s[1].address = 0x2000;
    assert(vfio_user_validate_dma_buffer_segments(s, 2, 2, 8192) == 0);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static kern_return_t\nvfio_user_validate_dma_buffer_segments(")
    end = source.index("\nstatic kern_return_t\nvfio_user_allocate_dma_buffer(", start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "segments.cpp"
        binary = pathlib.Path(directory) / "segments"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
