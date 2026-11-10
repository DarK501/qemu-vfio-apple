#!/usr/bin/env python3
"""Exercise placement probe QMP interaction with a synthetic QEMU process."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/dma_pool_placement_probe.py"
FAKE_QEMU = r"""
import json, socket, sys
args = sys.argv
device = args[args.index("-device") + 1]
addr = int(device.split("addr=")[1], 0)
if addr == 0x200000000:
    print("synthetic placement failure", file=sys.stderr)
    sys.exit(1)
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
    if name == "query-memory-devices":
        value = [{"data": {"id": "poolprobe", "addr": addr if addr != 0x300000000 else 1,
                           "size": 268435456}, "type": "dimm"}]
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


class PlacementProbeTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.qemu = pathlib.Path(self.directory.name) / "fake-qemu"
        self.qemu.write_text(f"#!{sys.executable}\n" + FAKE_QEMU)
        self.qemu.chmod(0o700)

    def run_probe(self, address):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--qemu", str(self.qemu),
             "--firmware", self.directory.name, "--address", address,
             "--timeout", "5"],
            text=True, capture_output=True, timeout=15)

    def test_exact_address(self):
        proc = self.run_probe("0x100000000")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        result = json.loads(proc.stdout)
        self.assertEqual(result["result"], "synthetic-placement-observed")
        self.assertEqual(result["memory_device"]["addr"], 0x100000000)
        self.assertTrue(result["not_verified"])

    def test_startup_error_is_not_a_placement_conclusion(self):
        proc = self.run_probe("0x200000000")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("synthetic placement failure", proc.stderr)
        self.assertEqual(proc.stdout, "")

    def test_unexpected_report_is_an_error(self):
        proc = self.run_probe("0x300000000")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("unexpected placement", proc.stderr)

    def test_zero_does_not_request_fixed_placement(self):
        proc = self.run_probe("0")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("must be between", proc.stderr)


if __name__ == "__main__":
    unittest.main()
