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
GUEST_RAM_BYTES = 16 * 1024 ** 3
HOST_HEADROOM_BYTES = 4 * 1024 ** 3
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
GUEST_INPUT_HASHES = {
    "arm64_guest_image_sha256": ("kernel_sha256", "ARM64 guest kernel"),
    "arm64_guest_initramfs_sha256": ("initramfs_sha256", "ARM64 guest initramfs"),
}
MANIFEST_READ_ERROR_PREFIX = "[manifest read error: "


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
    memory_available_bytes = None

    if system == "Linux":
        try:
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemTotal:"):
                    memory_bytes = int(line.split()[1]) * 1024
                elif line.startswith("MemAvailable:"):
                    memory_available_bytes = int(line.split()[1]) * 1024
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
        vm_stat = command_output(["vm_stat"])
        page_size_match = re.search(r"page size of (\d+) bytes", vm_stat)
        if page_size_match:
            page_size = int(page_size_match.group(1))
            available_pages = 0
            for label in ("Pages free", "Pages inactive", "Pages speculative", "Pages purgeable"):
                match = re.search(rf"^{re.escape(label)}:\s*([\d.]+)", vm_stat, re.MULTILINE)
                if match:
                    available_pages += int(float(match.group(1)))
            memory_available_bytes = available_pages * page_size
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
            memory_available_bytes = status.ullAvailPhys

    return {
        "system": system,
        "release": info.release,
        "version": info.version,
        "architecture": info.machine,
        "processor": processor or "unknown",
        "logical_cpus": os.cpu_count(),
        "memory_total_bytes": memory_bytes,
        "memory_available_bytes": memory_available_bytes,
    }


def available_memory_bytes() -> int | None:
    return host_facts()["memory_available_bytes"]


def ram_capacity(host: dict, measurement_phase: str) -> dict:
    reasons = []
    total = host["memory_total_bytes"]
    available = host["memory_available_bytes"]
    if total is None or total < GUEST_RAM_BYTES + HOST_HEADROOM_BYTES:
        reasons.append("host physical RAM is below 20 GiB (16 GiB guest plus 4 GiB host headroom)")
    if available is None or available < HOST_HEADROOM_BYTES:
        reasons.append("available RAM before QEMU is below the 4 GiB host-headroom allowance")
    return {
        "measurement_phase": measurement_phase,
        "minimum_total_ram_bytes": GUEST_RAM_BYTES + HOST_HEADROOM_BYTES,
        "minimum_available_before_qemu_bytes": HOST_HEADROOM_BYTES,
        "capacity_screen_passed": not reasons,
        "reasons": reasons,
    }


def qemu_path(path: Path) -> str:
    return path.resolve().as_posix()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_if_readable_file(path: Path) -> str | None:
    try:
        return sha256_file(path) if path.is_file() else None
    except OSError:
        return None


def read_manifest(path: Path) -> tuple[str | None, dict[str, str]]:
    try:
        raw = path.read_bytes().decode("utf-8")
    except OSError:
        return None, {}
    except UnicodeDecodeError as error:
        return f"{MANIFEST_READ_ERROR_PREFIX}{path.name} is not valid UTF-8: {error}]", {}
    values = {}
    for line in raw.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
    return raw, values


def serial_evidence(path: Path) -> tuple[str, list[str], list[str]]:
    try:
        log = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        log = ""
    kmsg_prefix = re.compile(r"^\d+,\d+,\d+,[^;]*;(.*)$")
    messages = []
    for line in log.splitlines():
        match = kmsg_prefix.match(line)
        messages.append(match.group(1) if match else line)
    missing = [marker for marker in REQUIRED if not any(marker in line for line in messages)]
    if "fki: darwin console: Darwin" not in messages:
        missing.append("exact Darwin uname output line")
    if not any(re.search(r"xnu: contiguous DRAM at phys 0x0*200000000\b", line) for line in messages):
        missing.append("XNU contiguous DRAM at physical 0x200000000")
    return log, messages, missing


def output_tail(value) -> str:
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    return "\n".join((value or "").splitlines()[-80:])


