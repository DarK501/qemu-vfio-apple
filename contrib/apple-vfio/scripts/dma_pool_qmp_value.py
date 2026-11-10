#!/usr/bin/env python3
"""Read or set the diagnostic value on a running DMA-pool QEMU backend."""

import argparse
import json
import socket
import sys


HOST_WRITE = 0x1827364554637281


def number(text):
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= value < 1 << 64:
        raise argparse.ArgumentTypeError("must be between 0 and 2^64-1")
    return value


def receive(stream):
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP disconnected")
        message = json.loads(line)
        if "event" not in message:
            return message


def command(stream, name, arguments=None):
    request = {"execute": name}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write((json.dumps(request) + "\r\n").encode())
    response = receive(stream)
    if "error" in response:
        raise RuntimeError(f"QMP {name}: {response['error']}")
    if "return" not in response:
        raise RuntimeError(f"unexpected QMP {name} response: {response}")
    return response["return"]


def validate_request(action, value):
    if action not in ("get", "set"):
        raise ValueError("action must be get or set")
    if (action == "set") != (value is not None):
        raise ValueError("set requires a value; get does not accept one")
    if action == "set" and value != HOST_WRITE:
        raise ValueError(f"this probe requires value 0x{HOST_WRITE:016x}")


def run(path, action, value):
    validate_request(action, value)
    with socket.socket(socket.AF_UNIX) as qmp:
        qmp.settimeout(10)
        qmp.connect(path)
        with qmp.makefile("rwb", buffering=0) as stream:
            greeting = receive(stream)
            if "QMP" not in greeting:
                raise RuntimeError(f"invalid QMP greeting: {greeting}")
            command(stream, "qmp_capabilities")
            prop = {"path": "/objects/poolprobe", "property": "test-value"}
            if action == "get":
                result = command(stream, "qom-get", prop)
                if type(result) is not int or not 0 <= result < 1 << 64:
                    raise RuntimeError(f"QMP returned invalid test-value: {result!r}")
                return result
            command(stream, "qom-set", {**prop, "value": value})
            return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("socket", help="QMP Unix socket path")
    parser.add_argument("action", choices=("get", "set"))
    parser.add_argument("value", nargs="?", type=number)
    args = parser.parse_args()
    try:
        value = run(args.socket, args.action, args.value)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"dma-pool-qmp-value: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"action": args.action, "test_value": f"0x{value:016x}"}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
