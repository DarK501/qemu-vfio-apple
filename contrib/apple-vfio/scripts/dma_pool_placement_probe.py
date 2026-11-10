#!/usr/bin/env python3
"""Test a synthetic pc-dimm address in a paused, device-free QEMU VM."""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time


def number(text):
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 < value < 1 << 64:
        raise argparse.ArgumentTypeError("must be between 1 and 2^64-1")
    return value


def receive(stream):
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP disconnected")
        message = json.loads(line)
        if "event" not in message:
            return message


def command(stream, name):
    stream.write((json.dumps({"execute": name}) + "\n").encode())
    response = receive(stream)
    if "error" in response:
        raise RuntimeError(f"QMP {name}: {response['error']}")
    if "return" not in response:
        raise RuntimeError(f"unexpected QMP {name} response: {response}")
    return response["return"]


def probe(qemu, firmware, address, timeout):
    with tempfile.TemporaryDirectory(prefix="dma-pool-placement-") as directory:
        path = os.path.join(directory, "qmp.sock")
        args = [
            qemu, "-L", firmware, "-machine", "q35,accel=tcg",
            "-nodefaults", "-display", "none", "-S",
            "-m", "2G,slots=1,maxmem=2304M",
            "-object", "memory-backend-ram,id=poolprobe,size=256M",
            "-device", f"pc-dimm,id=poolprobe,memdev=poolprobe,addr={address:#x}",
            "-qmp", f"unix:{path},server=on,wait=off",
        ]
        with tempfile.TemporaryFile(mode="w+b") as stderr:
            process = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=stderr)
            completed = False
            try:
                deadline = time.monotonic() + timeout
                while not os.path.exists(path) and process.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("timed out waiting for QMP socket")
                    time.sleep(0.05)
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited during startup ({process.returncode})")
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(max(0.1, deadline - time.monotonic()))
                    sock.connect(path)
                    with sock.makefile("rwb", buffering=0) as stream:
                        greeting = receive(stream)
                        if "QMP" not in greeting:
                            raise RuntimeError(f"invalid QMP greeting: {greeting}")
                        command(stream, "qmp_capabilities")
                        devices = command(stream, "query-memory-devices")
                        matches = [item["data"] for item in devices
                                   if item.get("data", {}).get("id") == "poolprobe"]
                        if len(matches) != 1:
                            raise RuntimeError("QMP did not report exactly one poolprobe")
                        actual = matches[0]
                        if actual.get("addr") != address or actual.get("size") != 256 * 1024 * 1024:
                            raise RuntimeError(f"QMP reported unexpected placement: {actual}")
                        command(stream, "quit")
                process.wait(timeout=timeout)
                if process.returncode:
                    raise RuntimeError(f"QEMU exited with status {process.returncode}")
                stderr.seek(0)
                warnings = stderr.read().decode("utf-8", errors="replace")
                completed = True
                return {
                    "result": "synthetic-placement-observed",
                    "memory_device": actual,
                    "qemu_stderr": warnings,
                    "not_verified": [
                        "guest firmware and Linux expose the range as usable RAM",
                        "host DriverKit can allocate a mapping at this address",
                        "stock Linux attaches the range to a PCI device as a restricted pool",
                        "physical DMA, coherency, and lifecycle are safe",
                    ],
                }
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                if not completed:
                    stderr.seek(0)
                    output = stderr.read().decode("utf-8", errors="replace")
                    if output:
                        print(f"QEMU stderr:\n{output}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, help="path to qemu-system-x86_64")
    parser.add_argument("--firmware", required=True, help="QEMU firmware/data directory")
    parser.add_argument("--address", required=True, type=number,
                        help="synthetic guest physical address (decimal or 0x...)")
    parser.add_argument("--timeout", type=int, default=30, help="timeout in seconds")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        result = probe(args.qemu, args.firmware, args.address, args.timeout)
    except (OSError, RuntimeError, ValueError, TimeoutError,
            subprocess.TimeoutExpired) as exc:
        print(f"dma-pool-placement-probe: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
