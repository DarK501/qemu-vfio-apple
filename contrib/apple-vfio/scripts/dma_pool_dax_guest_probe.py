#!/usr/bin/env python3
"""Check CPU access to a dedicated DAX namespace backed by the DMA pool."""

import argparse
import json
import mmap
import os
import re
import stat
import struct
import sys
from pathlib import Path


PAGE_SIZE = mmap.PAGESIZE
GUEST_WRITE = 0xA1B2C3D4E5F60718
HOST_WRITE = 0x1827364554637281


def number(text):
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= value < 1 << 64:
        raise argparse.ArgumentTypeError("must be between 0 and 2^64-1")
    return value


def validate_device(device, offset):
    path = Path(device)
    if (path.parent != Path("/dev") or
            not re.fullmatch(r"dax[0-9]+\.[0-9]+", path.name)):
        raise ValueError("device must be a /dev/daxN.N character device")
    if not stat.S_ISCHR(path.stat().st_mode):
        raise ValueError(f"{device} is not a character device")
    if offset % PAGE_SIZE:
        raise ValueError(f"offset must be aligned to {PAGE_SIZE} bytes")

    size_path = Path("/sys/bus/dax/devices") / path.name / "size"
    try:
        size = int(size_path.read_text().strip(), 0)
    except (OSError, ValueError) as exc:
        raise ValueError(f"cannot determine capacity of {device}: {exc}") from exc
    if offset > size or PAGE_SIZE > size - offset:
        raise ValueError(f"test page at offset 0x{offset:x} exceeds {device} size "
                         f"0x{size:x}")


def probe(device, offset):
    raise RuntimeError(
        "probe disabled: DAX namespace identity, alignment and allocation "
        "offset are not yet verified")


def unverified_probe(device, offset):
    validate_device(device, offset)
    fd = os.open(device, os.O_RDWR | getattr(os, "O_CLOEXEC", 0))
    try:
        with mmap.mmap(fd, PAGE_SIZE, flags=mmap.MAP_SHARED,
                       prot=mmap.PROT_READ | mmap.PROT_WRITE,
                       offset=offset) as mapping:
            guest_bytes = struct.pack("<Q", GUEST_WRITE)
            mapping[:len(guest_bytes)] = guest_bytes
            observed = struct.unpack("<Q", mapping[:len(guest_bytes)])[0]
            if observed != GUEST_WRITE:
                raise RuntimeError(
                    f"guest CPU read back 0x{observed:016x}, "
                    f"expected 0x{GUEST_WRITE:016x}")
            print(json.dumps({
                "result": "guest-cpu-write-ready",
                "device": device,
                "offset": offset,
                "guest_cpu_write_readback": f"0x{observed:016x}",
                "next": "set the QEMU test-value property to HOST_WRITE, "
                        "then type continue",
            }), flush=True)
            if sys.stdin.readline().strip() != "continue":
                raise RuntimeError("expected 'continue' after the host write")

            observed = struct.unpack("<Q", mapping[:len(guest_bytes)])[0]
            if observed != HOST_WRITE:
                raise RuntimeError(
                    f"guest CPU read 0x{observed:016x} after host write, "
                    f"expected 0x{HOST_WRITE:016x}")
            return {
                "result": "guest-cpu-host-alias-observed",
                "device": device,
                "offset": offset,
                "host_write_read_by_guest_cpu": f"0x{observed:016x}",
                "not_verified": [
                    "Linux DMA-pool attachment",
                    "physical device DMA, coherency, or quiescence",
                    "general host DMA lifecycle safety",
                ],
            }
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", help="dedicated /dev/daxN.N namespace device")
    parser.add_argument("--offset", type=number, default=0,
                        help="page-aligned byte offset (default: 0)")
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.device, args.offset), indent=2))
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"dma-pool-dax-guest-probe: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
