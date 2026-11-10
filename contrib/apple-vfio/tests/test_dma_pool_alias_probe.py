#!/usr/bin/env python3
"""Unit tests for the QEMU-backed DMA alias probe driver."""

import argparse
import importlib.util
import io
import pathlib
import struct
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/dma_pool_alias_probe.py"
SPEC = importlib.util.spec_from_file_location("dma_pool_alias_probe", SCRIPT)
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)


class FakeStream:
    def __init__(self, response):
        self.response = io.BytesIO(response)
        self.written = bytearray()

    def readline(self):
        return self.response.readline()

    def write(self, data):
        self.written.extend(data)


class AliasProbeTest(unittest.TestCase):
    def test_numeric_arguments(self):
        self.assertEqual(PROBE.number("0x8000000"), 0x8000000)
        with self.assertRaises(argparse.ArgumentTypeError):
            PROBE.number("0")
        with self.assertRaises(argparse.ArgumentTypeError):
            PROBE.number("not-a-number")

    def test_qmp_skips_events_and_checks_errors(self):
        stream = FakeStream(
            b'{"event":"RESET"}\r\n{"return":{"ready":true}}\r\n')
        self.assertEqual(PROBE.qmp_command(stream, "qmp_capabilities"),
                         {"ready": True})
        self.assertIn(b'"execute": "qmp_capabilities"', stream.written)

        stream = FakeStream(b'{"error":{"desc":"rejected"}}\r\n')
        with self.assertRaisesRegex(RuntimeError, "rejected"):
            PROBE.qmp_command(stream, "device_add")

    def test_qtest_rejects_non_ok_response(self):
        stream = FakeStream(b"FAIL unmapped\r\n")
        with self.assertRaisesRegex(RuntimeError, "unmapped"):
            PROBE.qtest_command(stream, "readq 0x1000")

    def test_invalid_layout_fails_before_launch(self):
        with self.assertRaisesRegex(ValueError, "multiple of alignment"):
            PROBE.probe("not-a-qemu", "", "44:00.0", None, 1, 8193, 4096)

    def test_driver_round_trip_with_fake_qemu_protocol(self):
        fake_qemu = r'''#!/usr/bin/env python3
import json
import socket
import struct
import sys
import threading

def listener(option):
    spec = sys.argv[sys.argv.index(option) + 1]
    path = spec.split(":", 1)[1].split(",", 1)[0]
    server = socket.socket(socket.AF_UNIX)
    server.bind(path)
    server.listen(1)
    return server

qmp_server = listener("-qmp")
qtest_server = listener("-qtest")
memory = bytearray(8)

def serve_qtest():
    conn, _ = qtest_server.accept()
    with conn, conn.makefile("rwb", buffering=0) as stream:
        for line in stream:
            parts = line.decode().split()
            if parts[0] == "writeq":
                memory[:] = struct.pack("<Q", int(parts[2], 0))
                stream.write(b"OK\n")
            elif parts[0] == "readq":
                stream.write(("OK 0x%016x\n" %
                              struct.unpack("<Q", memory)[0]).encode())
                break
            else:
                stream.write(b"FAIL unsupported\n")

thread = threading.Thread(target=serve_qtest)
thread.start()
conn, _ = qmp_server.accept()
with conn, conn.makefile("rwb", buffering=0) as stream:
    stream.write(b'{"QMP":{}}\r\n')
    for line in stream:
        request = json.loads(line)
        name = request["execute"]
        args = request.get("arguments", {})
        if name == "qom-get":
            prop = args["property"]
            if prop == "dma-address":
                value = 0x128000000
            elif prop == "cpu-mapping-size":
                value = 0x10000000
            elif prop == "test-value":
                value = struct.unpack("<Q", memory)[0]
            else:
                raise RuntimeError(prop)
        elif name == "query-memory-devices":
            value = [{"data": {"id": "poolprobe-dimm",
                               "addr": 0x128000000,
                               "size": 0x10000000}}]
        elif name in ("qmp_capabilities", "device_add", "qom-set"):
            if name == "qom-set":
                memory[:] = struct.pack("<Q", args["value"])
            value = {}
        elif name == "quit":
            stream.write(b'{"return":{}}\r\n')
            break
        else:
            raise RuntimeError(name)
        stream.write((json.dumps({"return": value}) + "\r\n").encode())
thread.join()
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "fake-qemu"
            executable.write_text(fake_qemu)
            executable.chmod(0o755)
            result = PROBE.probe(
                str(executable), "", "44:00.0", None, 5,
                PROBE.SIZE, PROBE.ALIGNMENT)
        self.assertEqual(result["result"], "qemu-host-alias-observed")
        self.assertTrue(result["qemu_guest_physical_write_observed_by_host"])
        self.assertTrue(result["host_write_observed_through_qemu_address_space"])


if __name__ == "__main__":
    unittest.main()
