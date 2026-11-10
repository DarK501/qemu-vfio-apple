#!/usr/bin/env python3
"""Test synthetic guest pool observation without physical hardware."""

import importlib.util
import pathlib
import struct
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/dma_pool_guest_probe.py"
spec = importlib.util.spec_from_file_location("probe", SCRIPT)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

IOMEM = "00001000-00003fff : System RAM\n  00001000-00001fff : Kernel code\n"


def flags(values):
    def read(offset, length):
        return b"".join(struct.pack("=Q", value) for value in values)[:length]
    return read


class GuestProbeTest(unittest.TestCase):
    def test_observed_pages(self):
        calls = []

        def read(offset, length):
            calls.append((offset, length))
            return bytes(length)

        result = probe.inspect(0x1000, 0x3000, 4096, IOMEM, read)
        self.assertEqual(result["result"], "pages-observed")
        self.assertEqual(result["pages_checked"], 3)
        self.assertEqual(calls, [(8, 24)])
        self.assertGreaterEqual(len(result["not_verified"]), 5)

    def test_not_ram_and_unavailable_pages(self):
        self.assertEqual(probe.inspect(0x4000, 4096, 4096, IOMEM, flags([]))["result"],
                         "not-system-ram")
        result = probe.inspect(0x1000, 8192, 4096, IOMEM,
                               flags([probe.KPF_NOPAGE, probe.KPF_OFFLINE]))
        self.assertEqual(result["result"], "pages-unavailable")
        self.assertEqual((result["no_page_count"], result["offline_count"]), (1, 1))

    def test_errors_are_explicit(self):
        with self.assertRaisesRegex(ValueError, "redacted"):
            probe.inspect(0, 4096, 4096, "00000000-00000000 : System RAM\n", flags([]))
        with self.assertRaisesRegex(ValueError, "aligned"):
            probe.inspect(1, 4096, 4096, IOMEM, flags([]))
        with self.assertRaisesRegex(ValueError, "overflowing"):
            probe.inspect((1 << 64) - 4096, 8192, 4096, IOMEM, flags([]))
        with self.assertRaisesRegex(ValueError, "short read"):
            probe.inspect(0x1000, 4096, 4096, IOMEM, flags([]))


if __name__ == "__main__":
    unittest.main()
