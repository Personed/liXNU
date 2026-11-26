#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot the real PureDarwin guest under ARM64 QEMU TCG and require a clean halt."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("serial_log", type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=14400)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--qemu-memory", default="16G")
    parser.add_argument("--linux-memory", default="7G")
    parser.add_argument("--fki-carve-base", default="0x200000000")
    parser.add_argument("--fki-carve-mb", type=int, default=8156)
    parser.add_argument("--xnu-ram-mb", type=int, default=7900)
    parser.add_argument("--max-rss-mib", type=int)
    args = parser.parse_args()
    for path in (args.kernel, args.initramfs, args.disk):
        if not path.is_file():
            parser.error(f"missing input: {path}")
    qemu = shutil.which(args.qemu)
    if not qemu:
        parser.error(f"QEMU executable not found: {args.qemu}")
    if args.timeout_seconds < 1:
        parser.error("--timeout-seconds must be positive")
    if args.fki_carve_mb < 1 or args.xnu_ram_mb < 1:
        parser.error("FKI carve-out and XNU RAM sizes must be positive")
    try:
        carve_base = int(args.fki_carve_base, 0)
    except ValueError:
        parser.error("--fki-carve-base must be an integer address")
    if carve_base < 0 or carve_base >= 1 << 64:
        parser.error("--fki-carve-base must fit in a 64-bit address")
    carve_base_hex = hex(carve_base)
    required = (
        f"fki: {args.fki_carve_mb} MiB at {carve_base_hex} for the foreign kernel's DRAM",
        "xnu: contiguous DRAM at phys 0x",
        "fki: darwin console: XNULINUX_USERLAND_OK",
        "xnu: the hosted system halted",
        "XNULINUX_E2E_PUREDARWIN_USERLAND_OK",
        "XNULINUX_E2E_GUEST_HALT_OK",
        "XNULINUX_E2E_XNU_UNLOADED",
        "reboot: Power down",
    )

    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    command = [
        qemu,
        "-name", "xnulinux-puredarwin-e2e",
        "-machine", "virt,virtualization=off",
        "-accel", "tcg,thread=multi",
        "-cpu", "max",
        "-smp", "2",
        "-m", args.qemu_memory,
        "-nodefaults",
        "-display", "none",
        "-serial", f"file:{args.serial_log}",
        "-monitor", "none",
        "-no-reboot",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-device,rng=rng0",
        "-drive", f"if=none,id=puredarwin,format=raw,file={args.disk}",
        "-device", "virtio-blk-device,drive=puredarwin",
        "-kernel", str(args.kernel),
        "-initrd", str(args.initramfs),
        "-append", (
            "console=ttyAMA0 rdinit=/init nokaslr "
            f"mem={args.linux_memory} "
            f"xnulinux.carve_base={carve_base_hex} "
            f"xnulinux.carve_mb={args.fki_carve_mb} "
            f"xnulinux.xnu_ram_mb={args.xnu_ram_mb} "
            "loglevel=1"
        ),
    ]
    print(f"Host: {subprocess.run(['uname', '-a'], check=False, text=True, capture_output=True).stdout.strip()}")
    host_cpus = subprocess.run(["lscpu"], check=False, text=True, capture_output=True)
    for line in (host_cpus.stdout or "").splitlines():
        if line.startswith(("Architecture:", "CPU(s):", "Model name:", "Hypervisor vendor:", "Virtualization type:")):
            print(f"Host {line.strip()}")
    try:
        for memory_line in Path("/proc/meminfo").read_text().splitlines():
            if memory_line.startswith(("MemTotal:", "MemAvailable:")):
                print(f"Host {memory_line.strip()}")
    except OSError:
        pass
    version = subprocess.run([qemu, "--version"], check=False, text=True, capture_output=True)
    print(f"QEMU: {(version.stdout or version.stderr).splitlines()[0]}")
    print(
        "Profile: ARM64 QEMU virt, TCG, "
        f"{args.qemu_memory} guest RAM, Linux mem={args.linux_memory}, "
        f"{args.fki_carve_mb} MiB FKI carve-out at {carve_base_hex}, "
        f"XNU RAM {args.xnu_ram_mb} MiB"
    )
    timeout_cmd = [
        "timeout", "--foreground", "--signal=TERM", "--kill-after=30s",
        f"{args.timeout_seconds}s", *command,
    ]
    peak_rss_kib = 0
    stop_monitor = threading.Event()

    def monitor_qemu_rss() -> None:
        nonlocal peak_rss_kib
        qemu_name = os.path.basename(qemu)
        while not stop_monitor.is_set():
            for pid in filter(str.isdigit, os.listdir("/proc")):
                try:
                    with open(f"/proc/{pid}/cmdline", "rb") as cmdline_file:
                        executable = cmdline_file.read().split(b"\0", 1)[0].decode(errors="ignore")
                    if not executable.endswith(qemu_name):
                        continue
                    with open(f"/proc/{pid}/status", encoding="utf-8") as status_file:
                        for line in status_file:
                            if line.startswith("VmRSS:"):
                                peak_rss_kib = max(peak_rss_kib, int(line.split()[1]))
                                break
                except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
                    pass
            time.sleep(0.2)

    monitor = threading.Thread(target=monitor_qemu_rss, daemon=True)
    monitor.start()
    boot_error = None
    try:
        try:
            result = subprocess.run(timeout_cmd, check=False, timeout=args.timeout_seconds + 45)
        except (OSError, subprocess.TimeoutExpired) as error:
            boot_error = error
    finally:
        stop_monitor.set()
        monitor.join()

    print(f"QEMU peak RSS: {peak_rss_kib} KiB ({peak_rss_kib / 1048576:.2f} GiB)", flush=True)
    if boot_error is not None:
        print(f"TCG boot could not complete: {boot_error}", file=sys.stderr)
        return 1

    log = args.serial_log.read_text(encoding="utf-8", errors="replace") if args.serial_log.exists() else ""
    kmsg_prefix = re.compile(r"^\d+,\d+,\d+,[^;]*;(.*)$")
    messages = []
    for line in log.splitlines():
        match = kmsg_prefix.match(line)
        messages.append(match.group(1) if match else line)
    missing = [marker for marker in required if not any(marker in line for line in messages)]
    darwin_line = "fki: darwin console: Darwin"
    if darwin_line not in messages:
        missing.append("exact Darwin uname output line")
    base_digits = carve_base_hex[2:]
    if not any(re.search(rf"xnu: contiguous DRAM at phys 0x0*{re.escape(base_digits)}\b", line) for line in messages):
        missing.append(f"XNU contiguous DRAM at physical {carve_base_hex}")
    if result.returncode != 0 or missing:
        print(f"QEMU exit status: {result.returncode}", file=sys.stderr)
        if missing:
            print("Missing boot evidence: " + "; ".join(missing), file=sys.stderr)
        print("--- serial log tail ---", file=sys.stderr)
        print("\n".join(log.splitlines()[-160:]), file=sys.stderr)
        return 1

    for line in messages:
        if line == darwin_line or any(marker in line for marker in required):
            print(line)
    if args.max_rss_mib is not None:
        if peak_rss_kib == 0:
            print("QEMU RSS was not measured", file=sys.stderr)
            return 1
        if peak_rss_kib > args.max_rss_mib * 1024:
            print(f"QEMU peak RSS exceeded {args.max_rss_mib} MiB", file=sys.stderr)
            return 1
    print("PureDarwin ARM64 console boot and guest-requested halt passed under QEMU TCG.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
