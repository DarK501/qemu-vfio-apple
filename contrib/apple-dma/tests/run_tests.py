#!/usr/bin/env python3
"""Run the focused DMA protocol, ownership, and teardown regression harnesses."""

import pathlib
import subprocess
import sys


TESTS = (
    "test_pci_identity.py",
    "test_claim.py",
    "test_claim_client.py",
    "test_dma_ops_dispatch.py",
    "test_hook_lifecycle.py",
    "test_client_stop.py",
    "test_driver_stop.py",
    "test_interrupt_stop.py",
    "test_dma_buffer.py",
    "test_bounce_client.py",
    "test_bar_cache.py",
    "test_config_filter.py",
    "test_config_snapshot.py",
    "test_alloc_alignment.py",
    "test_dma_permissions.py",
    "test_irq_limits.py",
    "test_rebar.py",
    "test_atomic_rejection.py",
    "test_stream_alignment.py",
    "test_stream_rollback.py",
    "test_sync_range.py",
    "test_connection.py",
    "test_guest_protocol.py",
    "test_mapping.py",
    "test_mapping_id_flags.py",
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
