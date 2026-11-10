#!/usr/bin/env python3
"""Exercise DriverKit bounce-buffer completion and failure retention."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnError = 2,
       kIODMACommandCompleteDMANoOptions = 0 };
static void vfio_user_dma_log(const char *, ...) {}
struct Object {
    int releases = 0;
    void release() { releases++; }
};
struct Command : Object {
    int result = kIOReturnSuccess;
    int calls = 0;
    int CompleteDMA(int options) {
        assert(options == kIODMACommandCompleteDMANoOptions);
        calls++;
        return result;
    }
};
struct Ivars {
    Command *dmaCommand = nullptr;
    Object *dmaBuffer = nullptr;
    uint64_t dmaBufferSize = 0, dmaFlags = 0;
    uint32_t dmaSegmentsCount = 0;
    uint64_t dmaSegments[2] = {};
};
struct VFIOUserPCIDriverUserClient { Ivars *ivars; };
#define OSSafeReleaseNULL(ptr) do { \
    if (ptr) { (ptr)->release(); (ptr) = nullptr; } \
} while (0)
"""

SUFFIX = r"""
int main()
{
    Command command;
    Object buffer;
    Ivars vars;
    vars.dmaCommand = &command;
    vars.dmaBuffer = &buffer;
    vars.dmaBufferSize = 4096;
    vars.dmaFlags = 1;
    vars.dmaSegmentsCount = 1;
    vars.dmaSegments[0] = 123;
    VFIOUserPCIDriverUserClient client { &vars };
    assert(vfio_user_release_dma_buffer(nullptr) == kIOReturnBadArgument);
    command.result = kIOReturnError;
    assert(vfio_user_release_dma_buffer(&client) == kIOReturnError);
    assert(vars.dmaCommand == &command && vars.dmaBuffer == &buffer);
    assert(vars.dmaBufferSize == 4096 && vars.dmaSegments[0] == 123);
    assert(command.releases == 0 && buffer.releases == 0);
    command.result = kIOReturnSuccess;
    assert(vfio_user_release_dma_buffer(&client) == kIOReturnSuccess);
    assert(!vars.dmaCommand && !vars.dmaBuffer && !vars.dmaBufferSize);
    assert(!vars.dmaFlags && !vars.dmaSegmentsCount && !vars.dmaSegments[0]);
    assert(command.calls == 2 && command.releases == 1 && buffer.releases == 1);
    assert(vfio_user_release_dma_buffer(&client) == kIOReturnSuccess);
    assert(command.calls == 2 && command.releases == 1 && buffer.releases == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static kern_return_t\nvfio_user_release_dma_buffer(")
    end = source.index("\nstatic kern_return_t\nvfio_user_validate_dma_buffer_segments(",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "dma_buffer.cpp"
        binary = pathlib.Path(directory) / "dma_buffer"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
