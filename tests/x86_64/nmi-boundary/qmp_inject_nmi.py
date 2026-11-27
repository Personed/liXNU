#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run one disposable TCG guest and inject one real QMP NMI."""

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path


READY_BYTE = b"N"
READY_TIMEOUT_SECONDS = 300
VM_TIMEOUT_SECONDS = 300


class ProbeFailure(RuntimeError):
    pass


def qmp_command(stream, command, command_id):
    request = {"execute": command, "id": command_id}
    stream.write((json.dumps(request) + "\r\n").encode("ascii"))
    stream.flush()
    while True:
        line = stream.readline()
        if not line:
            raise ProbeFailure(f"QMP closed while waiting for {command}")
        message = json.loads(line)
        if message.get("id") != command_id:
            continue
        if "error" in message:
            raise ProbeFailure(f"QMP {command} failed: {message['error']}")
        if "return" not in message:
            raise ProbeFailure(f"QMP {command} returned an invalid response")
        return message["return"]


def read_text(path):
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def fail_with_log(message, log_path):
    log = read_text(log_path)
    if log:
        print("----- guest serial log -----", file=sys.stderr)
        print(log[-30000:], file=sys.stderr, end="" if log.endswith("\n") else "\n")
        print("----- end guest serial log -----", file=sys.stderr)
    raise ProbeFailure(message)


def run(args):
    qmp_path = Path(args.qmp_socket)
    serial_path = Path(args.serial_log)
    debug_path = Path(args.debug_log)
    for path in (qmp_path, serial_path, debug_path):
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    qemu_cmd = [
        args.qemu,
        "-machine", "q35",
        "-accel", "tcg,thread=single",
        "-cpu", "max,hypervisor=on",
        "-smp", "1,sockets=1,cores=1,threads=1",
        "-m", "1024",
        "-kernel", args.kernel,
        "-initrd", args.initrd,
        "-append", "console=ttyS0,115200 rdinit=/init panic=1 loglevel=7 nmi_watchdog=0 nowatchdog",
        "-display", "none",
        "-serial", "stdio",
        "-monitor", "none",
        "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-debugcon", f"file:{debug_path}",
        "-global", "isa-debugcon.iobase=0xe9",
        "-no-reboot",
    ]
    print("Starting disposable QEMU TCG guest with one vCPU")
    process = None
    serial_file = None
    qmp_socket = None
    qmp_stream = None
    try:
        serial_file = serial_path.open("wb")
        process = subprocess.Popen(
            qemu_cmd,
            stdin=subprocess.DEVNULL,
            stdout=serial_file,
            stderr=subprocess.STDOUT,
        )

        connect_deadline = time.monotonic() + READY_TIMEOUT_SECONDS
        while True:
            if process.poll() is not None:
                fail_with_log(f"QEMU exited before QMP became ready (status {process.returncode})", serial_path)
            if qmp_path.exists():
                try:
                    qmp_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    qmp_socket.settimeout(10)
                    qmp_socket.connect(str(qmp_path))
                    break
                except OSError:
                    if qmp_socket is not None:
                        qmp_socket.close()
                        qmp_socket = None
            if time.monotonic() >= connect_deadline:
                fail_with_log("timed out waiting for the QMP socket", serial_path)
            time.sleep(0.05)

        qmp_stream = qmp_socket.makefile("rwb", buffering=0)
        greeting_line = qmp_stream.readline()
        if not greeting_line:
            raise ProbeFailure("QMP closed before sending its greeting")
        greeting = json.loads(greeting_line)
        if "QMP" not in greeting:
            raise ProbeFailure("QMP greeting was missing")
        qmp_command(qmp_stream, "qmp_capabilities", "nmi_caps")

        ready_deadline = time.monotonic() + READY_TIMEOUT_SECONDS
        while True:
            if process.poll() is not None:
                fail_with_log(f"QEMU exited before the shim-ready byte (status {process.returncode})", serial_path)
            try:
                debug_data = debug_path.read_bytes()
            except FileNotFoundError:
                debug_data = b""
            if READY_BYTE in debug_data:
                if debug_data != READY_BYTE:
                    raise ProbeFailure(f"unexpected debugcon handshake contents: {debug_data!r}")
                break
            if time.monotonic() >= ready_deadline:
                fail_with_log("timed out waiting for the active shim-ready byte", serial_path)
            time.sleep(0.02)

        qmp_command(qmp_stream, "inject-nmi", "nmi_inject_once")
        print("QMP inject-nmi command issued exactly once")

        try:
            status = process.wait(timeout=VM_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            fail_with_log(f"QEMU timed out after {VM_TIMEOUT_SECONDS} seconds", serial_path)

        if status != 0:
            fail_with_log(f"QEMU exited with nonzero status {status}", serial_path)

        serial_file.flush()
        serial_text = read_text(serial_path)
        debug_data = debug_path.read_bytes() if debug_path.exists() else b""
        required = (
            "NMI_BOUNDARY_RESULT",
            "X86_NMI_BOUNDARY_USER_PASS",
            "X86_NMI_BOUNDARY_VM_PASS",
            "reboot: Power down",
        )
        missing = [marker for marker in required if marker not in serial_text]
        if missing:
            fail_with_log(f"guest serial output missed required markers: {', '.join(missing)}", serial_path)
        if serial_text.count("X86_NMI_BOUNDARY_USER_PASS") != 1:
            raise ProbeFailure("the userspace success marker did not appear exactly once")
        if serial_text.count("X86_NMI_BOUNDARY_VM_PASS") != 1:
            raise ProbeFailure("the VM success marker did not appear exactly once")
        if debug_data != READY_BYTE:
            raise ProbeFailure(f"debugcon handshake was not exactly one byte: {debug_data!r}")
        lower = serial_text.lower()
        forbidden = ("kernel panic", "oops:", "bug:", "not syncing:", "dazed and confused")
        found = [marker for marker in forbidden if marker in lower]
        if found:
            fail_with_log(f"guest log contains fatal kernel output: {', '.join(found)}", serial_path)
        print("NMI boundary VM passed; one QMP NMI, clean guest power-off, QEMU status 0")
    finally:
        if qmp_stream is not None:
            qmp_stream.close()
        if qmp_socket is not None:
            qmp_socket.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if serial_file is not None:
            serial_file.close()
        try:
            qmp_path.unlink()
        except FileNotFoundError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--initrd", required=True)
    parser.add_argument("--qmp-socket", required=True)
    parser.add_argument("--serial-log", required=True)
    parser.add_argument("--debug-log", required=True)
    args = parser.parse_args()
    try:
        run(args)
    except (ProbeFailure, OSError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        print(f"nmi-boundary: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
