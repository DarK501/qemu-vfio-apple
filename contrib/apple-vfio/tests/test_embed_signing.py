#!/usr/bin/env python3
"""Exercise the embed script's signing options without macOS tools."""

import json
import os
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "contrib/apple-vfio/scripts/embed-qemu.sh"


def executable(path, text):
    path.write_text(text)
    path.chmod(0o755)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        src = root / "repo/contrib/apple-vfio"
        src.mkdir(parents=True)
        entitlements = root / "repo/accel/hvf/entitlements.plist"
        entitlements.parent.mkdir(parents=True)
        entitlements.write_text("test")
        dist = root / "dist"
        (dist / "lib").mkdir(parents=True)
        firmware = dist / "share/qemu"
        firmware.mkdir(parents=True)
        for name in ("kvmvapic.bin", "vgabios-stdvga.bin", "vgabios-virtio.bin",
                     "efi-virtio.rom", "edk2-x86_64-code.fd", "edk2-i386-vars.fd"):
            (firmware / name).write_text(name)
        for path in (dist / "qemu-system-x86_64", dist / "qemu-img",
                     dist / "lib/test.dylib"):
            path.write_text("binary")

        bin_dir = root / "bin"
        bin_dir.mkdir()
        executable(bin_dir / "uname", "#!/bin/sh\necho x86_64\n")
        executable(bin_dir / "rsync", "#!/bin/sh\ncp -R \"$3\". \"$4\"\n")
        executable(bin_dir / "codesign", """#!/usr/bin/env python3
import json, os, sys
with open(os.environ["SIGN_LOG"], "a") as out:
    out.write(json.dumps(sys.argv[1:]) + "\\n")
""")
        log = root / "sign.log"
        for configuration in ("Debug", "Release"):
            for identity, allowed in (("-", "YES"), ("Apple Development", "YES"),
                                      ("Apple Development", "NO")):
                log.write_text("")
                env = os.environ.copy()
                env.update(PATH=f"{bin_dir}:{env['PATH']}", SRCROOT=str(src),
                           QEMU_DIST_DIR=str(dist), TARGET_BUILD_DIR=str(root / "products"),
                           WRAPPER_NAME="Test.app", CONFIGURATION=configuration,
                           CODE_SIGN_IDENTITY=identity, CODE_SIGNING_ALLOWED=allowed,
                           CODE_SIGNING_REQUIRED="YES", SIGN_LOG=str(log))
                env.pop("EXPANDED_CODE_SIGN_IDENTITY", None)
                result = subprocess.run(["bash", str(SCRIPT)], env=env,
                                        text=True, capture_output=True)
                assert result.returncode == 0, result.stdout + result.stderr
                calls = [json.loads(line) for line in log.read_text().splitlines()]
                assert len(calls) == 3, calls
                expected_identity = "-" if allowed == "NO" else identity
                for args in calls:
                    assert args[:3] == ["--force", "--sign", expected_identity], args
                    assert ("--timestamp=none" in args) == (configuration == "Debug"), args
                    assert ("--options" in args) == (expected_identity != "-"), args
                    if "--options" in args:
                        assert args[args.index("--options") + 1] == "runtime", args
                assert "--entitlements" in calls[-1], calls
                assert all("--entitlements" not in args for args in calls[:-1]), calls


if __name__ == "__main__":
    main()
