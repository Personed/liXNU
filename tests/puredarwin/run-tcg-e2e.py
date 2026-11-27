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
import time
from typing import Optional


QEMU_VIRT_RAM_BASE = 0x40000000
MEMORY_SUFFIXES = {
    "": 1,
    "K": 1024,
    "M": 1024**2,
    "G": 1024**3,
    "T": 1024**4,
}


def parse_memory_size(value: str, option: str, bare_unit_multiplier: int = 1) -> int:
    match = re.fullmatch(r"([0-9]+)([KMGT]?)(?:i?b?)?", value.strip(), re.IGNORECASE)
    if not match:
        raise ValueError(f"{option} must be a positive memory size such as 512M or 16G")
    suffix = match.group(2).upper()
    size = int(match.group(1)) * (MEMORY_SUFFIXES[suffix] if suffix else bare_unit_multiplier)
    if size < 1:
        raise ValueError(f"{option} must be positive")
    return size


def max_rss_kib(raw_maxrss: int, platform_name: Optional[str] = None) -> int:
    """Normalize wait4 ru_maxrss to KiB; macOS reports it in bytes."""
    if (platform_name or sys.platform) == "darwin":
        return (raw_maxrss + 1023) // 1024
    return raw_maxrss


def can_measure_child_rss() -> bool:
    return hasattr(os, "fork") and hasattr(os, "wait4")


