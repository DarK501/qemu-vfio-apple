#!/usr/bin/env python3
"""Exercise the distribution firmware staging with an out-of-tree build."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "contrib/apple-vfio/scripts/make-dist.sh"
REQUIRED = (
    "kvmvapic.bin",
    "vgabios-stdvga.bin",
    "vgabios-virtio.bin",
    "efi-virtio.rom",
)


def main():
    source = SCRIPT.read_text()
    start = source.index('rm -rf "$DIST_DIR"\n')
    end = source.index("# Recursively collect all non-system dylibs", start)
    staging = source[start:end]

    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        src = root / "source"
        bios = src / "pc-bios"
        build = root / "external-build"
        dist = root / "dist"
        bios.mkdir(parents=True)
        (bios / "keymaps").mkdir()
        (bios / "descriptors").mkdir()
        (bios / "keymaps/en-us").write_text("keyboard")
        (bios / "descriptors/test.json").write_text("{}")
        for name in REQUIRED + ("pxe-virtio.rom", "bios.bin"):
            (bios / name).write_text(name)
        (build / "pc-bios").mkdir(parents=True)
        for name in ("qemu-system-x86_64", "qemu-img"):
            (build / name).write_text(name)
        for name in ("edk2-x86_64-code.fd", "edk2-i386-vars.fd"):
            (build / "pc-bios" / name).write_text(name)

        def run():
            return subprocess.run(
                ["bash", "-eu", "-c",
                 'BUILD_DIR="$1"; DIST_DIR="$2"; '
                 'MAIN_BINARIES=("$BUILD_DIR/qemu-system-x86_64"); '
                 'REQUIRED_BINARIES=qemu-img\n' + staging,
                 "test", str(build), str(dist)],
                cwd=src, text=True, capture_output=True,
            )

        result = run()
        assert result.returncode == 0, result.stderr
        packaged = dist / "share/qemu"
        for name in REQUIRED + ("pxe-virtio.rom", "bios.bin",
                                "edk2-x86_64-code.fd", "edk2-i386-vars.fd",
                                "keymaps/en-us", "descriptors/test.json"):
            assert (packaged / name).is_file(), name

        (bios / "vgabios-stdvga.bin").unlink()
        result = run()
        assert result.returncode != 0
        assert "x86_64 QEMU target requires vgabios-stdvga.bin" in result.stderr


if __name__ == "__main__":
    main()
