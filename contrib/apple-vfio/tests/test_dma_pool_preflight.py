#!/usr/bin/env python3
"""Exercise the pool layout preflight without a physical device."""

import copy
import json
import pathlib
import subprocess
import sys
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/dma_pool_preflight.py"


def sample():
    return {
        "size": 8192,
        "cpu_mapping_size": 8192,
        "page_size": 4096,
        "dma_address_bits": 44,
        "guest_physical_bits": 40,
        "device_read": True,
        "device_write": True,
        "segments": [
            {"address": 0x100000000, "length": 4096, "offset": 0},
            {"address": 0x100001000, "length": 4096, "offset": 4096},
        ],
        "occupied_guest_ranges": [{"start": 0, "size": 0x100000000}],
    }


def run(data):
    return subprocess.run([sys.executable, str(SCRIPT), "-"],
                          input=json.dumps(data), text=True, capture_output=True)


class PreflightTest(unittest.TestCase):
    def test_candidate_is_not_a_safety_proof(self):
        proc = run(sample())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        result = json.loads(proc.stdout)
        self.assertEqual(result["result"], "candidate")
        self.assertEqual(result["pool_end_inclusive"], 0x100001fff)
        self.assertGreaterEqual(len(result["not_verified"]), 5)

    def test_each_constraint_rejects(self):
        mutations = [
            lambda x: x["segments"][1].update(address=0x200000000),
            lambda x: x.update(cpu_mapping_size=4096),
            lambda x: x.update(device_write=False),
            lambda x: x.update(page_size=16384),
            lambda x: x.update(dma_address_bits=32),
            lambda x: x.update(guest_physical_bits=32),
            lambda x: x["occupied_guest_ranges"].append(
                {"start": 0x100001fff, "size": 1}),
        ]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                data = sample()
                mutate(data)
                proc = run(data)
                self.assertEqual(proc.returncode, 1, proc.stderr)
                self.assertTrue(json.loads(proc.stdout)["failures"])

    def test_invalid_or_incomplete_input_is_an_error(self):
        mutations = [
            lambda x: x["segments"][1].update(offset=8192),
            lambda x: x["segments"][1].update(length=4095),
            lambda x: x["segments"][0].update(address=(1 << 64) - 1),
            lambda x: x.update(page_size=3000),
            lambda x: x.update(memory_block_size=3000),
            lambda x: x.update(memory_block_size=2048),
            lambda x: x.update(device_read=1),
            lambda x: x.pop("occupied_guest_ranges"),
            lambda x: x["occupied_guest_ranges"].append(
                {"start": (1 << 64) - 1, "size": 2}),
        ]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                data = copy.deepcopy(sample())
                mutate(data)
                proc = run(data)
                self.assertEqual(proc.returncode, 2, proc.stderr)
                self.assertIn("dma-pool-preflight:", proc.stderr)

    def test_hotplug_alignment_at_observed_addresses(self):
        data = sample()
        data.update(size=0x10000000, cpu_mapping_size=0x10000000,
                    memory_block_size=0x8000000,
                    segments=[{"address": 0x120400000,
                               "length": 0x10000000, "offset": 0}])
        proc = run(data)
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertIn("pool address or size is not memory-hotplug-block aligned",
                      json.loads(proc.stdout)["failures"])
        data["segments"][0]["address"] = 0x128000000
        proc = run(data)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["result"], "candidate")
        data["size"] = data["cpu_mapping_size"] = data["segments"][0]["length"] = 0x8001000
        proc = run(data)
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertIn("pool address or size is not memory-hotplug-block aligned",
                      json.loads(proc.stdout)["failures"])

    def test_unspecified_block_size_is_not_verified(self):
        result = json.loads(run(sample()).stdout)
        self.assertIn("Linux memory-hotplug-block alignment",
                      result["not_verified"])


if __name__ == "__main__":
    unittest.main()
