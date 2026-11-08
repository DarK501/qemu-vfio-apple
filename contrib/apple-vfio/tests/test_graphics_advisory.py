#!/usr/bin/env python3
"""Compile and exercise the registry-evidence advisory with Swift."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "contrib/apple-vfio/qemu-vfio-apple/PCIServiceState.swift"


def main():
    with tempfile.TemporaryDirectory() as tmp:
        test = pathlib.Path(tmp) / "main.swift"
        binary = pathlib.Path(tmp) / "test"
        test.write_text("""
assert(hasExternalAMDGraphicsServices(classCode: 0x030000, tunnelled: true,
                                     driver: .ours(["AMDSupport"])))
assert(hasExternalAMDGraphicsServices(classCode: 0x030200, tunnelled: true,
                                     driver: .other(["AMDRadeonX6000_AmdGpuWrangler"])))
assert(!hasExternalAMDGraphicsServices(classCode: 0x030000, tunnelled: false,
                                      driver: .ours(["AMDSupport"])))
assert(!hasExternalAMDGraphicsServices(classCode: 0x040300, tunnelled: true,
                                      driver: .other(["AMDSupport"])))
assert(!hasExternalAMDGraphicsServices(classCode: 0x030000, tunnelled: true,
                                      driver: .ours([])))
assert(!hasExternalAMDGraphicsServices(classCode: 0x030000, tunnelled: true,
                                      driver: .other(["OtherService"])))
assert(!hasExternalAMDGraphicsServices(classCode: 0x030000, tunnelled: true,
                                      driver: .unknown))
assert(!hasExternalAMDGraphicsServices(classCode: 0x030000, tunnelled: true,
                                      driver: .none))
let message = externalGraphicsAdvisory(bdf: "44:00.0").joined(separator: " ")
assert(message.contains("44:00.0"))
assert(message.contains("menu bar"))
assert(message.contains("may remove"))
assert(message.contains("not prove"))
""")
        subprocess.run(["swiftc", str(SOURCE), str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
