#!/usr/bin/env python3
"""Verify interrupt teardown retains DriverKit state until async cancel drains."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
#include <functional>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1,
       kIOReturnStillOpen = 2 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
#define VFIO_USER_IRQ_PENDING_WORDS 4
struct Object {
    int releases = 0;
    void release() { releases++; }
};
using OSAction = Object;
struct IOInterruptDispatchSource : Object {
    std::function<void()> completion;
    void Cancel(std::function<void()> callback) {
        assert(!completion);
        completion = callback;
    }
};
struct ClientIvars {
    bool interruptsSetUp = true;
    uint32_t numInterrupts = 1;
    uint32_t pendingInterruptCancelCount = 0;
    IOInterruptDispatchSource *interruptSources[2] = {};
    Object *irqQueue = nullptr;
    uint64_t pending[4] = {};
    uint64_t enabled[4] = {};
    volatile uint64_t *irqPending = pending;
    volatile uint64_t *irqEnabled = enabled;
    Object *pendingInterruptNotify = nullptr;
    Object *irqSharedBuffer = nullptr;
};
struct VFIOUserPCIDriverUserClient {
    ClientIvars *ivars;
    int references = 1;
    void retain() { references++; }
    void release() { references--; }
};
#define OSSafeReleaseNULL(ptr) do { \
    if (ptr) { (ptr)->release(); (ptr) = nullptr; } \
} while (0)
"""

SUFFIX = r"""
int main()
{
    ClientIvars ivars;
    VFIOUserPCIDriverUserClient client = { &ivars };
    IOInterruptDispatchSource source;
    Object queue, shared;
    ivars.interruptSources[0] = &source;
    ivars.irqQueue = &queue;
    ivars.irqSharedBuffer = &shared;

    assert(vfio_user_release_all_interrupts(&client) == kIOReturnStillOpen);
    assert(!ivars.interruptsSetUp && ivars.pendingInterruptCancelCount == 1);
    assert(ivars.interruptSources[0] == nullptr && ivars.irqQueue == &queue);
    assert(ivars.irqSharedBuffer == &shared && client.references == 2);
    assert(vfio_user_release_all_interrupts(&client) == kIOReturnStillOpen);

    source.completion();
    assert(ivars.pendingInterruptCancelCount == 0);
    assert(source.releases == 1 && client.references == 1);
    assert(vfio_user_release_all_interrupts(&client) == kIOReturnSuccess);
    assert(ivars.numInterrupts == 0 && ivars.irqQueue == nullptr);
    assert(ivars.irqSharedBuffer == nullptr);
    assert(queue.releases == 1 && shared.releases == 1);

    ClientIvars partial;
    VFIOUserPCIDriverUserClient partialClient = { &partial };
    Object partialQueue, partialShared;
    partial.interruptsSetUp = false;
    partial.numInterrupts = 0;
    partial.irqQueue = &partialQueue;
    partial.irqSharedBuffer = &partialShared;
    assert(vfio_user_release_all_interrupts(&partialClient) == kIOReturnSuccess);
    assert(partialQueue.releases == 1 && partialShared.releases == 1);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index(
        "static kern_return_t\nvfio_user_release_all_interrupts("
        "VFIOUserPCIDriverUserClient *client)\n{")
    end = source.index(
        "\nstatic uint32_t\nvfio_user_get_requested_interrupt_vectors", start)
    body = source[start:end].replace(
        "src->Cancel(^{", "src->Cancel([client, src] {")
    with tempfile.TemporaryDirectory() as directory:
        test = pathlib.Path(directory) / "interrupt_stop.cpp"
        binary = pathlib.Path(directory) / "interrupt_stop"
        test.write_text(PREFIX + body + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
