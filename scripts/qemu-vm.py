#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Start an ARM64 Linux VM with full-system software emulation."""

import argparse
import os
import platform
from pathlib import Path
import subprocess
import sys


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def existing_file(value):
    path = Path(value).resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file does not exist: {value}")
    return path


def drive(path, options):
    # QEMU's -drive parser uses doubled commas for literal commas in paths.
    return f"file={str(path).replace(',', ',,')},{options}"


def build_command(args):
    network = "user,id=net0"
    if args.ssh_port:
        network += f",hostfwd=tcp:127.0.0.1:{args.ssh_port}-:22"
    command = [
        args.qemu,
        "-machine", "virt,virtualization=off",
        "-accel", "tcg",
        "-cpu", "max",
        "-smp", str(args.cpus),
        "-m", str(args.memory),
        "-nodefaults",
        "-serial", "mon:stdio",
        "-drive", drive(args.firmware_code, "if=pflash,format=raw,readonly=on"),
        "-drive", drive(args.firmware_vars, "if=pflash,format=raw"),
        "-drive", drive(args.disk, f"if=none,id=linux,format={args.disk_format}"),
        "-device", "virtio-blk-pci,drive=linux,bootindex=1",
        "-netdev", network,
        "-device", "virtio-net-pci,netdev=net0,romfile=",
        "-device", "virtio-rng-pci",
    ]
    if args.installer:
        command += [
            "-device", "virtio-scsi-pci,id=scsi",
            "-drive", drive(args.installer, "if=none,id=installer,format=raw,media=cdrom,readonly=on"),
            "-device", "scsi-cd,bus=scsi.0,drive=installer,bootindex=0",
        ]
    if args.graphical:
        display = {
            "Linux": "gtk",
            "Darwin": "cocoa",
            "Windows": "sdl",
        }.get(platform.system())
        if display is None:
            raise ValueError(f"no graphical QEMU backend configured for {platform.system()}")
        command += [
            "-display", display,
            "-device", "virtio-gpu-pci",
            "-device", "qemu-xhci",
            "-device", "usb-kbd",
            "-device", "usb-tablet",
        ]
    else:
        command += ["-display", "none"]
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=existing_file, help="ARM64 Linux VM disk")
    parser.add_argument("--disk-format", choices=("qcow2", "raw"), default="qcow2")
    parser.add_argument("--firmware-code", required=True, type=existing_file, help="AArch64 UEFI code image")
    parser.add_argument("--firmware-vars", required=True, type=existing_file, help="private writable copy of UEFI variables")
    parser.add_argument("--installer", type=existing_file, help="boot an ARM64 ISO before the VM disk")
    parser.add_argument("--memory", type=positive_int, default=16384, help="guest memory in MiB (default: 16384)")
    parser.add_argument("--cpus", type=positive_int, default=4, help="guest CPUs (default: 4)")
    parser.add_argument("--ssh-port", type=positive_int, help="forward this host loopback port to guest TCP port 22")
    parser.add_argument("--graphical", action="store_true", help="enable a QEMU window and virtual display")
    parser.add_argument("--qemu", default="qemu-system-aarch64", help="path to the QEMU executable")
    args = parser.parse_args()
    if args.ssh_port is not None and args.ssh_port > 65535:
        parser.error("--ssh-port must be between 1 and 65535")

    files = [args.disk, args.firmware_code, args.firmware_vars]
    if args.installer:
        files.append(args.installer)
    for index, path in enumerate(files):
        if any(path.samefile(other) for other in files[:index]):
            parser.error("the disk, firmware images and installer must be separate files")

    command = build_command(args)

    try:
        # POSIX supervisors can signal QEMU by the PID they started.
        if platform.system() != "Windows":
            os.execvp(command[0], command)
        result = subprocess.run(command)
    except OSError as error:
        parser.exit(1, f"{parser.prog}: cannot launch QEMU: {error}\n")
    except KeyboardInterrupt:
        return 130
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
