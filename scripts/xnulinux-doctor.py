#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Report host facts relevant to xnulinux without changing the host.

The report is diagnostic input for the complementary QEMU TCG/PureDarwin
route, with ARM64/native-x86 context for the hosted-XNU port. It is not a compatibility
certification or a physical-hardware test.
"""

import argparse
import ctypes
import gzip
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import zlib

GIB = 1024 ** 3
X86_64H_FEATURES = (
    "sse4_2",
    "avx",
    "fma",
    "movbe",
    "popcnt",
    "abm",
    "bmi1",
    "avx2",
    "bmi2",
)


def read_meminfo(path="/proc/meminfo"):
    values = {}
    try:
        with open(path, encoding="utf-8") as meminfo:
            for line in meminfo:
                key, _, rest = line.partition(":")
                fields = rest.strip().split()
                if fields and fields[0].isdigit():
                    values[key] = int(fields[0]) * 1024
    except OSError:
        pass
    return values


def memory_facts(system):
    if system == "Linux":
        values = read_meminfo()
        return values.get("MemTotal"), values.get("MemAvailable")
    if system == "Darwin":
        try:
            total = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True).strip())
        except (OSError, subprocess.SubprocessError, ValueError):
            total = None
        available = None
        try:
            vm_stat = subprocess.check_output(["vm_stat"], text=True)
            page_size = 4096
            pages = {}
            for line in vm_stat.splitlines():
                match = re.search(r"page size of (\d+) bytes", line)
                if match:
                    page_size = int(match.group(1))
                key, _, value = line.partition(":")
                if value.strip().rstrip(".").isdigit():
                    pages[key.strip()] = int(value.strip().rstrip("."))
            free_pages = pages.get("Pages free", 0) + pages.get("Pages inactive", 0)
            if free_pages:
                available = free_pages * page_size
        except (OSError, subprocess.SubprocessError):
            pass
        return total, available
    if system == "Windows":
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
        try:
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status.ullTotalPhys, status.ullAvailPhys
        except (AttributeError, OSError):
            pass
    return None, None


def qemu_facts(executable="qemu-system-aarch64"):
    path = shutil.which(executable)
    facts = {"executable": executable, "path": path, "version": None, "version_parts": None, "error": None}
    if not path:
        return facts
    try:
        output = subprocess.check_output([path, "--version"], text=True, stderr=subprocess.STDOUT, timeout=10)
    except (OSError, subprocess.SubprocessError) as exc:
        facts["error"] = str(exc)
        return facts
    first_line = output.splitlines()[0] if output.splitlines() else output
    match = re.search(r"version\s+(\d+)\.(\d+)\.(\d+)", first_line, re.IGNORECASE)
    if match:
        facts["version_parts"] = tuple(int(part) for part in match.groups())
        facts["version"] = ".".join(match.groups())
    else:
        facts["error"] = "unparsed version: " + first_line
    return facts


def linux_cpu_info(path="/proc/cpuinfo"):
    """Read selected fields from the first CPU entry Linux reports."""
    fields = {}
    try:
        with open(path, encoding="utf-8") as cpuinfo:
            for line in cpuinfo:
                if not line.strip() and fields:
                    break
                name, separator, value = line.partition(":")
                key = name.strip().lower()
                if separator and key in {
                    "vendor_id",
                    "model name",
                    "cpu family",
                    "model",
                    "stepping",
                    "microcode",
                    "flags",
                    "features",
                }:
                    fields[key] = value.strip()
    except OSError:
        return {}
    return fields


def cpu_hypervisor_flag(cpu_info):
    """Return the first reported CPU's hypervisor-flag hint, if available."""
    flags = cpu_info.get("flags", "").split()
    return "hypervisor" in flags if flags else None


