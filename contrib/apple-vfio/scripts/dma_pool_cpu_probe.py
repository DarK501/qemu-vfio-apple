#!/usr/bin/env python3
"""Run a bare-metal guest CPU alias probe against a dedicated DMA buffer."""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from dma_pool_alias_probe import connect_unix, number, qmp_command, qtest_command, receive_json
from dma_pool_hvf_baseline import ALIGNMENT, SIZE, has_rw_mapping_trace

PAYLOAD = bytes.fromhex(
    "fa31c08ed88ec08ed0bc0070e4920c02e6920f0116b0100f20c06683c801"
    "0f22c066ea29100000080066b810008ed88ec08ed00f20e083c8200f22e0"
    "b8002000000f22d80f20c00d000000800f22c0eb00bf00000040c7071807"
    "f6e5c74704d4c3b2a1c7050050000001000000833d005000000275f78b07"
    "a3085000008b4704a30c500000c7050050000003000000f4ebfd8db60000"
    "00000000000000000000ffff0000009acf00ffff00000092cf00170098100000"
)
GUEST_VALUE = 0xA1B2C3D4E5F60718
HOST_VALUE = 0x1827364554637281
MAILBOX = 0x5000


def validate_overlay(path):
    with open(path, "rb") as image:
        header = image.read(20)
    if len(header) != 20:
        raise ValueError("overlay has a truncated qcow2 header")
    magic, version, backing_offset, backing_size = struct.unpack(">4sIQI", header)
    if (magic != b"QFI\xfb" or version not in (2, 3) or
            backing_offset == 0 or backing_size == 0):
        raise ValueError("overlay must be a qcow2 image with a backing file")


def validate_address(address):
    if (type(address) is not int or address < 0x100000000 or
            address % ALIGNMENT or address > (1 << 52) - SIZE):
        raise ValueError(f"unusable or unaligned test address: {address!r}")


def write_value(stream, address, value, width="q"):
    qtest_command(stream, f"write{width} 0x{address:x} 0x{value:x}")


def read_value(stream, address, width="q"):
    return int(qtest_command(stream, f"read{width} 0x{address:x}").split()[1], 0)


def wait_mailbox(stream, value, deadline, process):
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited while waiting for mailbox ({process.returncode})")
        observed = read_value(stream, MAILBOX, "l")
        if observed == value:
            return
        if observed not in (0, 1, 2):
            raise RuntimeError(f"unexpected mailbox value {observed}")
        time.sleep(0.01)
    raise TimeoutError(f"timed out waiting for mailbox {value}")


