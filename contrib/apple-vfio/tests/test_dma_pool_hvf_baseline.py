#!/usr/bin/env python3
"""Exercise the synthetic HVF hotplug runner's QMP and error handling."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/dma_pool_hvf_baseline.py"
FAKE_QEMU = r"""
import json, os, signal, socket, sys
args = sys.argv
assert "q35,accel=hvf,nvdimm=on" in args
backend = args[args.index("-object") + 1]
live = backend.startswith("memory-backend-apple-dma,")
assert live or backend == "memory-backend-ram,id=poolprobe,size=268435456"
assert "enable=hvf_vm_map" in args
path = args[args.index("-qmp") + 1].split(",")[0][5:]
sock = socket.socket(socket.AF_UNIX)
sock.bind(path)
sock.listen(1)
conn, _ = sock.accept()
stream = conn.makefile("rwb", buffering=0)
stream.write(b'{"QMP":{}}\n')
while True:
    request = json.loads(stream.readline())
    name = request["execute"]
    if name == "device_add":
        device = request["arguments"]
        if device["addr"] == 0x200000000 or "bdf=44:00.1" in backend:
            print("synthetic map failure", file=sys.stderr, flush=True)
            os.kill(os.getpid(), signal.SIGTERM)
        if device["addr"] == 0x300000000:
            stream.write(b'{"error":{"desc":"placement rejected"}}\n')
            continue
        assert device["driver"] in ("nvdimm", "pc-dimm")
        value = {}
    elif name == "qom-get":
        assert live
        prop = request["arguments"]["property"]
        value = {"dma-address": 0x128000000,
                 "cpu-mapping-size": 268435456}[prop]
    elif name == "query-memory-devices":
        value = [{"data": {"id": device["id"], "addr": device["addr"],
                           "size": 268435456}}]
    elif name == "quit" and "bdf=44:00.2" in backend:
        print("vfio-apple: failed to release DMA pool", file=sys.stderr,
              flush=True)
        value = {}
    else:
        value = {}
    stream.write((json.dumps({"event": "TEST"}) + "\n").encode())
    stream.write((json.dumps({"return": value}) + "\n").encode())
    if name == "quit":
        break
stream.close()
conn.close()
sock.close()
"""


class HVFBaselineTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.qemu = pathlib.Path(self.directory.name) / "fake-qemu"
        self.qemu.write_text(f"#!{sys.executable}\n" + FAKE_QEMU)
        self.qemu.chmod(0o700)

    def run_probe(self, address, device="nvdimm"):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name, "--address", address,
             "--device", device, "--timeout", "5"],
            text=True, capture_output=True, timeout=15)

    def test_hotplug_both_device_types(self):
        for device in ("nvdimm", "pc-dimm"):
            with self.subTest(device=device):
                result = self.run_probe("0x128000000", device)
                self.assertEqual(result.returncode, 0, result.stderr)
                output = json.loads(result.stdout)
                self.assertEqual(output["result"], "synthetic-hvf-hotplug-observed")
                self.assertEqual(output["memory_device"]["addr"], 0x128000000)

    def test_signal_and_stderr_are_reported(self):
        result = self.run_probe("0x200000000")
        self.assertEqual(result.returncode, 2)
        self.assertIn("device_add nvdimm", result.stderr)
        self.assertIn("signal 15", result.stderr)
        self.assertIn("synthetic map failure", result.stderr)

    def test_qmp_rejection_is_not_reported_as_success(self):
        result = self.run_probe("0x300000000")
        self.assertEqual(result.returncode, 2)
        self.assertIn("placement rejected", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_driverkit_hotplug_uses_live_address(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name, "--bdf", "44:00.0",
             "--confirm-claim", "--timeout", "5"],
            text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["result"], "driverkit-hvf-hotplug-observed")
        self.assertEqual(output["memory_device"]["addr"], 0x128000000)

    def test_driverkit_requires_confirmation(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name, "--bdf", "44:00.0"],
            text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--confirm-claim", result.stderr)

    def test_driverkit_abort_reports_signal_and_output(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name, "--bdf", "44:00.1",
             "--confirm-claim", "--timeout", "5"],
            text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn("device_add nvdimm", result.stderr)
        self.assertIn("signal 15", result.stderr)
        self.assertIn("synthetic map failure", result.stderr)

    def test_driverkit_cleanup_failure_is_not_a_pass(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name, "--bdf", "44:00.2",
             "--confirm-claim", "--timeout", "5"],
            text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn("DMA pool cleanup failure", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_bdf_rejects_qemu_option_injection(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name,
             "--bdf", "44:00.0,extra=on", "--confirm-claim"],
            text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn("bdf must have the form", result.stderr)


if __name__ == "__main__":
    unittest.main()
