#!/usr/bin/env python3
"""Probe HVF memory hotplug with synthetic RAM, without a PCI claim."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

from dma_pool_alias_probe import connect_unix, number, qmp_command, receive_json


SIZE = 256 * 1024 * 1024


def probe(qemu, firmware, address, device, timeout):
    with tempfile.TemporaryDirectory(prefix="dma-pool-hvf-") as directory:
        path = os.path.join(directory, "qmp.sock")
        args = [
            qemu, "-L", firmware, "-machine", "q35,accel=hvf,nvdimm=on",
            "-cpu", "host", "-nodefaults", "-display", "none", "-S",
            "-m", "2G,slots=1,maxmem=2304M", "-smp", "4",
            "-object", f"memory-backend-ram,id=poolprobe,size={SIZE}",
            "-qmp", f"unix:{path},server=on,wait=off",
            "-trace", "enable=hvf_vm_map", "-trace", "enable=hvf_vm_unmap",
        ]
        with tempfile.TemporaryFile(mode="w+b") as stderr:
            process = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=stderr)
            stage = "startup"
            graceful = False
            try:
                deadline = time.monotonic() + timeout
                while not os.path.exists(path) and process.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("timed out waiting for QMP socket")
                    time.sleep(0.05)
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited during startup")
                with connect_unix(path, deadline) as sock:
                    with sock.makefile("rwb", buffering=0) as stream:
                        stage = "QMP greeting"
                        if "QMP" not in receive_json(stream):
                            raise RuntimeError("invalid QMP greeting")
                        qmp_command(stream, "qmp_capabilities")
                        stage = f"device_add {device}"
                        qmp_command(stream, "device_add", {
                            "driver": device,
                            "id": "poolprobe-dimm",
                            "memdev": "poolprobe",
                            "addr": address,
                        })
                        stage = "query-memory-devices"
                        devices = qmp_command(stream, "query-memory-devices")
                        matches = [item["data"] for item in devices
                                   if item.get("data", {}).get("id") ==
                                   "poolprobe-dimm"]
                        if len(matches) != 1:
                            raise RuntimeError("QMP did not report one test device")
                        actual = matches[0]
                        if (actual.get("addr") != address or
                                actual.get("size") != SIZE):
                            raise RuntimeError(f"unexpected placement: {actual}")
                        stage = "quit"
                        qmp_command(stream, "quit")
                process.wait(timeout=timeout)
                if process.returncode:
                    raise RuntimeError("QEMU failed during shutdown")
                graceful = True
                stderr.seek(0)
                return {
                    "result": "synthetic-hvf-hotplug-observed",
                    "memory_device": actual,
                    "qemu_stderr": stderr.read().decode("utf-8", errors="replace"),
                    "not_verified": [
                        "DriverKit-backed allocation can be mapped by HVF",
                        "guest CPU or Linux access",
                        "physical device DMA, coherency, or quiescence",
                    ],
                }
            except (OSError, RuntimeError, TimeoutError,
                    subprocess.TimeoutExpired) as exc:
                if process.poll() is None and stage != "startup":
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
                status = process.poll()
                if status is None:
                    detail = "QEMU was still running"
                elif status < 0:
                    detail = f"QEMU terminated by signal {-status}"
                else:
                    detail = f"QEMU exited with status {status}"
                raise RuntimeError(f"{stage}: {exc} ({detail})") from exc
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                if not graceful:
                    stderr.seek(0)
                    output = stderr.read().decode("utf-8", errors="replace")
                    if output:
                        print(f"QEMU stderr:\n{output}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--address", type=number, required=True,
                        help="synthetic guest physical address")
    parser.add_argument("--device", choices=("nvdimm", "pc-dimm"),
                        default="nvdimm")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        result = probe(args.qemu, args.firmware, args.address,
                       args.device, args.timeout)
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f"dma-pool-hvf-baseline: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
