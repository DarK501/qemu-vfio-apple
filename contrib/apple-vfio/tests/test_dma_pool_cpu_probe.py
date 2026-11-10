#!/usr/bin/env python3
"""Tests for the bare-metal diagnostic and its synthetic QEMU control."""

import contextlib
import hashlib
import importlib.util
import io
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("dma_pool_cpu_probe",
                                               SCRIPTS / "dma_pool_cpu_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class CpuProbeTest(unittest.TestCase):
    def test_address_validation(self):
        for value in (0, 0xfffff000, 0x128001000, 1 << 52, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                probe.validate_address(value)
        probe.validate_address(0x128000000)

    def test_overlay_rejects_non_overlay(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "not-overlay"
            path.write_bytes(b"QFI\xfb" + bytes(32))
            with self.assertRaises(ValueError):
                probe.validate_overlay(path)

    @unittest.skipUnless(sys.platform.startswith("linux") and
                         all(shutil.which(x) for x in ("as", "ld")),
                         "GNU assembler and linker required")
    def test_embedded_payload_matches_source(self):
        with tempfile.TemporaryDirectory() as directory:
            obj = pathlib.Path(directory) / "payload.o"
            binary = pathlib.Path(directory) / "payload.bin"
            subprocess.run(["as", "--32", str(SCRIPTS / "dma_pool_cpu_payload.S"),
                            "-o", str(obj)], check=True)
            subprocess.run(["ld", "-m", "elf_i386", "-Ttext", "0x1000",
                            "--oformat", "binary", str(obj), "-o", str(binary)],
                           check=True)
            self.assertEqual(binary.read_bytes(), probe.PAYLOAD)
            self.assertLess(len(probe.PAYLOAD), 0x1000)

    @unittest.skipUnless(shutil.which("qemu-system-x86_64") and shutil.which("qemu-img"),
                         "QEMU and qemu-img required")
    def test_real_tcg_cpu_access_and_overlay_unchanged(self):
        qemu = shutil.which("qemu-system-x86_64")
        img = shutil.which("qemu-img")
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory) / "base.qcow2"
            overlay = pathlib.Path(directory) / "overlay.qcow2"
            subprocess.run([img, "create", "-q", "-f", "qcow2", str(base), "1M"],
                           check=True)
            subprocess.run([img, "create", "-q", "-f", "qcow2", "-F", "qcow2",
                            "-b", str(base), str(overlay)], check=True)
            before = [hashlib.sha256(path.read_bytes()).digest()
                      for path in (base, overlay)]
            result = probe.probe(qemu, directory, str(overlay),
                                 None, 0x128000000, "tcg", 20)
            self.assertEqual(result["result"], "synthetic-guest-cpu-alias-observed")
            self.assertEqual(result["guest_physical_address"], 0x128000000)
            self.assertTrue(result["guest_write_observed_by_host"])
            self.assertTrue(result["host_write_observed_by_guest_cpu"])
            self.assertEqual(before, [hashlib.sha256(path.read_bytes()).digest()
                                      for path in (base, overlay)])
            with mock.patch.object(probe, "PAYLOAD", b"\xfa\xf4"), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "timed out waiting for mailbox 1"):
                    probe.probe(qemu, directory, str(overlay),
                                None, 0x128000000, "tcg", 2)
            self.assertEqual(before, [hashlib.sha256(path.read_bytes()).digest()
                                      for path in (base, overlay)])


if __name__ == "__main__":
    unittest.main()