def probe(qemu, firmware, overlay, bdf, address, accel, timeout):
    validate_overlay(overlay)
    if bdf and not re.fullmatch(r"[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", bdf):
        raise ValueError("bdf must have the form BB:DD.F")
    if address is not None:
        validate_address(address)
    with tempfile.TemporaryDirectory(prefix="dma-cpu-probe-") as directory:
        qmp_path = os.path.join(directory, "qmp.sock")
        qtest_path = os.path.join(directory, "qtest.sock")
        bios = bytearray(65536)
        bios[-16:-11] = bytes.fromhex("ea00100000")
        bios_path = os.path.join(directory, "bios.bin")
        Path(bios_path).write_bytes(bios)
        backend = (f"memory-backend-apple-dma,id=poolprobe,size={SIZE},"
                   f"alignment={ALIGNMENT},bdf={bdf},diagnostic-ram-device=on"
                   if bdf else f"memory-backend-ram,id=poolprobe,size={SIZE}")
        args = [
            qemu, "-L", firmware, "-machine", f"q35,accel={accel},nvdimm=on",
            "-cpu", "host" if accel == "hvf" else "max", "-smp", "1",
            "-nodefaults", "-display", "none", "-no-reboot", "-S",
            "-m", "2G,slots=1,maxmem=2304M",
            "-bios", bios_path, "-object", backend,
            "-blockdev", json.dumps({"driver": "file", "filename": overlay,
                                     "node-name": "probe-file", "read-only": True}),
            "-blockdev", json.dumps({"driver": "qcow2", "file": "probe-file",
                                     "node-name": "probe-overlay", "read-only": True}),
            "-device", "virtio-blk-pci,drive=probe-overlay",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-qtest", f"unix:{qtest_path},server=on,wait=off",
            "-qtest-log", os.devnull,
        ]
        if accel == "hvf":
            args += ["-trace", "enable=hvf_vm_map", "-trace", "enable=hvf_vm_unmap"]
        with tempfile.TemporaryFile(mode="w+b") as stderr:
            process = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=stderr)
            stage = "startup"
            complete = False
            try:
                deadline = time.monotonic() + timeout
                while process.poll() is None and not (
                        os.path.exists(qmp_path) and os.path.exists(qtest_path)):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("timed out waiting for QEMU sockets")
                    time.sleep(0.05)
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited during startup ({process.returncode})")
                with connect_unix(qmp_path, deadline) as qmp_sock, \
                     connect_unix(qtest_path, deadline) as qt_sock, \
                     qmp_sock.makefile("rwb", buffering=0) as qmp, \
                     qt_sock.makefile("rwb", buffering=0) as qt:
                    if "QMP" not in receive_json(qmp):
                        raise RuntimeError("invalid QMP greeting")
                    qmp_command(qmp, "qmp_capabilities")
                    if bdf:
                        stage = "query DMA allocation"
                        address = qmp_command(qmp, "qom-get", {
                            "path": "/objects/poolprobe", "property": "dma-address"})
                        size = qmp_command(qmp, "qom-get", {
                            "path": "/objects/poolprobe", "property": "cpu-mapping-size"})
                        if type(size) is not int or size < SIZE:
                            raise RuntimeError(f"invalid CPU mapping size: {size!r}")
                    validate_address(address)
                    stage = "hot-add NVDIMM"
                    qmp_command(qmp, "device_add", {
                        "driver": "nvdimm", "id": "poolprobe-dimm",
                        "memdev": "poolprobe", "addr": address})
                    devices = qmp_command(qmp, "query-memory-devices")
                    matches = [item["data"] for item in devices
                               if item.get("data", {}).get("id") == "poolprobe-dimm"]
                    if (len(matches) != 1 or matches[0].get("addr") != address or
                            matches[0].get("size") != SIZE):
                        raise RuntimeError(f"unexpected NVDIMM placement: {matches!r}")
                    if bdf:
                        trace = os.pread(stderr.fileno(), os.fstat(stderr.fileno()).st_size,
                                         0).decode("utf-8", errors="replace")
                        if not has_rw_mapping_trace(trace, address):
                            raise RuntimeError("expected RW-only HVF mapping trace not found")
                    stage = "install CPU payload"
                    qtest_command(qt, f"write 0x1000 {len(PAYLOAD)} 0x{PAYLOAD.hex()}")
                    qtest_command(qt, "memset 0x2000 0x4000 0x00")
                    for addr, value in ((0x2000, 0x3001), (0x2008, 0x4001),
                                        (0x3000, 0x83), (0x4000, address | 0x83),
                                        (MAILBOX, 0), (MAILBOX + 8, 0)):
                        write_value(qt, addr, value)
                    stage = "guest CPU write"
                    qmp_command(qmp, "cont")
                    wait_mailbox(qt, 1, deadline, process)
                    qmp_command(qmp, "stop")
                    observed = (qmp_command(qmp, "qom-get", {
                        "path": "/objects/poolprobe", "property": "test-value"})
                        if bdf else read_value(qt, address))
                    if observed != GUEST_VALUE:
                        raise RuntimeError(f"host observed unexpected value {observed!r}")
                    stage = "guest CPU read"
                    if bdf:
                        qmp_command(qmp, "qom-set", {
                            "path": "/objects/poolprobe", "property": "test-value",
                            "value": HOST_VALUE})
                    else:
                        write_value(qt, address, HOST_VALUE)
                    write_value(qt, MAILBOX, 2, "l")
                    qmp_command(qmp, "cont")
                    wait_mailbox(qt, 3, deadline, process)
                    qmp_command(qmp, "stop")
                    observed = read_value(qt, MAILBOX + 8)
                    if observed != HOST_VALUE:
                        raise RuntimeError(f"guest CPU observed unexpected value {observed!r}")
                    stage = "shutdown"
                    qmp_command(qmp, "quit")
                process.wait(timeout=timeout)
                if process.returncode:
                    raise RuntimeError(f"QEMU exited with status {process.returncode}")
                stderr.seek(0)
                output = stderr.read().decode("utf-8", errors="replace")
                if bdf and "failed to release DMA pool" in output:
                    raise RuntimeError("QEMU reported a DMA pool cleanup failure")
                if bdf and not has_rw_mapping_trace(output, address):
                    raise RuntimeError("expected RW-only HVF mapping trace not found")
                complete = True
                return {
                    "result": ("driverkit-guest-cpu-alias-observed" if bdf else
                               "synthetic-guest-cpu-alias-observed"),
                    "guest_physical_address": address, "size": SIZE,
                    "guest_write_observed_by_host": True,
                    "host_write_observed_by_guest_cpu": True,
                    "qemu_stderr": output,
                    "not_verified": ["Linux access or DMA-pool attachment",
                                     "physical device DMA, coherency, or quiescence",
                                     "general host DMA lifecycle safety",
                                     "RAM-device semantics are suitable for guest RAM"],
                }
            except (OSError, RuntimeError, TimeoutError, ValueError,
                    subprocess.TimeoutExpired) as exc:
                status = process.poll()
                detail = (f"; QEMU terminated by signal {-status}" if status is not None
                          and status < 0 else
                          f"; QEMU exited with status {status}" if status is not None
                          else "")
                raise RuntimeError(f"{stage}: {exc}{detail}") from exc
            finally:
                if process.poll() is None:
                    # Preserve normal QEMU cleanup even when the probe fails.
                    try:
                        with connect_unix(qmp_path, time.monotonic() + 2) as sock:
                            with sock.makefile("rwb", buffering=0) as stream:
                                if "QMP" not in receive_json(stream):
                                    raise RuntimeError("invalid cleanup QMP greeting")
                                qmp_command(stream, "qmp_capabilities")
                                qmp_command(stream, "quit")
                        process.wait(timeout=5)
                    except (OSError, RuntimeError, TimeoutError,
                            subprocess.TimeoutExpired) as exc:
                        print(f"graceful cleanup failed: {exc}", file=sys.stderr)
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                if not complete:
                    stderr.seek(0)
                    output = stderr.read().decode("utf-8", errors="replace")
                    if output:
                        print(f"QEMU stderr:\n{output}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--overlay", required=True,
                        help="dedicated persistent qcow2 overlay, attached read-only")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--bdf")
    group.add_argument("--address", type=number)
    parser.add_argument("--confirm-claim", action="store_true")
    parser.add_argument("--accel", choices=("hvf", "tcg"), default="hvf")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    if bool(args.bdf) != args.confirm_claim:
        parser.error("--bdf and --confirm-claim must be supplied together")
    if args.bdf and args.accel != "hvf":
        parser.error("DriverKit mode requires HVF")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        result = probe(args.qemu, args.firmware, os.path.abspath(args.overlay),
                       args.bdf, args.address, args.accel, args.timeout)
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"dma-pool-cpu-probe: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
