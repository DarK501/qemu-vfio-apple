#!/usr/bin/env python3
"""Check necessary address-layout conditions for a prospective DMA pool.

This tool does not allocate memory, interact with a device, or prove that
Linux can attach the pool. See docs/system/devices/vfio-apple.rst.
"""

import argparse
import json
import sys

U64_MAX = (1 << 64) - 1


def integer(obj, key, low, high):
    value = obj.get(key)
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{key} must be an integer in [{low}, {high}]")
    return value


def interval(start, size, label):
    if size - 1 > U64_MAX - start:
        raise ValueError(f"{label} overflows a 64-bit address")
    return start, start + size - 1


def assess(data):
    if not isinstance(data, dict):
        raise ValueError("input must be a JSON object")
    size = integer(data, "size", 1, U64_MAX)
    mapping_size = integer(data, "cpu_mapping_size", 0, U64_MAX)
    page_size = integer(data, "page_size", 1, U64_MAX)
    if page_size & (page_size - 1):
        raise ValueError("page_size must be a power of two")
    dma_bits = integer(data, "dma_address_bits", 1, 64)
    guest_bits = integer(data, "guest_physical_bits", 1, 64)
    segments = data.get("segments")
    if not isinstance(segments, list) or not segments:
        raise ValueError("segments must be a nonempty array")
    if type(data.get("device_read")) is not bool or type(
            data.get("device_write")) is not bool:
        raise ValueError("device_read and device_write must be booleans")
    occupied = data.get("occupied_guest_ranges")
    if not isinstance(occupied, list):
        raise ValueError("occupied_guest_ranges must be an array")

    failures = []
    covered = 0
    base = None
    for index, segment in enumerate(segments):
        if not isinstance(segment, dict):
            raise ValueError(f"segment {index} must be an object")
        address = integer(segment, "address", 0, U64_MAX)
        length = integer(segment, "length", 1, U64_MAX)
        offset = integer(segment, "offset", 0, U64_MAX)
        interval(address, length, f"segment {index}")
        if offset != covered or length > size - covered:
            raise ValueError(f"segment {index} has invalid coverage")
        if base is None:
            base = address
        elif covered > U64_MAX - base or address != base + covered:
            failures.append(f"segment {index} is not DMA-contiguous")
        covered += length
    if covered != size:
        raise ValueError("segments do not cover the requested pool size")

    start, end = interval(base, size, "pool")
    if mapping_size < size:
        failures.append("CPU mapping is shorter than the pool")
    if not data["device_read"] or not data["device_write"]:
        failures.append("pool is not readable and writable by the device")
    if start % page_size or size % page_size:
        failures.append("pool address or size is not guest-page aligned")
    if end > (1 << dma_bits) - 1:
        failures.append("pool exceeds the device DMA address width")
    if end > (1 << guest_bits) - 1:
        failures.append("pool exceeds the guest physical address width")

    for index, item in enumerate(occupied):
        if not isinstance(item, dict):
            raise ValueError(f"occupied range {index} must be an object")
        other_start = integer(item, "start", 0, U64_MAX)
        other_size = integer(item, "size", 1, U64_MAX)
        _, other_end = interval(other_start, other_size,
                                f"occupied range {index}")
        if start <= other_end and other_start <= end:
            failures.append(f"pool overlaps occupied guest range {index}")

    return {
        "result": "rejected" if failures else "candidate",
        "pool_start": start,
        "pool_end_inclusive": end,
        "failures": failures,
        "not_verified": [
            "occupied guest ranges are complete or remain free at realize time",
            "guest firmware describes the pool as usable RAM with page metadata",
            "stock Linux attaches the pool to the intended PCI device",
            "all streaming, coherent, and other DMA paths use the pool",
            "host coherency, device quiescence, reset, and teardown are safe",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="JSON file, or - for standard input")
    args = parser.parse_args()
    try:
        if args.input == "-":
            data = json.load(sys.stdin)
        else:
            with open(args.input, encoding="utf-8") as source:
                data = json.load(source)
        result = assess(data)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"dma-pool-preflight: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 1 if result["failures"] else 0


if __name__ == "__main__":
    sys.exit(main())
