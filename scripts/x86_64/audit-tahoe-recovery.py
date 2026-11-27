#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect the Intel Internet Recovery image returned by the catalog."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import plistlib
import re
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
RECOVERY_BASENAME = "verified-recovery"
BOOT_KERNEL_COLLECTION = Path("System/Library/KernelCollections/BootKernelExtensions.kc")
TARGET_RUNTIME_VERSION = "26.7.3"
TARGET_RUNTIME_BUILD = "25G342"
# Public source releases use three components, but shipping build trains
# can append more. These strings are observations, not artifact identities.
XNU_RELEASE_PATTERN = re.compile(rb"xnu-([0-9]+(?:\.[0-9]+){2,}(?:~[0-9]+)?)/RELEASE_X86_64\b")


def run(command: list[str], *, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
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


def resolve_image_target(path: Path, mount: Path) -> Path | None:
    """Resolve guest paths component-by-component, including chained absolute links."""
    image_root = mount.resolve()
    try:
        try:
            relative_parts = path.relative_to(mount).parts
        except ValueError:
            relative_parts = path.relative_to(image_root).parts
    except ValueError:
        return None

    current = image_root
    pending = list(relative_parts)
    symlink_count = 0
    while pending:
        component = pending.pop(0)
        if component in ("", "."):
            continue
        if component == "..":
            if current == image_root:
                return None
            current = current.parent
            continue

        candidate = current / component
        try:
            if candidate.is_symlink():
                symlink_count += 1
                if symlink_count > 40:
                    return None
                raw_target = os.readlink(candidate)
                if raw_target.startswith("/"):
                    current = image_root
                    target_parts = raw_target.lstrip("/").split("/")
                else:
                    target_parts = raw_target.split("/")
                pending = target_parts + pending
                continue
            candidate.lstat()
        except OSError:
            return None
        current = candidate

    if current != image_root and image_root not in current.parents:
        return None
    return current


def walk_image_entries(mount: Path, link_counts: dict[str, int]):
    """Walk the mounted image and follow only symlinks that stay inside it."""
    image_root = mount.resolve()
    pending: list[tuple[Path, Path, frozenset[Path]]] = [
        (mount, image_root, frozenset({image_root}))
    ]
    while pending:
        logical_directory, actual_directory, ancestors = pending.pop()
        try:
            children = sorted(actual_directory.iterdir(), key=lambda item: item.name, reverse=True)
        except OSError:
            continue
        for actual_child in children:
            logical_child = logical_directory / actual_child.name
            if actual_child.is_symlink():
                target = resolve_image_target(actual_child, mount)
                if target is None:
                    link_counts["outside_or_unresolved"] += 1
                    continue
                if target.is_dir():
                    if target in ancestors:
                        link_counts["directory_cycles_skipped"] += 1
                        continue
                    link_counts["directories_followed"] += 1
                    pending.append((logical_child, target, ancestors | {target}))
                    continue
                if target.is_file():
                    link_counts["files_followed"] += 1
                    yield logical_child, target
                    continue
                continue
            if actual_child.is_dir():
                resolved_child = actual_child.resolve()
                pending.append((logical_child, resolved_child, ancestors | {resolved_child}))
            elif actual_child.is_file():
                yield logical_child, actual_child


def host_metadata() -> dict[str, Any]:
    result: dict[str, Any] = {"platform": platform.platform(), "machine": platform.machine()}
    sw_vers = shutil.which("sw_vers")
    if sw_vers:
        version = run([sw_vers, "-productVersion"])
        build = run([sw_vers, "-buildVersion"])
        result["macos_product_version"] = version.stdout.strip() if version.returncode == 0 else None
        result["macos_build"] = build.stdout.strip() if build.returncode == 0 else None
    return result


def inspect_mounted_image(mount: Path, image: Path, fileset_helper: Path) -> dict[str, Any]:
    image_root = mount.resolve()
    result: dict[str, Any] = {
        "image_filename": image.name,
        "image_bytes": image.stat().st_size,
        "image_sha256": sha256(image),
        "system_product_version": None,
        "system_product_build": None,
        "recovery_matches_runtime_goal": False,
        "xnu_version": None,
        "xnu_version_candidates": [],
        "xnu_version_source": None,
        "system_kext_bundle_version": None,
        "kernel_artifacts": [],
        "kernel_search_roots": [],
        "trustcache_api_headers": [],
        "kdk_supplied": False,
    }

    system_version = mount / "System/Library/CoreServices/SystemVersion.plist"
    if system_version.is_file():
        with system_version.open("rb") as source:
            version = plistlib.load(source)
        result["system_product_version"] = version.get("ProductVersion")
        result["system_product_build"] = version.get("ProductBuildVersion")
        result["recovery_matches_runtime_goal"] = (
            result["system_product_version"] == TARGET_RUNTIME_VERSION
            and result["system_product_build"] == TARGET_RUNTIME_BUILD
        )

    for system_kext in (
        mount / "System/Library/Extensions/System.kext/Info.plist",
        mount / "System/Library/Extensions/System.kext/Contents/Info.plist",
    ):
        if system_kext.is_file():
            with system_kext.open("rb") as source:
                metadata = plistlib.load(source)
            result["system_kext_bundle_version"] = metadata.get("CFBundleVersion")
            break

    trusted_headers: list[str] = []
    kernel_artifacts: list[dict[str, Any]] = []
    xnu_version_candidates: set[str] = set()
    link_counts = {
        "directories_followed": 0,
        "files_followed": 0,
        "directory_cycles_skipped": 0,
        "outside_or_unresolved": 0,
    }
    kernel_names = {
        "kernel",
        "mach_kernel",
        "kernelcache",
        "prelinkedkernel",
        "kernel.development",
        "bootkernelextensions.kc",
        "systemkernelextensions.kc",
    }
    for logical_path, actual_path in walk_image_entries(mount, link_counts):
        relative_path = logical_path.relative_to(mount)
        if logical_path.name == "API.h" and logical_path.parent.name == "TrustCache":
            trusted_headers.append(str(relative_path))
        in_kernel_collection_dir = "KernelCollections" in relative_path.parts
        if logical_path.name.lower() not in kernel_names and not (
            in_kernel_collection_dir and logical_path.name.lower().endswith(".kc")
        ):
            continue
        try:
            item: dict[str, Any] = {
                "path": str(relative_path),
                "resolved_path": str(actual_path.relative_to(image_root)),
                "entry_type": "symlink" if logical_path.is_symlink() else "file",
                "bytes": actual_path.stat().st_size,
                "sha256": sha256(actual_path),
            }
            file_tool = shutil.which("file")
            if file_tool:
                description = run([file_tool, "-b", str(actual_path)])
                if description.returncode == 0:
                    item["file_type"] = description.stdout.strip()
            if logical_path.name == "BootKernelExtensions.kc":
                xnu_version_candidates.update(
                    match.decode("ascii")
                    for match in XNU_RELEASE_PATTERN.findall(actual_path.read_bytes())
                )
            kernel_artifacts.append(item)
        except OSError:
            continue

    for relative_root in (
        Path("System/Library/Kernels"),
        Path("System/Library/KernelCollections"),
        Path("System/Library/PrelinkedKernels"),
        Path("System/Library/Extensions/System.kext"),
    ):
        root = mount / relative_root
        actual_root = resolve_image_target(root, mount)
        if actual_root is None:
            state = "absent"
            entries: list[dict[str, str]] = []
        elif actual_root.is_dir():
            state = "directory"
            entries = [
                {
                    "name": entry.name,
                    "entry_type": "symlink" if entry.is_symlink() else "directory" if entry.is_dir() else "file",
                }
                for entry in sorted(actual_root.iterdir(), key=lambda item: item.name)[:100]
            ]
        else:
            state = "file"
            entries = []
        result["kernel_search_roots"].append(
            {"path": str(relative_root), "state": state, "entries": entries}
        )

    result["kernel_artifacts"] = kernel_artifacts
    result["mounted_symlinks_followed"] = link_counts
    result["xnu_version_candidates"] = sorted(xnu_version_candidates)
    if len(xnu_version_candidates) == 1:
        result["xnu_version"] = next(iter(xnu_version_candidates))
        result["xnu_version_source"] = "BootKernelExtensions.kc"
    result["trustcache_api_headers"] = sorted(set(trusted_headers))
    result["trustcache_api_header_present"] = bool(trusted_headers)
    collection = resolve_image_target(mount / BOOT_KERNEL_COLLECTION, mount)
    fileset: dict[str, Any] = {"path": str(BOOT_KERNEL_COLLECTION)}
    if collection is None or not collection.is_file():
        fileset["status"] = "collection_missing"
    else:
        inspection = run([str(fileset_helper), str(collection)], timeout=120)
        if inspection.returncode == 0:
            fileset.update(json.loads(inspection.stdout))
        else:
            fileset.update(status="helper_failed", helper_exit_code=inspection.returncode)
    result["boot_kernel_collection_fileset"] = fileset
    return result


def attached_image_devices(hdiutil: str, image: Path) -> list[str] | None:
    info = run([hdiutil, "info", "-plist"], timeout=30)
    if info.returncode != 0:
        return None
    try:
        images = plistlib.loads(info.stdout.encode("utf-8"))["images"]
        devices = []
        for attachment in images:
            image_path = attachment.get("image-path")
            if not image_path or Path(image_path).resolve() != image.resolve():
                continue
            entries = [
                entry.get("dev-entry", "")
                for entry in attachment["system-entities"]
            ]
            whole_devices = [
                entry for entry in entries
                if isinstance(entry, str) and re.fullmatch(r"/dev/disk[0-9]+", entry)
            ]
            if not whole_devices:
                return None
            devices.extend(whole_devices)
        return devices
    except (KeyError, TypeError, ValueError, plistlib.InvalidFileException):
        return None


def detach_recovery(hdiutil: str, image: Path, mount: Path) -> bool:
    for options in ([], ["-force"]):
        devices = attached_image_devices(hdiutil, image)
        if devices == [] and not mount.is_mount():
            return True
        # This mount point is private to the audit, including on attach timeout.
        for target in devices or ([str(mount)] if mount.is_mount() else []):
            run([hdiutil, "detach", "-quiet", *options, target], timeout=60)
    return attached_image_devices(hdiutil, image) == [] and not mount.is_mount()


def inspect_recovery(
    report: dict[str, Any], image: Path, fileset_helper: Path, temporary_path: Path
) -> None:
    hdiutil = shutil.which("hdiutil")
    if not hdiutil:
        report["audit_status"] = "recovery_downloaded_but_hdiutil_missing"
        return

    # Keep the mount point outside TemporaryDirectory's recursive cleanup.
    mount = Path(tempfile.mkdtemp(prefix="xnulinux-recovery-mount-", dir=temporary_path.parent))
    try:
        attach = run(
            [hdiutil, "attach", "-readonly", "-nobrowse", "-quiet", "-mountpoint", str(mount), str(image)],
            timeout=600,
        )
        if attach.returncode != 0 or not mount.is_mount():
            report["audit_status"] = "recovery_downloaded_but_mount_failed"
            report["failure_class"] = error_class(attach.stdout + attach.stderr)
            return
        try:
            mounted = inspect_mounted_image(mount, image, fileset_helper)
            report["recovery_image"] = mounted
            status = mounted["boot_kernel_collection_fileset"]["status"]
            report["audit_status"] = (
                "recovery_downloaded_and_inspected"
                if status in ("accepted", "parser_rejected", "not_fileset")
                else "recovery_downloaded_but_inspection_failed"
            )
        except (OSError, plistlib.InvalidFileException, ValueError) as error:
            report["audit_status"] = "recovery_downloaded_but_inspection_failed"
            report["failure_class"] = type(error).__name__
    finally:
        try:
            report["recovery_detached"] = detach_recovery(hdiutil, image, mount)
        except OSError:
            report["recovery_detached"] = False
        try:
            mount.rmdir()
            report["temporary_mount_directory_removed"] = True
        except OSError:
            report["temporary_mount_directory_removed"] = False
        if not report["recovery_detached"] or not report["temporary_mount_directory_removed"]:
            report["audit_status"] = "recovery_mount_cleanup_failed"


def audit_temporary_recovery(report: dict[str, Any], temporary_path: Path) -> None:
    repo = Path(__file__).resolve().parents[2]
    fileset_helper = temporary_path / "xnu-fileset-metadata"
    compiler = run(
        [
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(repo / "tools/dev/x86_64/xnu-fileset-metadata.c"),
            str(repo / "xnu-ko/macho_parse_x86_64.c"),
            "-o", str(fileset_helper),
        ],
        timeout=120,
    )
    if compiler.returncode != 0:
        report["audit_status"] = "fileset_helper_build_failed"
        report["failure_class"] = error_class(compiler.stdout + compiler.stderr)
        print(compiler.stderr, file=sys.stderr, end="")
        return

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
    if fetch.returncode != 0:
        report["audit_status"] = "opencore_source_fetch_failed"
        report["failure_class"] = error_class(fetch.stdout + fetch.stderr)
        return
    resolved = run(["git", "-C", str(source), "rev-parse", "HEAD"])
    if resolved.returncode != 0 or resolved.stdout.strip() != OPENCORE_COMMIT:
        report["audit_status"] = "opencore_source_pin_mismatch"
        return

    downloader = source / "Utilities/macrecovery/macrecovery.py"
    download_dir = temporary_path / "recovery"
    download_dir.mkdir()
    download = run(
        [
            sys.executable, "-E", str(downloader),
            "-o", str(download_dir),
            "-n", RECOVERY_BASENAME,
            "-b", BOARD_ID,
            "-m", MLB,
            "-os", "latest",
            "download",
        ],
        timeout=3000,
    )
    if download.returncode != 0:
        report["audit_status"] = "recovery_download_or_chunklist_verification_failed"
        report["failure_class"] = error_class(download.stdout + download.stderr)
        report["macrecovery_exit_code"] = download.returncode
        return

    image = download_dir / f"{RECOVERY_BASENAME}.dmg"
    chunklist = download_dir / f"{RECOVERY_BASENAME}.chunklist"
    if not image.is_file() or not chunklist.is_file():
        report["audit_status"] = "verified_recovery_pair_missing"
        return

    report["apple_chunklist_verified"] = True
    inspect_recovery(report, image, fileset_helper, temporary_path)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} REPORT.json", file=sys.stderr)
        return 2

    report_path = Path(sys.argv[1]).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema_version": 2,
        "audit_status": "started",
        "recovery_source": "OpenCorePkg Utilities/macrecovery",
        "opencore_commit": OPENCORE_COMMIT,
        "board_id": BOARD_ID,
        "os_selector": "latest",
        "target_runtime_goal": f"macOS Tahoe {TARGET_RUNTIME_VERSION}",
        "target_runtime_build": TARGET_RUNTIME_BUILD,
        "catalog_transport": "HTTP (OpenCore Internet Recovery protocol)",
        # Chunklist validation authenticates image bytes, not catalog selection.
        "catalog_selection_authenticated": False,
        "recovery_product_identity_verified": False,
        "apple_chunklist_verified": False,
        "host": host_metadata(),
        "recovery_image_uploaded": False,
        "kdk_supplied": False,
    }

    temporary_path = None
    try:
        with tempfile.TemporaryDirectory(prefix="tahoe-recovery-audit-") as temporary:
            temporary_path = Path(temporary)
            audit_temporary_recovery(report, temporary_path)
    except OSError as error:
        report["audit_status"] = "recovery_temporary_workspace_failed"
        report["failure_class"] = type(error).__name__
    report["temporary_recovery_files_removed"] = (
        temporary_path is not None and not temporary_path.exists()
    )

    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Recovery audit report written: {report_path}")
    print(f"Audit status: {report['audit_status']}")
    fileset = report.get("recovery_image", {}).get("boot_kernel_collection_fileset")
    if fileset:
        print(f"Fileset inspection: {fileset['status']} (parser result: {fileset.get('parser_result')})")
        segments = fileset.get("kernel", {}).get("segments")
        if fileset["status"] == "accepted" and isinstance(segments, list):
            print(f"Fileset kernel segments reported: {len(segments)}")
    print(f"Recovery detached: {report.get('recovery_detached')}")
    print(f"Temporary Recovery files removed: {report['temporary_recovery_files_removed']}")
    if not report["temporary_recovery_files_removed"]:
        return 1
    return 0 if report["audit_status"] == "recovery_downloaded_and_inspected" else 1


if __name__ == "__main__":
    raise SystemExit(main())
