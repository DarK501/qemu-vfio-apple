#!/usr/bin/env python3
"""Check DKMS targets the kernel selected by the guest boot symlink."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/image-builder/guest/provision-desktop.sh"
BUILDER = ROOT / "contrib/apple-vfio/image-builder/build-ubuntu-desktop-image.sh"


def main():
    source = SOURCE.read_text()
    start = source.index("apple_dma_target_kernel() {")
    end = source.index("\n}\n", start) + 2
    function = source[start:end]

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        link = root / "vmlinuz"
        link.symlink_to("vmlinuz-7.0.0-43-generic")
        command = function + '\n[ "$(apple_dma_target_kernel "$1")" = "7.0.0-43-generic" ]\n'
        subprocess.run(["bash", "-c", command, "test", str(link)], check=True)

        invalid = root / "not-a-kernel"
        invalid.symlink_to("bzImage-7.0.0-43-generic")
        result = subprocess.run(["bash", "-c", command, "test", str(invalid)])
        assert result.returncode != 0

        builder_source = BUILDER.read_text()
        python_start = builder_source.index(
            "    python3 - \"$MANIFEST_PATH\" <<'PY'\n") + len(
                "    python3 - \"$MANIFEST_PATH\" <<'PY'\n")
        python_end = builder_source.index("\nPY\n}", python_start)
        manifest_program = builder_source[python_start:python_end]

        build_log = root / "build.log"
        manifest_path = root / "manifest.json"
        env = os.environ.copy()
        env.update({
            "IMAGE_NAME": "guest-image",
            "GUEST_ARCH": "x86_64",
            "FINAL_IMAGE": str(root / "guest-image.qcow2"),
            "BASE_IMAGE": "base.img",
            "BASE_IMAGE_URL": "https://example.invalid/base.img",
            "DISK_SIZE": "96G",
            "CPUS": "8",
            "MEMORY": "12G",
            "BUILD_USER": "ubuntu",
            "INSTALL_FEX": "0",
            "INSTALL_STEAM": "1",
            "INSTALL_NVIDIA": "0",
            "INSTALL_APPLE_DMA": "1",
            "ARTIFACT_SHA256": "0" * 64,
            "ARTIFACT_SIZE": "1",
            "BUILD_LOG": str(build_log),
        })
        for ready, expected in (("false", False), ("true", True)):
            build_log.write_text(f"[qemu-vfio-image] apple_dma_ready={ready}\n")
            subprocess.run([sys.executable, "-", str(manifest_path)],
                           input=manifest_program, text=True, env=env,
                           check=True)
            manifest = json.loads(manifest_path.read_text())
            assert manifest["features"]["apple_dma"] is expected


if __name__ == "__main__":
    main()
