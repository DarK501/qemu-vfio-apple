#!/usr/bin/env python3
"""Exercise the dext registration selector and output rollback decisions."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include "contrib/apple-dma/apple-dma-protocol.h"
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1,
       kIOReturnNoMemory = 2, kIOReturnStillOpen = 3,
       kIOReturnExclusiveAccess = 4,
       kVFIOUserPCIDriverUserClientMethodRegisterDMARegion = 5 };
static int registration_ret, unregister_ret, unregister_calls;
static bool append_succeeds = true;
class OSData {
public:
    AppleDMAMapResult result = {};
    static OSData *withCapacity(size_t size) {
        assert(size == sizeof(result));
        return new OSData;
    }
    void release() { delete this; }
    bool appendBytes(const void *p, size_t size) {
        assert(size == sizeof(result));
        if (!append_succeeds) {
            return false;
        }
        std::memcpy(&result, p, size);
        return true;
    }
};
struct Arguments {
    uint64_t *scalarInput;
    int scalarInputCount;
    void *structureOutputDescriptor;
    size_t structureOutputMaximumSize;
    OSData *structureOutput;
};
class Client;
static void vfio_user_dma_log(const char *, ...) {}
static int vfio_user_register_client_dma_region(Client *, uint64_t id,
    uint64_t, uint64_t, AppleDMAMapResult *result)
{
    result->mapping_id = id;
    result->status = APPLE_DMA_MAP_STATUS_OK;
    if (registration_ret == kIOReturnStillOpen) {
        result->status = APPLE_DMA_MAP_STATUS_RETAINED;
        return kIOReturnSuccess;
    }
    return registration_ret;
}
static int vfio_user_unregister_client_dma_region(Client *, uint64_t id)
{
    assert(id == 42);
    unregister_calls++;
    return unregister_ret;
}
class Client {
public:
    int invoke(Arguments *arguments) {
        kern_return_t ret;
        switch (kVFIOUserPCIDriverUserClientMethodRegisterDMARegion) {
"""

SUFFIX = r"""
        default: return kIOReturnBadArgument;
        }
    }
};
static void run(int reg, bool append, int unreg, int expected_ret,
                int expected_calls, int expected_status)
{
    uint64_t input[] = {42, 0x1000, 4096};
    Arguments args = {input, 3, nullptr, sizeof(AppleDMAMapResult), nullptr};
    Client client;
    registration_ret = reg;
    append_succeeds = append;
    unregister_ret = unreg;
    unregister_calls = 0;
    assert(client.invoke(&args) == expected_ret);
    assert(unregister_calls == expected_calls);
    if (expected_ret == kIOReturnSuccess) {
        assert(args.structureOutput && args.structureOutput->result.mapping_id == 42);
        assert(args.structureOutput->result.status == (unsigned)expected_status);
        args.structureOutput->release();
    } else {
        assert(!args.structureOutput);
    }
}
int main()
{
    run(kIOReturnSuccess, true, 0, kIOReturnSuccess, 0, APPLE_DMA_MAP_STATUS_OK);
    run(kIOReturnStillOpen, true, 0, kIOReturnSuccess, 0, APPLE_DMA_MAP_STATUS_RETAINED);
    run(kIOReturnExclusiveAccess, true, 0, kIOReturnSuccess, 0, APPLE_DMA_MAP_STATUS_ERROR);
    run(kIOReturnExclusiveAccess, false, 0, kIOReturnNoMemory, 0, 0);
    run(kIOReturnSuccess, false, 0, kIOReturnNoMemory, 1, 0);
    run(kIOReturnStillOpen, false, kIOReturnBadArgument, kIOReturnStillOpen, 1, 0);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("    case kVFIOUserPCIDriverUserClientMethodRegisterDMARegion:")
    end = source.index("\n    case kVFIOUserPCIDriverUserClientMethodUnregisterDMARegion:",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "dext_registration.cpp"
        binary = pathlib.Path(directory) / "dext_registration"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(test), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
