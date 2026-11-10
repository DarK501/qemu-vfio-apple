#!/usr/bin/env python3
"""Compare paused HVF memory hotplug with synthetic or DriverKit RAM."""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from dma_pool_alias_probe import connect_unix, number, qmp_command, receive_json


SIZE = 256 * 1024 * 1024
ALIGNMENT = 128 * 1024 * 1024


def probe(qemu, firmware, address, device, timeout, bdf=None):
    if bdf is not None and not re.fullmatch(r"[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", bdf):
        raise ValueError("bdf must have the form BB:DD.F")
    with tempfile.TemporaryDirectory(prefix="dma-pool-hvf-") as directory:
        path = os.path.join(directory, "qmp.sock")
        backend = (f"memory-backend-apple-dma,id=poolprobe,size={SIZE},"
                   f"alignment={ALIGNMENT},bdf={bdf}" if bdf else
                   f"memory-backend-ram,id=poolprobe,size={SIZE}")
        args = [
            qemu, "-L", firmware, "-machine", "q35,accel=hvf,nvdimm=on",
            "-cpu", "host", "-nodefaults", "-display", "none", "-S",
            "-m", "2G,slots=1,maxmem=2304M", "-smp", "4",
            "-object", backend,
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
                        if bdf:
                            stage = "query DriverKit allocation"
                            address = qmp_command(stream, "qom-get", {
                                "path": "/objects/poolprobe",
                                "property": "dma-address",
                            })
                            mapping_size = qmp_command(stream, "qom-get", {
                                "path": "/objects/poolprobe",
                                "property": "cpu-mapping-size",
                            })
                            if (type(address) is not int or address <= 0 or
                                    address % ALIGNMENT or
                                    address > (1 << 64) - SIZE or
                                    type(mapping_size) is not int or
                                    mapping_size < SIZE):
                                raise RuntimeError(
                                    f"unusable DriverKit allocation: "
                                    f"{address!r}/{mapping_size!r}")
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
                stderr.seek(0)
                output = stderr.read().decode("utf-8", errors="replace")
                if bdf and "failed to release DMA pool" in output:
                    raise RuntimeError("QEMU reported a DMA pool cleanup failure")
                graceful = True
                return {
                    "result": ("driverkit-hvf-hotplug-observed" if bdf else
                               "synthetic-hvf-hotplug-observed"),
                    "memory_device": actual,
                    "qemu_stderr": output,
                    "not_verified": [
                        "guest CPU or Linux access",
                        "physical device DMA, coherency, or quiescence",
                        "general host DMA lifecycle safety",
                    ],
                }
            except (OSError, RuntimeError, TimeoutError,
                    subprocess.TimeoutExpired) as exc:
                if process.poll() is None and stage != "startup":
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
                cleanup = ""
                if bdf and process.poll() is None:
                    try:
                        with connect_unix(path, time.monotonic() + 2) as sock:
                            with sock.makefile("rwb", buffering=0) as stream:
                                if "QMP" not in receive_json(stream):
                                    raise RuntimeError("invalid QMP greeting")
                                qmp_command(stream, "qmp_capabilities")
                                qmp_command(stream, "quit")
                        process.wait(timeout=5)
                        cleanup = "; QMP quit requested after failure"
                    except (OSError, RuntimeError, TimeoutError,
                            subprocess.TimeoutExpired) as cleanup_exc:
                        cleanup = f"; graceful shutdown failed: {cleanup_exc}"
                status = process.poll()
                if status is None:
                    detail = "QEMU was still running"
                elif status < 0:
                    detail = f"QEMU terminated by signal {-status}"
                else:
                    detail = f"QEMU exited with status {status}"
                raise RuntimeError(f"{stage}: {exc} ({detail}{cleanup})") from exc
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
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--address", type=number,
                       help="synthetic guest physical address")
    group.add_argument("--bdf", help="claim this PCI device for DriverKit RAM")
    parser.add_argument("--confirm-claim", action="store_true",
                        help="confirm that the selected device will be claimed")
    parser.add_argument("--device", choices=("nvdimm", "pc-dimm"),
                        default="nvdimm")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if bool(args.bdf) != args.confirm_claim:
        parser.error("--bdf and --confirm-claim must be supplied together")
    try:
        result = probe(args.qemu, args.firmware, args.address,
                       args.device, args.timeout, args.bdf)
        if not args.bdf:
            result["not_verified"].insert(
                0, "DriverKit-backed allocation can be mapped by HVF")
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"dma-pool-hvf-baseline: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
