#!/usr/bin/env python3
"""Verify QEMU guest-physical writes alias a live DriverKit DMA allocation."""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time


SIZE = 256 * 1024 * 1024
ALIGNMENT = 128 * 1024 * 1024
GUEST_WRITE = 0xA1B2C3D4E5F60718
HOST_WRITE = 0x1827364554637281


def number(text):
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 < value < 1 << 64:
        raise argparse.ArgumentTypeError("must be between 1 and 2^64-1")
    return value


def receive_json(stream):
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP disconnected")
        message = json.loads(line)
        if "event" not in message:
            return message


def qmp_command(stream, name, arguments=None):
    request = {"execute": name}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write((json.dumps(request) + "\r\n").encode())
    response = receive_json(stream)
    if "error" in response:
        raise RuntimeError(f"QMP {name}: {response['error']}")
    if "return" not in response:
        raise RuntimeError(f"unexpected QMP {name} response: {response}")
    return response["return"]


def qtest_command(stream, command):
    stream.write((command + "\n").encode())
    response = stream.readline().decode().strip()
    if not response.startswith("OK"):
        raise RuntimeError(f"qtest {command!r}: {response or 'disconnected'}")
    return response


def connect_unix(path, deadline):
    while time.monotonic() < deadline:
        try:
            sock = socket.socket(socket.AF_UNIX)
            sock.settimeout(max(0.1, deadline - time.monotonic()))
            sock.connect(path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(0.05)
    raise TimeoutError(f"timed out connecting to {path}")


def probe(qemu, firmware, bdf, host_root, timeout, size, alignment):
    if size % alignment:
        raise ValueError("size must be a multiple of alignment")
    if size < 8:
        raise ValueError("size must be at least 8 bytes")
    with tempfile.TemporaryDirectory(prefix="dma-pool-alias-") as directory:
        qmp_path = os.path.join(directory, "qmp.sock")
        qtest_path = os.path.join(directory, "qtest.sock")
        backend = (
            f"memory-backend-apple-dma,id=poolprobe,size={size},"
            f"alignment={alignment},bdf={bdf}"
        )
        if host_root:
            backend += f",host-root={host_root}"
        args = [
            qemu, "-L", firmware, "-machine", "q35,accel=tcg",
            "-nodefaults", "-display", "none", "-S",
            "-m", "2G,slots=1,maxmem=2304M",
            "-object", backend,
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-qtest", f"unix:{qtest_path},server=on,wait=off",
        ]
        with tempfile.TemporaryFile(mode="w+b") as stderr:
            process = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=stderr)
            qmp_sock = qtest_sock = None
            qmp_stream = qtest_stream = None
            completed = False
            try:
                deadline = time.monotonic() + timeout
                while process.poll() is None and not (
                        os.path.exists(qmp_path) and os.path.exists(qtest_path)):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("timed out waiting for QEMU sockets")
                    time.sleep(0.05)
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited during startup ({process.returncode})")

                qmp_sock = connect_unix(qmp_path, deadline)
                qmp_stream = qmp_sock.makefile("rwb", buffering=0)
                greeting = receive_json(qmp_stream)
                if "QMP" not in greeting:
                    raise RuntimeError(f"invalid QMP greeting: {greeting}")
                qmp_command(qmp_stream, "qmp_capabilities")
                address = qmp_command(
                    qmp_stream, "qom-get",
                    {"path": "/objects/poolprobe", "property": "dma-address"})
                mapping_size = qmp_command(
                    qmp_stream, "qom-get",
                    {"path": "/objects/poolprobe",
                     "property": "cpu-mapping-size"})
                if (type(address) is not int or address == 0 or
                        address % alignment or mapping_size < size or
                        size - 1 > (1 << 64) - 1 - address):
                    raise RuntimeError(
                        f"backend returned unusable address/mapping: "
                        f"{address!r}/{mapping_size!r}")

                qmp_command(qmp_stream, "device_add", {
                    "driver": "pc-dimm",
                    "id": "poolprobe-dimm",
                    "memdev": "poolprobe",
                    "addr": address,
                })
                devices = qmp_command(qmp_stream, "query-memory-devices")
                matches = [item["data"] for item in devices
                           if item.get("data", {}).get("id") == "poolprobe-dimm"]
                if len(matches) != 1:
                    raise RuntimeError("QMP did not report exactly one test DIMM")
                dimm = matches[0]
                if dimm.get("addr") != address or dimm.get("size") != size:
                    raise RuntimeError(f"QEMU placed DIMM unexpectedly: {dimm}")

                qtest_sock = connect_unix(qtest_path, deadline)
                qtest_stream = qtest_sock.makefile("rwb", buffering=0)
                test_gpa = address
                qtest_command(qtest_stream,
                              f"writeq 0x{test_gpa:x} 0x{GUEST_WRITE:x}")
                host_observed = qmp_command(
                    qmp_stream, "qom-get",
                    {"path": "/objects/poolprobe", "property": "test-value"})
                if host_observed != GUEST_WRITE:
                    raise RuntimeError(
                        f"host mapping did not observe QEMU write: "
                        f"{host_observed!r}")

                qmp_command(qmp_stream, "qom-set", {
                    "path": "/objects/poolprobe",
                    "property": "test-value",
                    "value": HOST_WRITE,
                })
                guest_observed = qtest_command(qtest_stream,
                                               f"readq 0x{test_gpa:x}")
                if int(guest_observed.split()[1], 0) != HOST_WRITE:
                    raise RuntimeError(
                        f"QEMU address space did not observe host write: "
                        f"{guest_observed}")

                qmp_command(qmp_stream, "quit")
                process.wait(timeout=timeout)
                if process.returncode:
                    raise RuntimeError(f"QEMU exited with status {process.returncode}")
                stderr.seek(0)
                warnings = stderr.read().decode("utf-8", errors="replace")
                if ("failed to release DMA pool" in warnings or
                        "failed to release DMA pool claim" in warnings):
                    raise RuntimeError(
                        "QEMU reported an unverified DriverKit allocation teardown")
                completed = True
                return {
                    "result": "qemu-host-alias-observed",
                    "bdf": bdf,
                    "dma_address": address,
                    "size": size,
                    "qemu_guest_physical_write_observed_by_host": True,
                    "host_write_observed_through_qemu_address_space": True,
                    "qemu_stderr": warnings,
                    "not_verified": [
                        "guest CPU or Linux access (the VM remains paused)",
                        "Linux usable-RAM or restricted-DMA-pool attachment",
                        "physical device DMA, coherency, or quiescence",
                        "general host DMA lifecycle safety",
                    ],
                }
            finally:
                if qmp_stream is not None:
                    qmp_stream.close()
                if qtest_stream is not None:
                    qtest_stream.close()
                if qmp_sock is not None:
                    qmp_sock.close()
                if qtest_sock is not None:
                    qtest_sock.close()
                if process.poll() is None:
                    try:
                        process.terminate()
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
    parser.add_argument("--bdf", required=True, help="host PCI BDF, e.g. 44:00.0")
    parser.add_argument("--host-root", help="IORegistry root name if BDF is ambiguous")
    parser.add_argument("--size", type=number, default=SIZE)
    parser.add_argument("--alignment", type=number, default=ALIGNMENT)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--confirm-claim", action="store_true",
                        help="confirm that the selected device will be claimed")
    args = parser.parse_args()
    if not args.confirm_claim:
        parser.error("this experiment claims the host device; pass --confirm-claim")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        result = probe(args.qemu, args.firmware, args.bdf, args.host_root,
                       args.timeout, args.size, args.alignment)
    except (OSError, RuntimeError, ValueError, TimeoutError,
            subprocess.TimeoutExpired) as exc:
        print(f"dma-pool-alias-probe: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
