#!/usr/bin/env python3
"""Verify Apple VFIO performs at most one function-reset attempt."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
PCI = ROOT / "hw/vfio/pci.c"
APPLE_CONTAINER = ROOT / "hw/vfio/container-apple.c"
APPLE_DEVICE = ROOT / "hw/vfio/apple-device.c"

PREFIX = r"""
#include <cassert>
#include <cerrno>
struct VFIODevice {
    const char *name;
    bool reset_works;
    struct VFIOIOOps *io_ops;
};
struct VFIOIOOps {
    int (*device_reset)(VFIODevice *);
};
struct VFIOPCIDevice {
    VFIODevice vbasedev;
    bool has_flr;
    bool has_pm_reset;
};
static int reset_calls, reset_result, warnings, infos;
static int device_reset(VFIODevice *) { reset_calls++; return reset_result; }
static VFIOIOOps io_ops = { device_reset };
static void warn_report(const char *, ...) { warnings++; }
static void info_report(const char *, ...) { infos++; }
"""

SUFFIX = r"""
int main()
{
    VFIOPCIDevice dev = { { "apple-gpu", true, &io_ops }, true, true };
    assert(vfio_pci_reset_once(&dev) == 0);
    assert(reset_calls == 1 && infos == 1 && warnings == 0);

    reset_calls = infos = warnings = 0;
    reset_result = -EIO;
    assert(vfio_pci_reset_once(&dev) == -EIO);
    assert(reset_calls == 1 && infos == 0 && warnings == 1);

    reset_calls = infos = warnings = 0;
    dev.vbasedev.reset_works = false;
    assert(vfio_pci_reset_once(&dev) == -ENOTSUP);
    assert(reset_calls == 0 && infos == 0 && warnings == 1);

    reset_calls = infos = warnings = 0;
    dev.vbasedev.reset_works = true;
    dev.has_flr = false;
    assert(vfio_pci_reset_once(&dev) == -ENOTSUP);
    assert(reset_calls == 0 && infos == 0 && warnings == 1);

    reset_calls = infos = warnings = 0;
    dev.has_pm_reset = false;
    reset_result = 0;
    assert(vfio_pci_reset_once(&dev) == 0);
    assert(reset_calls == 1 && infos == 1 && warnings == 0);

    reset_calls = infos = warnings = 0;
    dev.has_flr = true;
    assert(vfio_pci_reset_once(&dev) == 0);
    assert(reset_calls == 1 && infos == 1 && warnings == 0);

    reset_calls = infos = warnings = 0;
    dev.vbasedev.io_ops = nullptr;
    assert(vfio_pci_reset_once(&dev) == -ENOTSUP);
    assert(reset_calls == 0 && infos == 0 && warnings == 1);

    reset_calls = infos = warnings = 0;
    VFIOIOOps missing_reset = { nullptr };
    dev.vbasedev.io_ops = &missing_reset;
    assert(vfio_pci_reset_once(&dev) == -ENOTSUP);
    assert(reset_calls == 0 && infos == 0 && warnings == 1);

    VFIODevice host = { "apple-gpu", true, &io_ops };
    assert(apple_vfio_pci_hot_reset(&host, true) == -ENOTSUP);
    assert(warnings == 2);
}
"""


def main():
    pci = PCI.read_text()
    start = pci.index("static int vfio_pci_reset_once(")
    end = pci.index("\nstatic void vfio_pci_reset(", start)
    reset = pci.index("if (vdev->reset_once)", end)
    generic = pci.index("if (vdev->resetfn", reset)
    post_reset = pci.index("post_reset:", generic)
    assert reset < generic < post_reset
    assert "vfio_pci_reset_once(vdev);" in pci[reset:generic]
    assert "goto post_reset;" in pci[reset:generic]
    apple = APPLE_DEVICE.read_text()
    init_start = apple.index("static void apple_vfio_pci_init(")
    init_end = apple.index("\n}", init_start)
    assert "vdev->reset_once = true;" in apple[init_start:init_end]

    container = APPLE_CONTAINER.read_text()
    hot_start = container.index("static int apple_vfio_pci_hot_reset(")
    hot_end = container.index("\n}", hot_start) + 2
    with tempfile.TemporaryDirectory() as directory:
        source = pathlib.Path(directory) / "reset_policy.cpp"
        binary = pathlib.Path(directory) / "reset_policy"
        source.write_text(PREFIX + pci[start:end] +
                          container[hot_start:hot_end] + SUFFIX)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
