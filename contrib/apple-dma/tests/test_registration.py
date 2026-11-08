#!/usr/bin/env python3
"""Exercise DriverKit registration response handling without a macOS SDK."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "hw/vfio/apple-dext-client.c"

PREFIX = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "contrib/apple-dma/apple-dma-protocol.h"

typedef int kern_return_t;
typedef int io_connect_t;
typedef enum AppleDMARegistrationState {
    APPLE_DMA_REGISTRATION_NONE,
    APPLE_DMA_REGISTRATION_OWNED,
    APPLE_DMA_REGISTRATION_UNKNOWN,
} AppleDMARegistrationState;
enum { KERN_SUCCESS = 0, kIOReturnSuccess = 0, IO_OBJECT_NULL = 0,
       kIOReturnBadArgument = 1,
       kIOReturnError = 2, kIOReturnStillOpen = 3,
       kIOReturnNotAligned = 4, kSelectorRegisterDMARegion = 5 };
static AppleDMAMapResult reply;
static size_t reply_size;
static int transport_result, calls;
static kern_return_t IOConnectCallMethod(io_connect_t connection, int selector,
    const uint64_t *input, int input_count, const void *input_struct,
    size_t input_size, uint64_t *output, uint32_t *output_count,
    void *output_struct, size_t *output_size)
{
    assert(connection == 1 && selector == kSelectorRegisterDMARegion);
    assert(input_count == 3 && input[0] == 0 && input[1] == 4096);
    assert(input[2] == 4096);
    assert(!input_struct && !input_size && !output && !output_count);
    calls++;
    memcpy(output_struct, &reply, reply_size);
    *output_size = reply_size;
    return transport_result;
}
"""

SUFFIX = r"""
static void run(int expected_ret, AppleDMARegistrationState expected_state)
{
    AppleDMAMapResult out = {0};
    AppleDMARegistrationState state = APPLE_DMA_REGISTRATION_NONE;
    calls = 0;
    assert(apple_dext_client_register_dma(1, 0, 4096, 4096, &out, &state) ==
           expected_ret);
    assert(state == expected_state && calls == 1);
}
int main(void)
{
    reply_size = sizeof(reply);
    reply.mapping_id = 0;
    reply.status = APPLE_DMA_MAP_STATUS_OK;
    reply.segment_count = 2;
    reply.segments[0] = (AppleDMASegment){ .address = 0x1000,
                                          .length = 2048, .offset = 0 };
    reply.segments[1] = (AppleDMASegment){ .address = 0x9000,
                                          .length = 2048, .offset = 2048 };
    run(KERN_SUCCESS, APPLE_DMA_REGISTRATION_OWNED);

    transport_result = kIOReturnError;
    run(kIOReturnError, APPLE_DMA_REGISTRATION_UNKNOWN);
    transport_result = KERN_SUCCESS;
    reply_size--;
    run(kIOReturnNotAligned, APPLE_DMA_REGISTRATION_UNKNOWN);
    reply_size++;
    reply.mapping_id = 1;
    run(kIOReturnNotAligned, APPLE_DMA_REGISTRATION_UNKNOWN);
    reply.mapping_id = 0;
    reply.segments[1].offset = 1024;
    run(kIOReturnNotAligned, APPLE_DMA_REGISTRATION_UNKNOWN);
    reply.segments[1].offset = 2048;
    reply.segments[1].address = UINT64_MAX;
    run(kIOReturnNotAligned, APPLE_DMA_REGISTRATION_UNKNOWN);
    reply.segments[1].address = 0x9000;
    reply.segment_count = APPLE_DMA_MAX_SEGMENTS + 1;
    run(kIOReturnNotAligned, APPLE_DMA_REGISTRATION_UNKNOWN);
    reply.segment_count = 0;
    reply.status = APPLE_DMA_MAP_STATUS_ERROR;
    run(kIOReturnError, APPLE_DMA_REGISTRATION_NONE);
    reply.status = APPLE_DMA_MAP_STATUS_RETAINED;
    run(kIOReturnStillOpen, APPLE_DMA_REGISTRATION_OWNED);
    reply.segment_count = 1;
    run(kIOReturnNotAligned, APPLE_DMA_REGISTRATION_UNKNOWN);
    return 0;
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("kern_return_t\napple_dext_client_register_dma(")
    end = source.index("\nkern_return_t\napple_dext_client_allocate_dma_buffer",
                       start)
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "registration.c"
        binary = pathlib.Path(directory) / "registration"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(test), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