def save_report(
    report: dict,
    serial_log: Path,
    *,
    memory_phase: str,
    write_summary: bool = True,
) -> None:
    report["host"]["memory_available_at_report_bytes"] = available_memory_bytes()
    report["host"]["memory_available_at_report_phase"] = memory_phase
    temp_dir = Path(os.environ.get("RUNNER_TEMP", str(serial_log.parent)))
    report_path = temp_dir / "puredarwin-tcg-boot-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path or not write_summary:
        return

    host = report["host"]
    memory = host["memory_total_bytes"]
    memory_text = f"{memory / (1024 ** 3):.1f} GiB" if memory else "unavailable"
    available = host["memory_available_bytes"]
    available_text = f"{available / (1024 ** 3):.1f} GiB" if available is not None else "unavailable"
    available_at_report = host["memory_available_at_report_bytes"]
    available_at_report_text = (
        f"{available_at_report / (1024 ** 3):.1f} GiB" if available_at_report is not None else "unavailable"
    )
    result = report["result"]
    lines = [
        "## x86-64 host to PureDarwin ARM64 console",
        "",
        f"- Runner: {report['runner_label']}",
        f"- Host OS: {host['system']} {host['release']} ({host['architecture']})",
        f"- Host CPU: {host['processor']}",
        f"- Host resources: {host['logical_cpus']} logical CPUs / {memory_text} total RAM",
        f"- Available RAM at capacity screen ({report['ram_capacity']['measurement_phase']}): {available_text}",
        f"- Available RAM when report was written ({host['memory_available_at_report_phase']}): {available_at_report_text}",
        f"- RAM capacity screen: {'passed' if report['ram_capacity']['capacity_screen_passed'] else 'capacity-limited'}",
        f"- QEMU: {report['qemu_version']}",
        f"- Profile: {report['profile']}",
        f"- Result: {'passed' if result['passed'] else 'failed'}",
        "- Same downloaded guest inputs (SHA-256):",
        f"  - ARM64 Linux kernel: {report['inputs']['kernel_sha256'] or 'unavailable'}",
        f"  - initramfs: {report['inputs']['initramfs_sha256'] or 'unavailable'}",
        f"  - PureDarwin disk: {report['inputs']['disk_sha256'] or 'unavailable'}",
    ]
    if report.get("preparation_steps"):
        lines.append("- Preparation step outcomes: " + ", ".join(
            f"{name}={outcome}" for name, outcome in report["preparation_steps"].items()
        ))
    if report["ram_capacity"]["reasons"]:
        lines.append("- RAM capacity limits: " + "; ".join(report["ram_capacity"]["reasons"]))
    for name, manifest in report["manifests"].items():
        lines.extend([
            f"- {name} manifest SHA-256: {manifest['sha256'] or 'unavailable'}",
            "```text",
            manifest["contents"] or "(manifest unavailable)",
            "```",
        ])
    if result.get("missing_markers"):
        lines.append("- Missing evidence: " + "; ".join(result["missing_markers"]))
    if result.get("errors"):
        lines.append("- Errors: " + "; ".join(result["errors"]))
    if result.get("error"):
        lines.append("- Error: " + result["error"])
    if result.get("qemu_stderr_tail"):
        lines.extend(["- QEMU stderr:", "```text", result["qemu_stderr_tail"], "```"])
    with Path(summary_path).open("a", encoding="utf-8") as summary:
        summary.write("\n".join(lines) + "\n")


def compare_manifest_digest(
    errors: list[str],
    description: str,
    expected: str | None,
    actual: str | None,
) -> None:
    if not expected:
        errors.append(f"manifest is missing {description} SHA-256")
    elif not SHA256_PATTERN.fullmatch(expected):
        errors.append(f"manifest {description} is not a valid SHA-256 digest")
    elif actual is None:
        errors.append(f"cannot verify {description} SHA-256: input file is missing or unreadable")
    elif not SHA256_PATTERN.fullmatch(actual):
        errors.append(f"computed {description} is not a valid SHA-256 digest")
    elif expected.lower() != actual.lower():
        errors.append(f"{description} SHA-256 does not match its manifest")


