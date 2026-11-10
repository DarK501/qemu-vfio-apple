#!/usr/bin/env python3
"""Tests for the Linux DAX and QMP sides of the DMA-pool probe."""

import argparse
import importlib.util
import json
import pathlib
import socket
import tempfile
import threading
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


DAX = load_script("dma_pool_dax_guest_probe.py")
QMP = load_script("dma_pool_qmp_value.py")


class NvdimmProbeTest(unittest.TestCase):
    def test_numeric_values(self):
        self.assertEqual(DAX.number("0x1000"), 0x1000)
        self.assertEqual(QMP.number("0x1827364554637281"),
                         QMP.HOST_WRITE)
        with self.assertRaises(argparse.ArgumentTypeError):
            DAX.number("-1")
        with self.assertRaises(argparse.ArgumentTypeError):
            QMP.number("0x10000000000000000")

    def test_guest_probe_rejects_non_dax_paths(self):
        with self.assertRaisesRegex(ValueError, "daxN.N"):
            DAX.validate_device("/dev/mem", 0)

    def test_guest_probe_disabled_before_device_access(self):
        with self.assertRaisesRegex(RuntimeError, "probe disabled"):
            DAX.probe("/dev/dax0.0", 0)

    def test_qmp_helper_reads_live_value(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(pathlib.Path(directory) / "qmp.sock")
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            server.listen(1)
            observed = []

            def serve():
                with server:
                    conn, _ = server.accept()
                    with conn, conn.makefile("rwb", buffering=0) as stream:
                        stream.write(b'{"QMP":{}}\r\n')
                        for response in ({"return": {}},
                                         {"return": 0xA1B2C3D4E5F60718}):
                            request = stream.readline()
                            parsed = json.loads(request)
                            observed.append(parsed)
                            stream.write((json.dumps(response) + "\r\n").encode())

            thread = threading.Thread(target=serve)
            thread.start()
            value = QMP.run(path, "get", None)
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
            self.assertEqual(value, 0xA1B2C3D4E5F60718)
            self.assertEqual(observed[0]["execute"], "qmp_capabilities")
            self.assertEqual(observed[1]["execute"], "qom-get")
            self.assertEqual(observed[1]["arguments"]["path"],
                             "/objects/poolprobe")

    def test_qmp_helper_writes_only_probe_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(pathlib.Path(directory) / "qmp.sock")
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            server.listen(1)
            observed = []

            def serve():
                with server:
                    conn, _ = server.accept()
                    with conn, conn.makefile("rwb", buffering=0) as stream:
                        stream.write(b'{"QMP":{}}\r\n')
                        for _ in range(2):
                            parsed = json.loads(stream.readline())
                            observed.append(parsed)
                            stream.write(b'{"return":{}}\r\n')

            thread = threading.Thread(target=serve)
            thread.start()
            self.assertEqual(QMP.run(path, "set", QMP.HOST_WRITE),
                             QMP.HOST_WRITE)
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
            self.assertEqual(observed[1]["execute"], "qom-set")
            self.assertEqual(observed[1]["arguments"]["value"], QMP.HOST_WRITE)

        with self.assertRaisesRegex(ValueError, "requires value"):
            QMP.validate_request("set", 1)


if __name__ == "__main__":
    unittest.main()
