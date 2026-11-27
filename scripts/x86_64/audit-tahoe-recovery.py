#!/usr/bin/env python3
"""Inspect Apple's latest Tahoe Internet Recovery image without retaining it."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


OPENCORE_COMMIT = "ebea824880706ac9faa8ee45604245de62be658b"
OPENCORE_URL = "https://github.com/acidanthera/OpenCorePkg.git"
BOARD_ID = "Mac-CFF7D910A743CAAF"
MLB = "00000000000000000"


def run(command: list[str], *, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        return subprocess.CompletedProcess(command, 124, stdout, stderr + " timeout")


def error_class(output: str) -> str:
    message = output.lower()
    if "http error 403" in message or "forbidden" in message or "connect tunnel" in message:
        return "apple_endpoint_forbidden"
    if "http error" in message:
        return "apple_http_error"
    if "image verification failed" in message or "invalid chunk" in message:
        return "apple_chunklist_verification_failed"
    if "timed out" in message or "timeout" in message:
        return "network_timeout"
    return "external_command_failed"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def host_metadata() -> dict[str, Any]:
    result: dict[str, Any] = {"platform": platform.platform(), "machine": platform.machine()}
    sw_vers = shutil.which("sw_vers")
    if sw_vers:
        version = run([sw_vers, "-productVersion"])
        build = run([sw_vers, "-buildVersion"])
        result["macos_product_version"] = version.stdout.strip() if version.returncode == 0 else None
        result["macos_build"] = build.stdout.strip() if build.returncode == 0 else None
    return result


def inspect_mounted_image(mount: Path, image: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "image_filename": image.name,
        "image_bytes": image.stat().st_size,
        "image_sha256": sha256(image),
        "system_product_version": None,
        "system_product_build": None,
        "xnu_version": None,
        "kernel_artifacts": [],
        "trustcache_api_headers": [],
        "kdk_supplied": False,
    }

    system_version = mount / "System/Library/CoreServices/SystemVersion.plist"
    if system_version.is_file():
        with system_version.open("rb") as source:
            version = plistlib.load(source)
        result["system_product_version"] = version.get("ProductVersion")
        result["system_product_build"] = version.get("ProductBuildVersion")

    for system_kext in (
        mount / "System/Library/Extensions/System.kext/Info.plist",
        mount / "System/Library/Extensions/System.kext/Contents/Info.plist",
    ):
        if system_kext.is_file():
            with system_kext.open("rb") as source:
                metadata = plistlib.load(source)
            result["xnu_version"] = metadata.get("CFBundleVersion")
            break

    trusted_headers: list[str] = []
    kernel_artifacts: list[dict[str, Any]] = []
    for root, dirs, files in os.walk(mount):
        directory = Path(root)
        if directory.name == "TrustCache" and "API.h" in files:
            trusted_headers.append(str((directory / "API.h").relative_to(mount)))
        for name in files:
            if name not in {"kernel", "kernelcache", "kernel.development"}:
                continue
            path = directory / name
            try:
                item: dict[str, Any] = {
                    "path": str(path.relative_to(mount)),
                    "bytes": path.stat().st_size,
                    "sha256": sha256(path),
                }
                file_tool = shutil.which("file")
                if file_tool:
                    description = run([file_tool, "-b", str(path)])
                    if description.returncode == 0:
                        item["file_type"] = description.stdout.strip()
                kernel_artifacts.append(item)
            except OSError:
                continue

    result["kernel_artifacts"] = kernel_artifacts
    result["trustcache_api_headers"] = sorted(set(trusted_headers))
    result["trustcache_api_header_present"] = bool(trusted_headers)
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} REPORT.json", file=sys.stderr)
        return 2

    report_path = Path(sys.argv[1]).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema_version": 1,
        "audit_status": "started",
        "recovery_source": "OpenCorePkg Utilities/macrecovery",
        "opencore_commit": OPENCORE_COMMIT,
        "board_id": BOARD_ID,
        "os_selector": "latest",
        "target_runtime_goal": "macOS Tahoe 26.7.1",
        "host": host_metadata(),
        "recovery_image_uploaded": False,
        "kdk_supplied": False,
    }

    with tempfile.TemporaryDirectory(prefix="tahoe-recovery-audit-") as temporary:
        temporary_path = Path(temporary)
        source = temporary_path / "OpenCorePkg"
        fetch = run(["git", "init", str(source)])
        if fetch.returncode == 0:
            fetch = run(["git", "-C", str(source), "remote", "add", "origin", OPENCORE_URL])
        if fetch.returncode == 0:
            fetch = run(
                ["git", "-C", str(source), "fetch", "--depth=1", "origin", OPENCORE_COMMIT],
                timeout=600,
            )
        if fetch.returncode == 0:
            fetch = run(["git", "-C", str(source), "checkout", "--detach", "FETCH_HEAD"])
        if fetch.returncode == 0:
            resolved = run(["git", "-C", str(source), "rev-parse", "HEAD"])
            if resolved.returncode != 0 or resolved.stdout.strip() != OPENCORE_COMMIT:
                fetch = subprocess.CompletedProcess(
                    resolved.args,
                    1,
                    resolved.stdout,
                    "pinned OpenCore commit mismatch",
                )
        if fetch.returncode != 0:
            report["audit_status"] = "opencore_source_fetch_failed"
            report["failure_class"] = error_class(fetch.stdout + fetch.stderr)
        else:
            downloader = source / "Utilities/macrecovery/macrecovery.py"
            download_dir = temporary_path / "recovery"
            download_dir.mkdir()
            download = run(
                [
                    sys.executable,
                    str(downloader),
                    "-o",
                    str(download_dir),
                    "-b",
                    BOARD_ID,
                    "-m",
                    MLB,
                    "-os",
                    "latest",
                    "download",
                ],
                timeout=5400,
            )
            if download.returncode != 0:
                report["audit_status"] = "recovery_download_or_chunklist_verification_failed"
                report["failure_class"] = error_class(download.stdout + download.stderr)
                report["macrecovery_exit_code"] = download.returncode
                report["apple_chunklist_verified"] = False
            else:
                report["apple_chunklist_verified"] = True
                images = sorted(download_dir.glob("*.dmg"))
                if not images:
                    report["audit_status"] = "recovery_image_missing_after_download"
                else:
                    image = images[0]
                    mount = temporary_path / "mounted-recovery"
                    mount.mkdir()
                    hdiutil = shutil.which("hdiutil")
                    if not hdiutil:
                        report["audit_status"] = "recovery_downloaded_but_hdiutil_missing"
                        report["image_sha256"] = sha256(image)
                        report["image_bytes"] = image.stat().st_size
                    else:
                        attach = run(
                            [hdiutil, "attach", "-readonly", "-nobrowse", "-quiet", "-mountpoint", str(mount), str(image)],
                            timeout=900,
                        )
                        if attach.returncode != 0:
                            report["audit_status"] = "recovery_downloaded_but_mount_failed"
                            report["failure_class"] = error_class(attach.stdout + attach.stderr)
                            report["image_sha256"] = sha256(image)
                            report["image_bytes"] = image.stat().st_size
                        else:
                            try:
                                report["recovery_image"] = inspect_mounted_image(mount, image)
                                report["audit_status"] = "recovery_downloaded_and_inspected"
                            except (OSError, plistlib.InvalidFileException, ValueError) as error:
                                report["audit_status"] = "recovery_downloaded_but_inspection_failed"
                                report["failure_class"] = type(error).__name__
                            finally:
                                run([hdiutil, "detach", "-quiet", str(mount)], timeout=300)

    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Recovery audit report written: {report_path}")
    print(f"Audit status: {report['audit_status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
