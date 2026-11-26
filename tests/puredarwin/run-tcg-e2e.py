#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot the real PureDarwin guest under ARM64 QEMU TCG and require a clean halt."""

import argparse
import ctypes
import hashlib
import json
import os
import platform
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


def command_output(command: list[str]) -> str:
    try:
        result = subprocess.run(command, check=False, text=True, capture_output=True)
    except OSError:
        return ""
    return (result.stdout or "").strip()


def host_facts() -> dict:
    info = platform.uname()
    system = info.system
    processor = platform.processor() or info.processor
    memory_total_bytes = None
    memory_available_bytes = None

    if system == "Linux":
        try:
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemTotal:"):
                    memory_total_bytes = int(line.split()[1]) * 1024
                elif line.startswith("MemAvailable:"):
                    memory_available_bytes = int(line.split()[1]) * 1024
            for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines():
                if line.lower().startswith(("model name", "hardware")):
                    processor = line.split(":", 1)[-1].strip() or processor
                    break
        except OSError:
            pass
    elif system == "Darwin":
        raw_memory = command_output(["sysctl", "-n", "hw.memsize"])
        if raw_memory.isdigit():
            memory_total_bytes = int(raw_memory)
        vm_stat = command_output(["vm_stat"])
        page_size_match = re.search(r"page size of (\d+) bytes", vm_stat)
        if page_size_match:
            page_size = int(page_size_match.group(1))
            pages = 0
            for label in ("Pages free", "Pages inactive", "Pages speculative", "Pages purgeable"):
                match = re.search(rf"^{re.escape(label)}:\s*([\d.]+)", vm_stat, re.MULTILINE)
                if match:
                    pages += int(float(match.group(1)))
            memory_available_bytes = pages * page_size
        processor = command_output(["sysctl", "-n", "machdep.cpu.brand_string"]) or processor
    elif system == "Windows":
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong),
                ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]

        status = MEMORYSTATUSEX()
        status.dwLength = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            memory_total_bytes = status.ullTotalPhys
            memory_available_bytes = status.ullAvailPhys

    return {
        "system": system,
        "release": info.release,
        "architecture": info.machine,
        "processor": processor or "unknown",
        "logical_cpus": os.cpu_count(),
        "memory_total_bytes": memory_total_bytes,
        "memory_available_bytes": memory_available_bytes,
    }