def linux_kernel_config_facts(release, proc_config_path="/proc/config.gz", boot_config_dir="/boot"):
    """Hash the Linux config bytes, preferring the running kernel's gzip export.

    The /boot fallback identifies a file associated with the kernel release,
    which can be shared by multiple kernel builds. The digest covers the plain
    .config bytes. If there is no readable, nonblank config, the status is
    "unavailable" and the hash and source are null.
    """
    candidates = [(proc_config_path, True)]
    if release and os.path.basename(release) == release:
        candidates.append((os.path.join(boot_config_dir, f"config-{release}"), False))

    for path, compressed in candidates:
        try:
            if compressed:
                with gzip.open(path, "rb") as config_file:
                    config = config_file.read()
            else:
                with open(path, "rb") as config_file:
                    config = config_file.read()
        except (OSError, EOFError, zlib.error):
            continue
        if not config.strip():
            continue
        return {
            "status": "available",
            "source": path,
            "sha256": hashlib.sha256(config).hexdigest(),
        }

    return {"status": "unavailable", "source": None, "sha256": None}


def collect_facts():
    system = platform.system() or "Unknown"
    machine = platform.machine() or "unknown"
    total, available = memory_facts(system)
    qemu = qemu_facts()
    cpu_info = linux_cpu_info() if system == "Linux" else {}
    flags = set(cpu_info.get("flags", cpu_info.get("features", "")).split())
    linux_page_size = os.sysconf("SC_PAGE_SIZE") if system == "Linux" else None
    kernel_config = (
        linux_kernel_config_facts(platform.release())
        if system == "Linux"
        else {"status": "not_applicable", "source": None, "sha256": None}
    )
    try:
        free_disk = shutil.disk_usage(os.getcwd()).free
    except OSError:
        free_disk = None
    return {
        "system": system,
        "release": platform.release(),
        "machine": machine,
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "memory_total_bytes": total,
        "memory_available_bytes": available,
        "free_disk_bytes": free_disk,
        "qemu": qemu,
        "linux_cpu_flags": sorted(flags.intersection(X86_64H_FEATURES)),
        "linux_cpu_vendor": cpu_info.get("vendor_id"),
        "linux_cpu_model": cpu_info.get("model name"),
        "linux_cpu_family": cpu_info.get("cpu family"),
        "linux_cpu_model_number": cpu_info.get("model"),
        "linux_cpu_stepping": cpu_info.get("stepping"),
        "linux_cpu_microcode": cpu_info.get("microcode"),
        "linux_cpu_hypervisor_flag": cpu_hypervisor_flag(cpu_info),
        "linux_page_size_bytes": linux_page_size,
        "linux_kernel_config_status": kernel_config["status"],
        "linux_kernel_config_source": kernel_config["source"],
        "linux_kernel_config_sha256": kernel_config["sha256"],
    }


def check(name, status, summary, details=None):
    item = {"name": name, "status": status, "summary": summary}
    if details is not None:
        item["details"] = details
    return item


