#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot the real PureDarwin guest under ARM64 QEMU TCG and require a clean halt."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys


REQUIRED = (
    "fki: 8156 MiB at 0x200000000 for the foreign kernel's DRAM",
    "xnu: contiguous DRAM at phys 0x",
    "fki: darwin console: XNULINUX_USERLAND_OK",
    "xnu: the hosted system halted",
    "XNULINUX_E2E_PUREDARWIN_USERLAND_OK",
    "XNULINUX_E2E_GUEST_HALT_OK",
    "XNULINUX_E2E_XNU_UNLOADED",
    "reboot: Power down",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("serial_log", type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=14400)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    for path in (args.kernel, args.initramfs, args.disk):
        if not path.is_file():
            parser.error(f"missing input: {path}")
    qemu = shutil.which(args.qemu)
    if not qemu:
        parser.error(f"QEMU executable not found: {args.qemu}")
    if args.timeout_seconds < 1:
        parser.error("--timeout-seconds must be positive")

    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    command = [
        qemu,
        "-name", "xnulinux-puredarwin-e2e",
        "-machine", "virt,virtualization=off",
        "-accel", "tcg,thread=multi",
        "-cpu", "max",
        "-smp", "2",
        "-m", "16G",
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
        "-append", "console=ttyAMA0 rdinit=/init nokaslr mem=7G loglevel=1",
    ]
    print(f"Host: {subprocess.run(['uname', '-a'], check=False, text=True, capture_output=True).stdout.strip()}")
    host_cpus = subprocess.run(["lscpu"], check=False, text=True, capture_output=True)
    for line in (host_cpus.stdout or "").splitlines():
        if line.startswith(("Architecture:", "CPU(s):", "Model name:", "Hypervisor vendor:", "Virtualization type:")):
            print(f"Host {line.strip()}")
    try:
        memory_line = next(line for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemTotal:"))
        print(f"Host {memory_line.strip()}")
    except (OSError, StopIteration):
        pass
    version = subprocess.run([qemu, "--version"], check=False, text=True, capture_output=True)
    print(f"QEMU: {(version.stdout or version.stderr).splitlines()[0]}")
    print("Profile: ARM64 QEMU virt, TCG, 16 GiB guest RAM, Linux mem=7G, 8,156 MiB FKI carve-out")
    timeout_cmd = [
        "timeout", "--foreground", "--signal=TERM", "--kill-after=30s",
        f"{args.timeout_seconds}s", *command,
    ]
    try:
        result = subprocess.run(timeout_cmd, check=False, timeout=args.timeout_seconds + 45)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"TCG boot could not complete: {error}", file=sys.stderr)
        return 1

    log = args.serial_log.read_text(encoding="utf-8", errors="replace") if args.serial_log.exists() else ""
    kmsg_prefix = re.compile(r"^\d+,\d+,\d+,[^;]*;(.*)$")
    messages = []
    for line in log.splitlines():
        match = kmsg_prefix.match(line)
        messages.append(match.group(1) if match else line)
    missing = [marker for marker in REQUIRED if not any(marker in line for line in messages)]
    darwin_line = "fki: darwin console: Darwin"
    if darwin_line not in messages:
        missing.append("exact Darwin uname output line")
    if not any(re.search(r"xnu: contiguous DRAM at phys 0x0*200000000\b", line) for line in messages):
        missing.append("XNU contiguous DRAM at physical 0x200000000")
    if result.returncode != 0 or missing:
        print(f"QEMU exit status: {result.returncode}", file=sys.stderr)
        if missing:
            print("Missing boot evidence: " + "; ".join(missing), file=sys.stderr)
        print("--- serial log tail ---", file=sys.stderr)
        print("\n".join(log.splitlines()[-160:]), file=sys.stderr)
        return 1

    for line in messages:
        if line == darwin_line or any(marker in line for marker in REQUIRED):
            print(line)
    print("PureDarwin ARM64 console boot and guest-requested halt passed under QEMU TCG.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
