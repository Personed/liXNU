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


def choose_display_backend(qemu, host, requested=None):
    host_preference = {
        "Linux": ("gtk", "sdl"),
        "Darwin": ("cocoa", "sdl"),
        "Windows": ("sdl",),
    }
    if requested is not None and requested not in ("gtk", "sdl", "cocoa"):
        raise ValueError(f"unsupported graphical backend: {requested}")
    preferred = (requested,) if requested is not None else host_preference.get(host)
    if preferred is None:
        raise ValueError(f"no graphical QEMU backend configured for {host}")

    result = subprocess.run(
        [qemu, "-display", "help"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    available_text = result.stdout or ""
    available = {
        backend
        for backend in ("gtk", "sdl", "cocoa")
        if any(line.strip().split()[0].split(",", 1)[0].rstrip(":") == backend
               for line in available_text.splitlines() if line.strip())
    }
    headless = (
        "run headless after removing --graphical, --display-backend, "
        "and --window-title"
    )
    if requested is not None and requested not in available:
        choices = ", ".join(sorted(available)) or "none detected"
        if host == "Windows" and "sdl" in available:
            recovery = (
                "use --display-backend sdl or omit --display-backend to use "
                f"the Windows default; {headless}"
            )
        elif host == "Windows":
            alternative = (
                f"; try an available backend with --display-backend ({choices}) "
                "if supported by this QEMU build"
                if available else ""
            )
            switch_to_sdl = (
                "; after installing SDL, retry with --display-backend sdl or "
                "remove --display-backend to use the Windows default"
                if requested != "sdl" else "; retry after enabling SDL support"
            )
            recovery = (
                "Windows graphical mode requires SDL for its default; install "
                f"QEMU with SDL support{alternative}{switch_to_sdl}; {headless}"
            )
        elif available:
            recovery = (
                f"select an available backend with --display-backend ({choices}) "
                f"or omit --display-backend to use the host default; {headless}"
            )
        else:
            supported = "GTK or SDL" if host == "Linux" else "Cocoa or SDL"
            recovery = f"install a QEMU build with {supported} support, or {headless}"
        raise ValueError(
            f"QEMU does not provide the requested display backend {requested!r} "
            f"(available: {choices}); {recovery}"
        )
    if host == "Windows" and requested is None and "sdl" not in available:
        choices = ", ".join(sorted(available)) or "none detected"
        alternative = (
            f" or select an available backend with --display-backend ({choices}) "
            "if it is supported by this QEMU build"
            if available else ""
        )
        raise ValueError(
            f"QEMU has no graphical backend suitable for Windows (available: {choices}); "
            "Windows graphical mode requires SDL; install QEMU with SDL support"
            f"{alternative}; {headless}"
        )
    for backend in preferred:
        if backend in available:
            return backend

    choices = ", ".join(sorted(available)) or "none detected"
    supported = "GTK or SDL" if host == "Linux" else "Cocoa or SDL"
    raise ValueError(
        f"QEMU has no graphical backend suitable for {host} (available: {choices}); "
        f"install a QEMU build with {supported} support, or {headless}"
    )


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
        display = choose_display_backend(
            args.qemu, platform.system(), getattr(args, "display_backend", None)
        )
        command += [
            "-display", display,
            "-device", "virtio-gpu-pci",
            "-device", "qemu-xhci",
            "-device", "usb-kbd",
            "-device", "usb-tablet",
        ]
        window_title = getattr(args, "window_title", None)
        if window_title:
            escaped_title = window_title.replace(",", ",,")
            command += ["-name", f"guest={escaped_title}"]
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
    parser.add_argument(
        "--display-backend", choices=("gtk", "sdl", "cocoa"),
        help="force a graphical backend instead of selecting the host default",
    )
    parser.add_argument("--window-title", help="set the QEMU window title (requires --graphical)")
    parser.add_argument("--qemu", default="qemu-system-aarch64", help="path to the QEMU executable")
    args = parser.parse_args()
    if args.window_title and not args.graphical:
        parser.error("--window-title requires --graphical")
    if args.display_backend and not args.graphical:
        parser.error("--display-backend requires --graphical")
    if args.ssh_port is not None and args.ssh_port > 65535:
        parser.error("--ssh-port must be between 1 and 65535")

    files = [args.disk, args.firmware_code, args.firmware_vars]
    if args.installer:
        files.append(args.installer)
    for index, path in enumerate(files):
        if any(path.samefile(other) for other in files[:index]):
            parser.error("the disk, firmware images and installer must be separate files")

    try:
        command = build_command(args)
    except ValueError as error:
        parser.error(str(error))

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