def evaluate(facts):
    checks = []
    system = facts["system"]
    machine = facts["machine"].lower()
    x86 = machine in {"x86_64", "amd64"}
    arm64 = machine in {"aarch64", "arm64"}

    if x86 and system == "Linux":
        checks.append(check(
            "host-profile", "pass",
            "x86-64 Linux is the required host profile for the complementary QEMU TCG/PureDarwin route",
        ))
    elif x86 and system in {"Windows", "Darwin"}:
        checks.append(check(
            "host-profile", "warning",
            f"x86-64 {system} has only hosted-runner candidate evidence; physical-host validation is still missing",
        ))
    elif arm64 and system == "Linux":
        checks.append(check(
            "host-profile", "pass",
            "ARM64 Linux is the existing direct hosted-XNU profile; this does not exercise the x86-64 TCG route",
        ))
    else:
        checks.append(check(
            "host-profile", "fail",
            f"unsupported or unvalidated host profile: {system} {facts['machine']}",
        ))

    qemu = facts["qemu"]
    if not qemu["path"]:
        if arm64 and system == "Linux":
            checks.append(check(
                "qemu", "info",
                "qemu-system-aarch64 was not found; it is not required for the direct ARM64 hosted-XNU path, only for TCG reproduction",
            ))
        else:
            checks.append(check(
                "qemu", "fail",
                "qemu-system-aarch64 was not found in PATH; the complementary ARM64 guest route cannot start",
            ))
    elif qemu["version_parts"]:
        version = qemu["version_parts"]
        if version < (8, 2, 0):
            checks.append(check(
                "qemu", "warning",
                f"QEMU {qemu['version']} is older than the hosted 8.2.2 reference and may lack newer CPU models such as cortex-a710",
                {"path": qemu["path"]},
            ))
        else:
            checks.append(check(
                "qemu", "pass",
                f"qemu-system-aarch64 {qemu['version']} found",
                {"path": qemu["path"]},
            ))
    else:
        checks.append(check(
            "qemu", "warning",
            "qemu-system-aarch64 was found but its version could not be parsed",
            qemu,
        ))

    total = facts["memory_total_bytes"]
    available = facts["memory_available_bytes"]
    if total:
        available_text = f"{available / GIB:.1f} GiB" if available is not None else "unknown"
        checks.append(check(
            "memory", "info",
            f"host RAM total {total / GIB:.1f} GiB, available {available_text}; no physical RAM floor is established",
            {"total_bytes": total, "available_bytes": available},
        ))
    else:
        checks.append(check("memory", "warning", "host memory could not be determined"))

    if facts["free_disk_bytes"] is not None:
        free = facts["free_disk_bytes"]
        checks.append(check(
            "disk", "info",
            f"{free / GIB:.1f} GiB free in the current filesystem; artifact preparation can require substantial temporary space",
            {"free_bytes": free},
        ))

    if x86 and system == "Linux":
        flags = set(facts["linux_cpu_flags"])
        missing = [feature for feature in X86_64H_FEATURES if feature not in flags]
        details = {"missing_features": missing, "note": "informational for the experimental native x86_64 port; not required by QEMU TCG"}
        summary = "First Linux CPU entry reports all listed x86_64h flags"
        if missing:
            summary = "Flags missing from the first Linux CPU entry: " + " ".join(missing)
        checks.append(check("native-x86-feature-floor", "info", summary, details))
        kvm_access = os.access("/dev/kvm", os.R_OK | os.W_OK)
        checks.append(check(
            "kvm", "info",
            "/dev/kvm is " + ("accessible" if kvm_access else "not accessible") + "; KVM is not required for the QEMU TCG route",
        ))

    overall = "pass"
    if any(item["status"] == "fail" for item in checks):
        overall = "fail"
    elif any(item["status"] == "warning" for item in checks):
        overall = "warning"
    return checks, overall


def human_report(report):
    lines = [
        "xnulinux doctor (diagnostic only; not a compatibility certification)",
        f"Host: {report['facts']['system']} {report['facts']['release']} ({report['facts']['machine']}), logical CPUs: {report['facts']['logical_cpus']}",
    ]
    facts = report["facts"]
    cpu_parts = (facts.get("linux_cpu_vendor"), facts.get("linux_cpu_model"))
    if any(cpu_parts):
        lines.append("Linux CPU: " + ", ".join(part for part in cpu_parts if part))
    if facts.get("linux_page_size_bytes") is not None:
        lines.append(f"Linux page size: {facts['linux_page_size_bytes']} bytes")
    for item in report["checks"]:
        lines.append(f"{item['status'].upper()}: {item['name']}: {item['summary']}")
    lines.append(f"Overall: {report['overall']}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--strict", action="store_true", help="exit nonzero for warnings as well as failures")
    args = parser.parse_args(argv)

    facts = collect_facts()
    checks, overall = evaluate(facts)
    report = {"schema": 1, "facts": facts, "checks": checks, "overall": overall}
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(human_report(report))
    if overall == "fail" or (args.strict and overall == "warning"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
