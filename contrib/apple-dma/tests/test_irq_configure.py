#!/usr/bin/env python3
"""Verify interrupt configuration failure does not register IRQ sources."""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp"

PREFIX = r"""
#include <cassert>
#include <cstdint>
using kern_return_t = int;
enum { kIOReturnSuccess = 0, kIOInterruptTypePCIMessaged = 1,
       kIOInterruptTypePCIMessagedX = 2 };
#define OS_LOG_DEFAULT 0
#define os_log(...) ((void)0)
struct IOPCIDevice {
    int calls = 0, first = 0, second = 0;
    int types[2] = {};
    uint32_t counts[2] = {};
    int ConfigureInterrupts(int type, int minimum, uint32_t count, int flags) {
        assert(minimum == 1 && flags == 0 && calls < 2);
        types[calls] = type; counts[calls] = count;
        return calls++ == 0 ? first : second;
    }
};
"""

SUFFIX = r"""
int main()
{
    bool msix = true;
    uint32_t count = 8;
    IOPCIDevice pci;
    assert(!vfio_user_configure_interrupts(&pci, &msix, &count));
    assert(pci.calls == 1 && pci.types[0] == kIOInterruptTypePCIMessagedX);
    assert(msix && count == 8);

    pci = {}; pci.first = 7; msix = true; count = 8;
    assert(!vfio_user_configure_interrupts(&pci, &msix, &count));
    assert(pci.calls == 2 && pci.types[1] == kIOInterruptTypePCIMessaged);
    assert(pci.counts[0] == 8 && pci.counts[1] == 1 && !msix && count == 1);

    pci = {}; pci.first = 7; pci.second = 9; msix = true; count = 8;
    assert(vfio_user_configure_interrupts(&pci, &msix, &count) == 9);
    assert(pci.calls == 2 && !msix && count == 1);

    pci = {}; pci.first = 7; msix = false; count = 4;
    assert(vfio_user_configure_interrupts(&pci, &msix, &count) == 7);
    assert(pci.calls == 1 && count == 4);
}
"""


def main():
    source = SOURCE.read_text()
    start = source.index("static kern_return_t\nvfio_user_configure_interrupts(")
    end = source.index("static kern_return_t\nvfio_user_setup_interrupts(", start)
    setup_end = source.index("\nkern_return_t\nVFIOUserPCIDriverUserClient::ExternalMethod",
                             end)
    setup = source[end:setup_end]
    fail = setup.index("ret = vfio_user_configure_interrupts(")
    loop = setup.index("for (uint32_t i = 0; i < requested; i++)", fail)
    assert "vfio_user_release_all_interrupts(client)" in setup[fail:loop]
    assert "return ret;" in setup[fail:loop]
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "test.cpp"
        binary = pathlib.Path(tmp) / "test"
        test.write_text(PREFIX + source[start:end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