def manifest_input_errors(
    input_hashes: dict[str, str | None],
    missing_inputs: list[Path],
    build_manifest_text: str | None,
    build_manifest: dict[str, str],
    guest_manifest_text: str | None,
    guest_manifest: dict[str, str],
) -> list[str]:
    errors = [f"missing input file: {path}" for path in missing_inputs]

    if not build_manifest_text:
        errors.append("build-manifest.txt is missing")
    elif build_manifest_text.startswith(MANIFEST_READ_ERROR_PREFIX):
        errors.append(build_manifest_text[1:-1])
    else:
        compare_manifest_digest(
            errors,
            "PureDarwin disk",
            build_manifest.get("puredarwin_disk_sha256"),
            input_hashes.get("disk_sha256"),
        )

    if not guest_manifest_text:
        errors.append("guest-manifest.txt is missing")
    elif guest_manifest_text.startswith(MANIFEST_READ_ERROR_PREFIX):
        errors.append(guest_manifest_text[1:-1])
    else:
        for key in (
            "arm64_guest_kernel_release",
            "arm64_guest_page_size",
            "arm64_guest_pac",
            "arm64_guest_exception_level",
        ):
            if key not in guest_manifest:
                errors.append(f"guest manifest is missing {key}")
        for manifest_key, (input_key, description) in GUEST_INPUT_HASHES.items():
            compare_manifest_digest(
                errors,
                description,
                guest_manifest.get(manifest_key),
                input_hashes.get(input_key),
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("serial_log", type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=14400)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    missing_inputs = []
    for path in (args.kernel, args.initramfs, args.disk):
        if not path.is_file():
            missing_inputs.append(path)
    if args.timeout_seconds < 1:
        parser.error("--timeout-seconds must be positive")

    host = host_facts()
    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    input_hashes = {
        "kernel_sha256": sha256_if_readable_file(args.kernel),
        "initramfs_sha256": sha256_if_readable_file(args.initramfs),
        "disk_sha256": sha256_if_readable_file(args.disk),
    }
    build_manifest_path = args.kernel.parent / "build-manifest.txt"
    guest_manifest_path = args.kernel.parent / "guest-manifest.txt"
    build_manifest_text, build_manifest = read_manifest(build_manifest_path)
    guest_manifest_text, guest_manifest = read_manifest(guest_manifest_path)
    report = {
        "runner_label": os.environ.get("XNULINUX_RUNNER_LABEL", "local"),
        "host": host,
        "ram_capacity": ram_capacity(host, "python_runner_start_before_input_hashing"),
        "qemu_version": "unavailable",
        "profile": PROFILE,
        "inputs": input_hashes,
        "preparation_steps": {
            "host_preflight_posix": os.environ.get("XNULINUX_HOST_PREFLIGHT_POSIX_OUTCOME", "not-applicable"),
            "host_preflight_windows": os.environ.get("XNULINUX_HOST_PREFLIGHT_WINDOWS_OUTCOME", "not-applicable"),
            "guest_artifact_download": os.environ.get("XNULINUX_GUEST_ARTIFACT_OUTCOME", "not-recorded"),
            "disk_artifact_download": os.environ.get("XNULINUX_DISK_ARTIFACT_OUTCOME", "not-recorded"),
            "qemu_install_linux": os.environ.get("XNULINUX_QEMU_LINUX_OUTCOME", "not-applicable"),
            "qemu_install_windows": os.environ.get("XNULINUX_QEMU_WINDOWS_OUTCOME", "not-applicable"),
            "qemu_install_macos": os.environ.get("XNULINUX_QEMU_MACOS_OUTCOME", "not-applicable"),
            "python_setup": os.environ.get("XNULINUX_PYTHON_SETUP_OUTCOME", "not-recorded"),
        },
        "manifests": {
            "build": {
                "path": build_manifest_path.name,
                "sha256": hashlib.sha256(build_manifest_text.encode("utf-8")).hexdigest() if build_manifest_text is not None else None,
                "contents": build_manifest_text,
            },
            "guest": {
                "path": guest_manifest_path.name,
                "sha256": hashlib.sha256(guest_manifest_text.encode("utf-8")).hexdigest() if guest_manifest_text is not None else None,
                "contents": guest_manifest_text,
            },
        },
        "result": {"passed": False, "returncode": None, "missing_markers": list(REQUIRED)},
    }

    qemu = shutil.which(args.qemu)
    if not qemu and os.name == "nt" and not args.qemu.lower().endswith(".exe"):
        qemu = shutil.which(args.qemu + ".exe")
    if qemu:
        try:
            version_result = subprocess.run([qemu, "--version"], check=False, text=True, capture_output=True)
            version_text = version_result.stdout or version_result.stderr
            report["qemu_version"] = version_text.splitlines()[0] if version_text else "unknown"
        except OSError as error:
            report["qemu_version"] = f"unavailable ({error})"

    manifest_errors = manifest_input_errors(
        input_hashes,
        missing_inputs,
        build_manifest_text,
        build_manifest,
        guest_manifest_text,
        guest_manifest,
    )

    host_errors = []
    if host["architecture"].lower() not in ("x86_64", "amd64"):
        host_errors.append(f"x86-64 host required; got {host['architecture']}")
    if not qemu:
        host_errors.append(f"QEMU executable not found: {args.qemu}")
    if manifest_errors or host_errors:
        for name, outcome in report["preparation_steps"].items():
            if outcome not in ("success", "skipped", "not-recorded", "not-applicable"):
                host_errors.append(f"preparation step {name} outcome: {outcome}")
        report["result"] = {
            "passed": False,
            "returncode": None,
            "missing_markers": [],
            "errors": manifest_errors + host_errors,
        }
        print("TCG boot could not start: " + "; ".join(manifest_errors + host_errors), file=sys.stderr)
        save_report(report, args.serial_log, memory_phase="validation_failed_before_qemu")
        return 1

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
    print(f"QEMU: {report['qemu_version']}")
    print(f"Profile: {PROFILE}")
    print(f"Runner: {report['runner_label']}")
    for key, value in input_hashes.items():
        print(f"{key}: {value}")

    try:
        result = subprocess.run(command, check=False, timeout=args.timeout_seconds, text=True, capture_output=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        log, _, missing = serial_evidence(args.serial_log)
        report["result"] = {
            "passed": False,
            "returncode": None,
            "error": str(error),
            "missing_markers": missing,
            "serial_log_tail": "\n".join(log.splitlines()[-160:]),
            "qemu_stdout_tail": output_tail(getattr(error, "stdout", None)),
            "qemu_stderr_tail": output_tail(getattr(error, "stderr", None)),
        }
        print(f"TCG boot could not complete: {error}", file=sys.stderr)
        if report["result"]["qemu_stderr_tail"]:
            print(report["result"]["qemu_stderr_tail"], file=sys.stderr)
        save_report(report, args.serial_log, memory_phase="qemu_start_failed_or_timed_out")
        return 1

    log, messages, missing = serial_evidence(args.serial_log)

    report["result"] = {
        "passed": result.returncode == 0 and not missing,
        "returncode": result.returncode,
        "missing_markers": missing,
        "serial_log_tail": "\n".join(log.splitlines()[-160:]),
    }
    if result.returncode != 0:
        report["result"]["qemu_stdout_tail"] = output_tail(result.stdout)
        report["result"]["qemu_stderr_tail"] = output_tail(result.stderr)
    save_report(report, args.serial_log, memory_phase="after_qemu_exit")
    if result.returncode != 0 or missing:
        print(f"QEMU exit status: {result.returncode}", file=sys.stderr)
        if missing:
            print("Missing boot evidence: " + "; ".join(missing), file=sys.stderr)
        if report["result"].get("qemu_stderr_tail"):
            print(report["result"]["qemu_stderr_tail"], file=sys.stderr)
        print("--- serial log tail ---", file=sys.stderr)
        print("\n".join(log.splitlines()[-160:]), file=sys.stderr)
        return 1

    for line in messages:
        if line == "fki: darwin console: Darwin" or any(marker in line for marker in REQUIRED):
            print(line)
    print("PureDarwin ARM64 console boot and guest-requested halt passed under QEMU TCG.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