def run_with_timeout(command: list[str], timeout_seconds: int) -> tuple[int, Optional[int], bool]:
    """Run one child, returning its exit status, exact peak RSS in KiB when measurable, and timeout flag."""
    if not can_measure_child_rss():
        try:
            result = subprocess.run(command, check=False, timeout=timeout_seconds)
            return result.returncode, None, False
        except subprocess.TimeoutExpired:
            return -15, None, True

    pid = os.fork()
    if pid == 0:
        try:
            os.execvp(command[0], command)
        except OSError as error:
            print(f"could not execute {command[0]}: {error}", file=sys.stderr)
            os._exit(127)

    deadline = time.monotonic() + timeout_seconds
    kill_deadline = None
    timed_out = False
    while True:
        done, status, usage = os.wait4(pid, os.WNOHANG)
        if done == pid:
            if os.WIFEXITED(status):
                returncode = os.WEXITSTATUS(status)
            elif os.WIFSIGNALED(status):
                returncode = -os.WTERMSIG(status)
            else:
                returncode = 1
            return returncode, max_rss_kib(usage.ru_maxrss), timed_out

        now = time.monotonic()
        if not timed_out and now >= deadline:
            timed_out = True
            kill_deadline = now + 30
            try:
                os.kill(pid, 15)
            except ProcessLookupError:
                pass
        elif timed_out and kill_deadline is not None and now >= kill_deadline:
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass
            done, status, usage = os.wait4(pid, 0)
            if os.WIFEXITED(status):
                returncode = os.WEXITSTATUS(status)
            elif os.WIFSIGNALED(status):
                returncode = -os.WTERMSIG(status)
            else:
                returncode = 1
            return returncode, max_rss_kib(usage.ru_maxrss), timed_out
        time.sleep(0.1)


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
    parser.add_argument(
        "--fki-carve-mb", type=int, default=8156,
        help="reserved FKI RAM in MiB; must exceed XNU RAM to leave room for the XNU image and boot data",
    )
    parser.add_argument(
        "--xnu-ram-mb", type=int, default=7900,
        help="XNU's DRAM size in MiB; reserve additional FKI carve-out space for the XNU image and boot data",
    )
    parser.add_argument("--max-rss-mib", type=int)
    parser.add_argument(
        "--require-pie",
        action="store_true",
        help="require the guest to report TCR2_EL1.PIE=1 and pass XNU page-table probes before userland",
    )
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
    try:
        # QEMU interprets a bare -m value as MiB, unlike Linux's bare mem= value (bytes).
        qemu_memory_bytes = parse_memory_size(args.qemu_memory, "--qemu-memory", 1024**2)
        linux_memory_bytes = parse_memory_size(args.linux_memory, "--linux-memory")
    except ValueError as error:
        parser.error(str(error))
    linux_memory_end = QEMU_VIRT_RAM_BASE + linux_memory_bytes
    qemu_memory_end = QEMU_VIRT_RAM_BASE + qemu_memory_bytes
    carve_end = carve_base + args.fki_carve_mb * 1024**2
    if carve_base < linux_memory_end:
        parser.error("FKI carve-out must start after the memory given to Linux")
    if carve_end > qemu_memory_end:
        parser.error("FKI carve-out must end inside the QEMU guest RAM")
    if args.xnu_ram_mb >= args.fki_carve_mb:
        parser.error("XNU RAM must be smaller than the FKI carve-out to leave room for the XNU image and boot data")
    carve_base_hex = hex(carve_base)
    required = (
        f"fki: {args.fki_carve_mb} MiB at {carve_base_hex} for the foreign kernel's DRAM",
        "xnu: contiguous DRAM at phys 0x",
        "xnu: bootstrap window write probe passed",
        "xnu: bootstrap window execute probe passed",
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
    try:
        returncode, peak_rss_kib, timed_out = run_with_timeout(command, args.timeout_seconds)
    except OSError as error:
        print(f"TCG boot could not start: {error}", file=sys.stderr)
        return 1

    if peak_rss_kib is None:
        print("QEMU peak RSS: not measured on this platform", flush=True)
    else:
        print(f"QEMU peak RSS: {peak_rss_kib} KiB ({peak_rss_kib / 1048576:.2f} GiB)", flush=True)
    if timed_out:
        print(f"TCG boot timed out after {args.timeout_seconds} seconds", file=sys.stderr)
        return 1

    log = args.serial_log.read_text(encoding="utf-8", errors="replace") if args.serial_log.exists() else ""
    kmsg_prefix = re.compile(r"^\d+,\d+,\d+,[^;]*;(.*)$")
    messages = []
    for line in log.splitlines():
        match = kmsg_prefix.match(line)
        messages.append(match.group(1) if match else line)
    missing = [marker for marker in required if not any(marker in line for line in messages)]
    tcr2_logged = any("xnu: host TCR2_EL1 " in line for line in messages)
    pie_values = []
    xnu_pie_values = []
    for line in messages:
        match = re.search(r"xnu: host TCR2_EL1\.PIE (0x[0-9a-fA-F]+)(?:\s|$)", line)
        if match:
            pie_values.append(int(match.group(1), 16))
        match = re.search(r"xnu: XNU PIE mapping mode (0x[0-9a-fA-F]+)(?:\s|$)", line)
        if match:
            xnu_pie_values.append(int(match.group(1), 16))
    pie_logged = bool(pie_values)
    tcr2_unsupported = any(
        "xnu: host TCR2_EL1 unavailable; using direct permissions" in line for line in messages
    )
    if args.require_pie:
        if not (tcr2_logged and len(pie_values) == 1 and pie_values[0] == 1):
            missing.append("requires TCR2_EL1.PIE=1 (direct-permission fallback is not accepted)")
        if len(xnu_pie_values) != 1 or xnu_pie_values[0] != 1:
            missing.append("requires XNU PIE mapping mode=1")
        for marker in (
            "xnu: probe port_crumb (rw)",
            "xnu: probe exec strlen through XNU tables",
            "xnu: probes done",
        ):
            if not any(marker in line for line in messages):
                missing.append(marker)
    elif not (tcr2_unsupported or (tcr2_logged and pie_logged)):
        missing.append("TCR2_EL1 read and PIE-bit result (or unsupported-feature fallback)")
    darwin_line = "fki: darwin console: Darwin"
    if darwin_line not in messages:
        missing.append("exact Darwin uname output line")
    base_digits = carve_base_hex[2:]
    if not any(re.search(rf"xnu: contiguous DRAM at phys 0x0*{re.escape(base_digits)}\b", line) for line in messages):
        missing.append(f"XNU contiguous DRAM at physical {carve_base_hex}")
    if returncode != 0 or missing:
        print(f"QEMU exit status: {returncode}", file=sys.stderr)
        if missing:
            print("Missing boot evidence: " + "; ".join(missing), file=sys.stderr)
        print("--- serial log tail ---", file=sys.stderr)
        print("\n".join(log.splitlines()[-160:]), file=sys.stderr)
        return 1

    printed_markers = list(required)
    if args.require_pie:
        printed_markers.extend((
            "xnu: host TCR2_EL1",
            "xnu: XNU PIE mapping mode",
            "xnu: probe port_crumb (rw)",
            "xnu: probe exec strlen through XNU tables",
            "xnu: probes done",
        ))
    for line in messages:
        if line == darwin_line or any(marker in line for marker in printed_markers):
            print(line)
    if args.max_rss_mib is not None:
        if peak_rss_kib is None:
            print("QEMU RSS ceiling not evaluated: peak RSS was not measured", file=sys.stderr)
        elif peak_rss_kib > args.max_rss_mib * 1024:
            print(f"QEMU peak RSS exceeded {args.max_rss_mib} MiB", file=sys.stderr)
            return 1
    print("PureDarwin ARM64 console boot and guest-requested halt passed under QEMU TCG.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
