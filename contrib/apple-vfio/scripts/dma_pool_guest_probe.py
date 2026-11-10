#!/usr/bin/env python3
"""Inspect a synthetic guest-physical RAM range; never perform device DMA."""

import argparse
import json
import os
import re
import struct
import sys

U64_MAX = (1 << 64) - 1
KPF_NOPAGE = 1 << 20
KPF_OFFLINE = 1 << 23


def inspect(start, size, page_size, iomem, read_flags):
    if start < 0 or size <= 0 or start > U64_MAX or size - 1 > U64_MAX - start:
        raise ValueError("invalid or overflowing physical range")
    if page_size <= 0 or page_size & (page_size - 1):
        raise ValueError("invalid guest page size")
    if start % page_size or size % page_size:
        raise ValueError("range must be page aligned")

    end = start + size - 1
    ram = []
    for line in iomem.splitlines():
        match = re.fullmatch(r"\s*([0-9a-fA-F]+)-([0-9a-fA-F]+)\s*:\s*System RAM\s*", line)
        if match:
            ram.append((int(match.group(1), 16), int(match.group(2), 16)))
    if not any(last > first for first, last in ram):
        raise ValueError("System RAM ranges missing or redacted in /proc/iomem")
    covered = any(first <= start and end <= last for first, last in ram)
    if not covered:
        return {"result": "not-system-ram", "start": start, "size": size}

    page_count = size // page_size
    first_pfn = start // page_size
    missing = offline = 0
    for offset in range(0, page_count, 8192):
        count = min(8192, page_count - offset)
        raw = read_flags((first_pfn + offset) * 8, count * 8)
        if len(raw) != count * 8:
            raise ValueError("short read from /proc/kpageflags")
        for (flags,) in struct.iter_unpack("=Q", raw):
            missing += bool(flags & KPF_NOPAGE)
            offline += bool(flags & KPF_OFFLINE)
    return {
        "result": "pages-unavailable" if missing or offline else "pages-observed",
        "start": start,
        "size": size,
        "pages_checked": page_count,
        "no_page_count": missing,
        "offline_count": offline,
        "not_verified": [
            "range is reserved or safe to use as a DMA pool",
            "pages are accessible through the kernel linear map",
            "stock Linux attaches the range to a PCI device as a restricted pool",
            "host DMA addresses match guest physical addresses",
            "device DMA, coherency, and lifecycle are safe",
        ],
    }


def number(value):
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("start", type=number, help="guest physical start (decimal or 0x...)")
    parser.add_argument("size", type=number, help="size in bytes (decimal or 0x...)")
    args = parser.parse_args()
    try:
        with open("/proc/iomem", encoding="utf-8") as source:
            iomem = source.read()
        fd = os.open("/proc/kpageflags", os.O_RDONLY)
        try:
            result = inspect(args.start, args.size, os.sysconf("SC_PAGE_SIZE"),
                             iomem, lambda offset, length: os.pread(fd, length, offset))
        finally:
            os.close(fd)
    except (OSError, OverflowError, ValueError) as exc:
        print(f"dma-pool-guest-probe: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["result"] == "pages-observed" else 1


if __name__ == "__main__":
    sys.exit(main())