def qemu_path(path: Path) -> str:
    """Use forward slashes in QEMU option values, including Windows drive paths."""
    return path.resolve().as_posix()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_manifest(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
    return values


def verify_input_hashes(kernel: Path, initramfs: Path, disk: Path) -> dict[str, str]:
    """Require guest inputs to match the build manifests from the same workflow run."""
    hashes = {
        "arm64_guest_image_sha256": sha256_file(kernel),
        "arm64_guest_initramfs_sha256": sha256_file(initramfs),
        "puredarwin_disk_sha256": sha256_file(disk),
    }
    manifest_dir = kernel.parent
    guest_manifest = read_manifest(manifest_dir / "guest-manifest.txt")
    build_manifest = read_manifest(manifest_dir / "build-manifest.txt")
    expected = {
        "arm64_guest_image_sha256": guest_manifest.get("arm64_guest_image_sha256"),
        "arm64_guest_initramfs_sha256": guest_manifest.get("arm64_guest_initramfs_sha256"),
        "puredarwin_disk_sha256": build_manifest.get("puredarwin_disk_sha256"),
    }
    for key, digest in hashes.items():
        if not expected[key]:
            raise ValueError(f"input manifest is missing {key}")
        if digest.lower() != expected[key].lower():
            raise ValueError(f"downloaded input hash does not match its manifest: {key}")
    return hashes


def save_boot_report(
    *,
    args,
    host: dict,
    qemu_version: str,
    input_hashes: dict[str, str],
    returncode: Optional[int],
    peak_rss_kib: Optional[int],
    timed_out: bool,
    missing: list[str],
    error: Optional[str] = None,
    serial_tail: str = "",
) -> None:
    passed = returncode == 0 and not timed_out and not missing and error is None
    report = {
        "runner_label": os.environ.get("XNULINUX_RUNNER_LABEL", "local"),
        "host": host,
        "qemu_version": qemu_version,
        "profile": {
            "accelerator": "TCG",
            "qemu_memory": args.qemu_memory,
            "linux_memory": args.linux_memory,
            "fki_carve_base": hex(int(args.fki_carve_base, 0)),
            "fki_carve_mb": args.fki_carve_mb,
            "xnu_ram_mb": args.xnu_ram_mb,
            "rss_limit_mib": args.max_rss_mib,
        },
        "inputs": input_hashes,
        "result": {
            "passed": passed,
            "qemu_exit_status": returncode,
            "timed_out": timed_out,
            "qemu_peak_rss_kib": peak_rss_kib,
            "rss_limit_evaluated": peak_rss_kib is not None,
            "missing_evidence": missing,
            "error": error,
            "serial_log_tail": serial_tail,
        },
    }
    report_path = Path(os.environ.get("RUNNER_TEMP", str(args.serial_log.parent))) / "puredarwin-tcg-boot-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        total = host["memory_total_bytes"]
        available = host["memory_available_bytes"]
        rss_text = f"{peak_rss_kib} KiB" if peak_rss_kib is not None else "not measured on this platform"
        lines = [
            "## x86-64 host to PureDarwin ARM64 console boot",
            "",
            f"- Runner: {report['runner_label']}",
            f"- Host: {host['system']} {host['release']} ({host['architecture']})",
            f"- CPU: {host['processor']} ({host['logical_cpus']} logical CPUs)",
            f"- RAM before QEMU: {total} total bytes; {available} available bytes",
            f"- QEMU: {qemu_version}",
            f"- Profile: {args.qemu_memory} guest RAM; Linux mem={args.linux_memory}; FKI {args.fki_carve_mb} MiB at {args.fki_carve_base}; XNU {args.xnu_ram_mb} MiB",
            f"- Result: {'passed' if passed else 'failed'}; QEMU exit={returncode}; timeout={timed_out}; peak RSS={rss_text}",
            f"- RSS limit: {args.max_rss_mib} MiB; evaluated={peak_rss_kib is not None}",
            "- Input SHA-256:",
        ]
        lines.extend(f"  - {key}: {value}" for key, value in input_hashes.items())
        if missing:
            lines.append("- Missing evidence: " + "; ".join(missing))
        if error:
            lines.append(f"- Error: {error}")
        if serial_tail:
            lines.extend(["- Serial log tail:", "```text", serial_tail, "```"])
        with Path(summary_path).open("a", encoding="utf-8") as summary:
            summary.write("\n".join(lines) + "\n")


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
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=(
            "All inputs must come from one workflow run. Place guest-manifest.txt and "
            "build-manifest.txt beside the kernel Image; they are required to verify the "
            "kernel, initramfs, and disk hashes before QEMU starts."
        ),
    )
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
    args = parser.parse_args()
    for path in (args.kernel, args.initramfs, args.disk):
        if not path.is_file():
            parser.error(f"missing input: {path}")
    qemu = shutil.which(args.qemu)
    if not qemu and os.name == "nt" and not args.qemu.lower().endswith(".exe"):
        qemu = shutil.which(args.qemu + ".exe")
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
        "fki: darwin console: XNULINUX_USERLAND_OK",
        "xnu: the hosted system halted",
        "XNULINUX_E2E_PUREDARWIN_USERLAND_OK",
        "XNULINUX_E2E_GUEST_HALT_OK",
        "XNULINUX_E2E_XNU_UNLOADED",
        "reboot: Power down",
    )
    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    host = host_facts()
    if host["architecture"].lower() not in ("x86_64", "amd64"):
        print(f"x86-64 host required; got {host['architecture']}", file=sys.stderr)
        return 1
    print(f"Host: {host['system']} {host['release']} ({host['architecture']})")
    print(f"Host CPU: {host['processor']}; logical CPUs: {host['logical_cpus']}")
    print(f"Host total RAM before QEMU: {host['memory_total_bytes']} bytes")
    print(f"Host available RAM before QEMU: {host['memory_available_bytes']} bytes")
    version = subprocess.run([qemu, "--version"], check=False, text=True, capture_output=True)
    version_text = version.stdout or version.stderr
    qemu_version = version_text.splitlines()[0] if version_text else "unknown"
    print(f"QEMU: {qemu_version}")

    try:
        input_hashes = verify_input_hashes(args.kernel, args.initramfs, args.disk)
    except (OSError, ValueError) as error:
        print(f"TCG boot input validation failed: {error}", file=sys.stderr)
        save_boot_report(
            args=args, host=host, qemu_version=qemu_version, input_hashes={},
            returncode=None, peak_rss_kib=None, timed_out=False,
            missing=["input manifest validation"], error=str(error),
        )
        return 1

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
        "-serial", f"file:{qemu_path(args.serial_log)}",
        "-monitor", "none",
        "-no-reboot",
        "-object", "rng-builtin,id=rng0",
        "-device", "virtio-rng-device,rng=rng0",
        "-drive", f"if=none,id=puredarwin,format=raw,file={qemu_path(args.disk)}",
        "-device", "virtio-blk-device,drive=puredarwin",
        "-kernel", qemu_path(args.kernel),
        "-initrd", qemu_path(args.initramfs),
        "-append", (
            "console=ttyAMA0 rdinit=/init nokaslr "
            f"mem={args.linux_memory} "
            f"xnulinux.carve_base={carve_base_hex} "
            f"xnulinux.carve_mb={args.fki_carve_mb} "
            f"xnulinux.xnu_ram_mb={args.xnu_ram_mb} "
            "loglevel=1"
        ),
    ]
    for name, digest in input_hashes.items():
        print(f"Input {name}: {digest}")
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
        log = args.serial_log.read_text(encoding="utf-8", errors="replace") if args.serial_log.exists() else ""
        save_boot_report(
            args=args, host=host, qemu_version=qemu_version, input_hashes=input_hashes,
            returncode=None, peak_rss_kib=None, timed_out=False, missing=list(required),
            error=str(error), serial_tail="\n".join(log.splitlines()[-160:]),
        )
        return 1

    if peak_rss_kib is None:
        print("QEMU peak RSS: not measured on this platform", flush=True)
    else:
        print(f"QEMU peak RSS: {peak_rss_kib} KiB ({peak_rss_kib / 1048576:.2f} GiB)", flush=True)

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
    if timed_out:
        missing.append(f"QEMU timeout after {args.timeout_seconds} seconds")
    if returncode != 0:
        missing.append(f"QEMU exit status {returncode}")
    if args.max_rss_mib is not None and peak_rss_kib is not None and peak_rss_kib > args.max_rss_mib * 1024:
        missing.append(f"QEMU peak RSS exceeded {args.max_rss_mib} MiB")
    serial_tail = "\n".join(log.splitlines()[-160:])
    save_boot_report(
        args=args, host=host, qemu_version=qemu_version, input_hashes=input_hashes,
        returncode=returncode, peak_rss_kib=peak_rss_kib, timed_out=timed_out,
        missing=missing, serial_tail=serial_tail,
    )
    if returncode != 0 or timed_out or missing:
        print(f"QEMU exit status: {returncode}", file=sys.stderr)
        if missing:
            print("Missing boot evidence: " + "; ".join(missing), file=sys.stderr)
        print("--- serial log tail ---", file=sys.stderr)
        print(serial_tail, file=sys.stderr)
        return 1

    for line in messages:
        if line == darwin_line or any(marker in line for marker in required):
            print(line)
    if args.max_rss_mib is not None and peak_rss_kib is None:
        print("QEMU RSS ceiling not evaluated: peak RSS was not measured", file=sys.stderr)
    print("PureDarwin ARM64 console boot and guest-requested halt passed under QEMU TCG.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
