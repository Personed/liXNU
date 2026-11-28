#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inventory the ZIP directory of the pinned Intel Tahoe software update."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import zipfile


# AppleDB record 25G342 at 2d59226769714041389efda35daf4b9a938ba8a9.
OTA_URL = (
    "https://updates.cdn-apple.com/2026FallFCS/f3454894-99b2-4749-b569-b52fcfc410ea/"
    "com_apple_MobileAsset_MacSoftwareUpdate/f8edc36b86d55418b3534012d22e69c96720f6c3.zip"
)
OTA_BYTES = 1461006831
OTA_SHA256 = "b7b97ed3337af7408879da969964328f6d78282ab837333d1cc2f0507f4019a4"
COLLECTION_NAMES = {"BootKernelExtensions.kc", "SystemKernelExtensions.kc"}
PAYLOAD_PART = re.compile(r"payload\.\d+(?:\.(?:yaa|aar|aea))?", re.ASCII)
MAX_REPORT_ENTRIES = 32


class AuditError(Exception):
    pass


def entry_kind(filename):
    path = PurePosixPath(filename)
    name = path.name
    if name in COLLECTION_NAMES:
        return "collection_file"
    if name.removesuffix(".bxdiff") in COLLECTION_NAMES and name.endswith(".bxdiff"):
        return "collection_delta"
    if name.endswith(".bom"):
        return "bom"
    if PAYLOAD_PART.fullmatch(name) or name.endswith((".yaa", ".aar", ".aea")):
        return "payload_archive"
    return None


def inventory(archive):
    entries = archive.infolist()
    groups = {kind: {"count": 0, "entries": []} for kind in (
        "collection_file", "collection_delta", "bom", "payload_archive",
    )}
    for entry in entries:
        mode = entry.external_attr >> 16
        if entry.is_dir() or (entry.create_system == 3 and stat.S_ISLNK(mode)):
            continue
        kind = entry_kind(entry.filename)
        if kind is None:
            continue
        group = groups[kind]
        group["count"] += 1
        if len(group["entries"]) < MAX_REPORT_ENTRIES:
            group["entries"].append({"path": entry.filename, "bytes": entry.file_size})
    return {"zip_entries": len(entries), "matches": groups}


def inspect(path, report, expected_size=OTA_BYTES, expected_sha256=OTA_SHA256):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as source:
        # Verify the pinned object before asking zipfile to read its directory.
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise AuditError("OTA must be a regular file")
        report["bytes"] = before.st_size
        if before.st_size != expected_size:
            raise AuditError("unexpected OTA byte count")
        digest = hashlib.sha256()
        remaining = before.st_size
        while remaining:
            chunk = source.read(min(remaining, 1 << 20))
            if not chunk:
                raise AuditError("OTA was truncated")
            digest.update(chunk)
            remaining -= len(chunk)
        if source.read(1):
            raise AuditError("OTA grew during inspection")
        report["sha256"] = digest.hexdigest()
        if report["sha256"] != expected_sha256:
            raise AuditError("unexpected OTA SHA-256")
        source.seek(0)
        with zipfile.ZipFile(source) as archive:
            result = inventory(archive)
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_size, after.st_mtime_ns, after.st_ctime_ns
        ):
            raise AuditError("OTA changed during inspection")
        report["source_digest_verified"] = True
        report.update(result)


def download(path):
    result = subprocess.run([
        "curl", "--fail", "--silent", "--show-error", "--location", "--max-redirs", "3",
        "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "20",
        "--max-time", "360", "--retry", "1", "--retry-max-time", "360",
        "--max-filesize", str(OTA_BYTES), "--output", str(path), OTA_URL,
    ], capture_output=True, timeout=750, check=False)
    if result.returncode:
        raise AuditError(f"OTA download failed (curl exit {result.returncode})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--archive", type=Path, help="inspect a local copy of the pinned ZIP")
    source.add_argument("--download", action="store_true", help="download the pinned ZIP to a temporary directory")
    args = parser.parse_args()

    report = {
        "schema_version": 1,
        "catalog_prerequisite_build": "25G76",
        "catalog_target_build": "25G342",
        "source_digest_verified": False,
        "installed_collection_verified": False,
        "boot_verified": False,
    }
    try:
        if args.archive is not None:
            inspect(args.archive, report)
        else:
            with tempfile.TemporaryDirectory(prefix="lixnu-tahoe-ota-") as directory:
                archive = Path(directory) / "intel-ota.zip"
                download(archive)
                inspect(archive, report)
    except AuditError as error:
        report.update(status="failed", error=str(error))
    except (OSError, subprocess.SubprocessError, zipfile.BadZipFile):
        report.update(status="failed", error="unable to obtain or read the OTA ZIP")
    else:
        report["status"] = "zip_directory_inventoried"
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "zip_directory_inventoried" else 1


if __name__ == "__main__":
    raise SystemExit(main())
