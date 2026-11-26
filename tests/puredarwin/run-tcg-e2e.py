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
PROFILE = (
    "ARM64 Linux, 4 KiB pages, PAC at EL1, 16 GiB QEMU RAM; mem=7G; "
    "FKI carve-out 8156 MiB at 0x200000000; XNU RAM 7900 MiB"
)


def command_output(command: list[str]) -> str:
    try:
        result = subprocess.run(command, check=False, text=True, capture_output=True)
    except OSError:
        return ""
    return (result.stdout or "").strip()


def host_facts() -> dict:
    info = platform.uname()
    system = info.system
    processor = platform.processor()
    memory_bytes = None

    if system == "Linux":
        try:
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemTotal:"):
                    memory_bytes = int(line.split()[1]) * 1024
                    break
            for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines():
                if line.lower().startswith(("model name", "hardware")):
                    processor = line.split(":", 1)[-1].strip()
                    if processor:
                        break
        except OSError:
            pass
    elif system == "Darwin":
        raw_memory = command_output(["sysctl", "-n", "hw.memsize"])
        if raw_memory.isdigit():
            memory_bytes = int(raw_memory)
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
            memory_bytes = status.ullTotalPhys

    return {
        "system": system,
        "release": info.release,
        "version": info.version,
        "architecture": info.machine,
        "processor": processor or "unknown",
        "logical_cpus": os.cpu_count(),
        "memory_total_bytes": memory_bytes,
    }


def qemu_path(path: Path) -> str:
    return path.resolve().as_posix()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save_report(report: dict, serial_log: Path) -> None:
    temp_dir = Path(os.environ.get("RUNNER_TEMP", str(serial_log.parent)))
    report_path = temp_dir / "puredarwin-tcg-boot-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return

    host = report["host"]
    memory = host["memory_total_bytes"]
    memory_text = f"{memory / (1024 ** 3):.1f} GiB" if memory else "unavailable"
    result = report["result"]
    lines = [
        "## x86-64 host to PureDarwin ARM64 console",
        "",
        f"- Runner: {report['runner_label']}",
        f"- Host OS: {host['system']} {host['release']} ({host['architecture']})",
        f"- Host CPU: {host['processor']}",
        f"- Host resources: {host['logical_cpus']} logical CPUs / {memory_text}",
        f"- QEMU: {report['qemu_version']}",
        f"- Profile: {report['profile']}",
        f"- Result: {'passed' if result['passed'] else 'failed'}",
        "- Same downloaded guest inputs (SHA-256):",
        f"  - ARM64 Linux kernel: {report['inputs']['kernel_sha256']}",
        f"  - initramfs: {report['inputs']['initramfs_sha256']}",
        f"  - PureDarwin disk: {report['inputs']['disk_sha256']}",
    ]
    if result.get("missing_markers"):
        lines.append("- Missing evidence: " + "; ".join(result["missing_markers"]))
    with Path(summary_path).open("a", encoding="utf-8") as summary:
        summary.write("\n".join(lines) + "\n")


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

    host = host_facts()
    if host["architecture"].lower() not in ("x86_64", "amd64"):
        parser.error(f"x86-64 host required for this TCG acceptance; got {host['architecture']}")

    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    version_result = subprocess.run([qemu, "--version"], check=False, text=True, capture_output=True)
    version_text = version_result.stdout or version_result.stderr
    qemu_version = version_text.splitlines()[0] if version_text else "unknown"
    input_hashes = {
        "kernel_sha256": sha256_file(args.kernel),
        "initramfs_sha256": sha256_file(args.initramfs),
        "disk_sha256": sha256_file(args.disk),
    }
    report = {
        "runner_label": os.environ.get("XNULINUX_RUNNER_LABEL", "local"),
        "host": host,
        "qemu_version": qemu_version,
        "profile": PROFILE,
        "inputs": input_hashes,
        "result": {"passed": False, "returncode": None, "missing_markers": list(REQUIRED)},
    }

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
        "-serial", f"file:{qemu_path(args.serial_log)}",
        "-monitor", "none",
        "-no-reboot",
        "-object", "rng-builtin,id=rng0",
        "-device", "virtio-rng-device,rng=rng0",
        "-drive", f"if=none,id=puredarwin,format=raw,file={qemu_path(args.disk)}",
        "-device", "virtio-blk-device,drive=puredarwin",
        "-kernel", qemu_path(args.kernel),
        "-initrd", qemu_path(args.initramfs),
        "-append", "console=ttyAMA0 rdinit=/init nokaslr mem=7G loglevel=1",
    ]
    print(f"Host: {host['system']} {host['release']} ({host['architecture']})")
    print(f"Host CPU: {host['processor']}")
    print(f"Host logical CPUs / RAM: {host['logical_cpus']} / {host['memory_total_bytes']}")
    print(f"QEMU: {qemu_version}")
    print(f"Profile: {PROFILE}")
    print(f"Runner: {report['runner_label']}")
    for key, value in input_hashes.items():
        print(f"{key}: {value}")

    try:
        result = subprocess.run(command, check=False, timeout=args.timeout_seconds)
    except (OSError, subprocess.TimeoutExpired) as error:
        report["result"] = {
            "passed": False,
            "returncode": None,
            "error": str(error),
            "missing_markers": list(REQUIRED),
        }
        print(f"TCG boot could not complete: {error}", file=sys.stderr)
        save_report(report, args.serial_log)
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

    report["result"] = {
        "passed": result.returncode == 0 and not missing,
        "returncode": result.returncode,
        "missing_markers": missing,
    }
    save_report(report, args.serial_log)
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
