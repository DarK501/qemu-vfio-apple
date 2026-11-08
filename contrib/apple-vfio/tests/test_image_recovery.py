#!/usr/bin/env python3
"""Exercise image recovery and refusal to overwrite existing files."""

import os
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "contrib/apple-vfio/image-builder/build-ubuntu-desktop-image.sh"
NAME = "ubuntu-desktop-resolute-x86_64"
MOCK_IMG = """#!/bin/sh
case "$1" in
    check)
        if [ "${FAIL_CHECK:-0}" = 1 ]; then exit 1; fi
        test -s "$4"
        ;;
    convert)
        for last do :; done
        printf 'converted' > "$last"
        if [ "${FAIL_CONVERT:-0}" = 1 ]; then exit 1; fi
        if [ "${EMPTY_CONVERT:-0}" = 1 ]; then : > "$last"; fi
        ;;
    *) exit 2 ;;
esac
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        img = root / "qemu-img"
        img.write_text(MOCK_IMG)
        img.chmod(0o755)
        env = os.environ.copy()
        env.update(GUEST_ARCH="x86_64", QEMU_IMG_BIN=str(img))

        def case(name, log="provisioning complete, powering off\n"
                           "[ 123.000] reboot: Power down\n",
                 extra_env=None):
            output = root / name
            work = output / "work"
            work.mkdir(parents=True)
            disk = work / f"{NAME}.build.qcow2"
            disk.write_bytes(b"original")
            (work / f"{NAME}.build.log").write_text(log)
            case_env = env.copy()
            case_env.update(extra_env or {})

            def run():
                return subprocess.run(
                    ["bash", str(SCRIPT), str(output)], env=case_env,
                    text=True, capture_output=True,
                )

            return output, disk, run

        out, disk, run = case("success")
        result = run()
        artifact = out / "artifacts" / f"{NAME}.qcow2"
        assert result.returncode == 0, result.stderr
        assert artifact.read_bytes() == b"converted"
        assert disk.read_bytes() == b"original"
        assert not (out / "artifacts" / f"{NAME}.manifest.json").exists()
        assert "no manifest was generated" in result.stdout
        result = run()
        assert result.returncode != 0 and artifact.read_bytes() == b"converted"

        out, disk, run = case("no-shutdown", log="provisioning complete, powering off\n")
        result = run()
        assert result.returncode != 0 and "shutdown is not confirmed" in result.stderr
        assert disk.read_bytes() == b"original"
        assert not (out / "artifacts" / f"{NAME}.qcow2").exists()

        out, disk, run = case("no-marker", log="reboot: Power down\n")
        result = run()
        assert result.returncode != 0 and "without the success marker" in result.stderr
        assert disk.read_bytes() == b"original"

        for name, flag, message in (
            ("locked", "FAIL_CHECK", "build disk check failed"),
            ("failed-convert", "FAIL_CONVERT", "image conversion failed"),
            ("empty-convert", "EMPTY_CONVERT", "missing or invalid"),
        ):
            out, disk, run = case(name, extra_env={flag: "1"})
            result = run()
            assert result.returncode != 0 and message in result.stderr
            assert disk.read_bytes() == b"original"
            artifacts = out / "artifacts"
            assert not list(artifacts.iterdir())

        out, disk, run = case("existing-manifest")
        (out / "artifacts").mkdir()
        manifest = out / "artifacts" / f"{NAME}.manifest.json"
        manifest.write_text("existing")
        result = run()
        assert result.returncode != 0 and "manifest already exists" in result.stderr
        assert manifest.read_text() == "existing"
        assert disk.read_bytes() == b"original"

        real_img = os.environ.get("QEMU_IMG_REAL")
        if real_img:
            out = root / "real-image"
            work = out / "work"
            work.mkdir(parents=True)
            base = root / "base.qcow2"
            disk = work / f"{NAME}.build.qcow2"
            artifact = out / "artifacts" / f"{NAME}.qcow2"
            subprocess.run([real_img, "create", "-q", "-f", "qcow2",
                            str(base), "1M"], check=True)
            subprocess.run([real_img, "create", "-q", "-f", "qcow2",
                            "-F", "qcow2", "-b", str(base), str(disk)],
                           check=True)
            (work / f"{NAME}.build.log").write_text(
                "provisioning complete, powering off\nreboot: Power down\n")
            real_env = env.copy()
            real_env["QEMU_IMG_BIN"] = real_img
            result = subprocess.run(
                ["bash", str(SCRIPT), str(out)], env=real_env,
                text=True, capture_output=True,
            )
            assert result.returncode == 0, result.stderr
            subprocess.run([real_img, "compare", "-q", str(disk),
                            str(artifact)], check=True)
            info = subprocess.run([real_img, "info", "--output=json",
                                   str(artifact)], text=True,
                                  capture_output=True, check=True)
            assert '"backing-filename"' not in info.stdout


if __name__ == "__main__":
    main()
