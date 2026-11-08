#!/usr/bin/env python3
"""Run the focused DMA protocol, ownership, and teardown regression harnesses."""

import pathlib
import subprocess
import sys


TESTS = (
    "test_claim.py",
    "test_client_stop.py",
    "test_driver_stop.py",
    "test_interrupt_stop.py",
    "test_dma_buffer.py",
    "test_bounce_client.py",
    "test_bar_cache.py",
    "test_connection.py",
    "test_guest_protocol.py",
    "test_mapping.py",
    "test_registration.py",
    "test_dext_registration.py",
    "test_teardown.py",
)


def main():
    test_dir = pathlib.Path(__file__).resolve().parent
    for test in TESTS:
        print(f"==> {test}", flush=True)
        subprocess.run([sys.executable, str(test_dir / test)], check=True)


if __name__ == "__main__":
    main()
