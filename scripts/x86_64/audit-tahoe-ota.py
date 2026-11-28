#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inventory the ZIP directory of the pinned Intel Tahoe software update."""

import argparse
import hashlib
import json
import mmap
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
BOOT_MEMBER = "AssetData/boot/System/Library/KernelCollections/BootKernelExtensions.kc"
BOOT_MEMBER_BYTES = 67_600_384
XNU_RELEASE_PATTERN = re.compile(rb"xnu-([0-9]+(?:\.[0-9]+){2,}(?:~[0-9]+)?)/RELEASE_X86_64\b")
COLLECTION_NAMES = {"BootKernelExtensions.kc", "SystemKernelExtensions.kc"}
COLLECTION_DELTAS = {f"{name}.bxdiff" for name in COLLECTION_NAMES}
PAYLOAD_ROOT = PurePosixPath("AssetData/payloadv2")
NAMED_PAYLOADS = {PAYLOAD_ROOT / name for name in ("data_payload", "firmlinks_payload")}
PAYLOAD_PART = re.compile(r"payload\.\d+(?:\.(?:yaa|aar|aea))?", re.ASCII)
MAX_REPORT_ENTRIES = 32


class AuditError(Exception):
    pass


def entry_kind(filename):
    path = PurePosixPath(filename)
    name = path.name
    is_collection = name in COLLECTION_NAMES or name in COLLECTION_DELTAS
    if path.is_relative_to(PAYLOAD_ROOT / "ecc_data"):
        return "collection_ecc" if is_collection else None
    if path.is_relative_to(PAYLOAD_ROOT / "patches"):
        return "collection_delta" if is_collection else None
    if name in COLLECTION_DELTAS:
        return "collection_delta"
    if name in COLLECTION_NAMES:
        return "collection_file"
    if name.endswith(".bom"):
        return "bom"
    if path in NAMED_PAYLOADS or PAYLOAD_PART.fullmatch(name) or name.endswith((".yaa", ".aar", ".aea")):
        return "payload_archive"
    return None


def inventory(archive):
    entries = archive.infolist()
    groups = {kind: {"count": 0, "entries": []} for kind in (
        "collection_file", "collection_delta", "collection_ecc", "bom", "payload_archive",
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


def inspect_boot_collection(archive, inspector, report, expected_size=BOOT_MEMBER_BYTES):
    """Copy and inspect the exact OTA boot member, never a patch for the installed System."""
    matches = [entry for entry in archive.infolist() if entry.filename == BOOT_MEMBER]
    if len(matches) != 1:
        raise AuditError("expected exactly one OTA boot collection member")
    entry = matches[0]
    mode = stat.S_IFMT(entry.external_attr >> 16) if entry.create_system == 3 else 0
    if entry.is_dir() or mode not in (0, stat.S_IFREG) or entry.file_size != expected_size:
        raise AuditError("unexpected OTA boot collection member type or length")

    with tempfile.TemporaryDirectory(prefix="lixnu-ota-bootkc-") as directory:
        collection = Path(directory) / "BootKernelExtensions.kc"
        digest = hashlib.sha256()
        remaining = expected_size
        with archive.open(entry) as source, collection.open("xb") as destination:
            while remaining:
                chunk = source.read(min(remaining, 1 << 20))
                if not chunk:
                    raise AuditError("truncated OTA boot collection member")
                destination.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
        report.update(path=BOOT_MEMBER, bytes=expected_size, sha256=digest.hexdigest())
        with collection.open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            report["xnu_release_candidates"] = sorted({
                match.decode("ascii") for match in XNU_RELEASE_PATTERN.findall(data)
            })
        result = subprocess.run([str(inspector), str(collection)], capture_output=True,
                                text=True, timeout=120, check=True)
        report["fileset"] = json.loads(result.stdout)
        fileset = report["fileset"]
        if (fileset.get("status") != "accepted" or fileset.get("parser_result") != 0
            or not fileset.get("kernel", {}).get("segments")):
            raise AuditError("fileset inspector did not accept an Intel kernel with segments")


def inspect(path, report, expected_size=OTA_BYTES, expected_sha256=OTA_SHA256, inspector=None):
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
            if inspector is not None:
                result["ota_boot_collection"] = {}
                try:
                    inspect_boot_collection(archive, inspector, result["ota_boot_collection"])
                except (AuditError, OSError, subprocess.SubprocessError, ValueError, zipfile.BadZipFile) as error:
                    message = str(error) if isinstance(error, AuditError) else "unable to inspect the OTA boot member"
                    result["ota_boot_collection"]["error"] = message
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_size, after.st_mtime_ns, after.st_ctime_ns
        ):
            raise AuditError("OTA changed during inspection")
        report["source_digest_verified"] = True
        report.update(result)
        if inspector is not None and "error" in result["ota_boot_collection"]:
            raise AuditError(result["ota_boot_collection"]["error"])


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
    parser.add_argument("--fileset-inspector", type=Path,
                        help="also inspect the OTA boot member with the compiled xnu-fileset-metadata helper")
    args = parser.parse_args()
    inspector = args.fileset_inspector.resolve() if args.fileset_inspector is not None else None

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
            inspect(args.archive, report, inspector=inspector)
        else:
            with tempfile.TemporaryDirectory(prefix="lixnu-tahoe-ota-") as directory:
                archive = Path(directory) / "intel-ota.zip"
                download(archive)
                inspect(archive, report, inspector=inspector)
    except AuditError as error:
        report.update(status="failed", error=str(error))
    except (OSError, subprocess.SubprocessError, zipfile.BadZipFile):
        report.update(status="failed", error="unable to obtain or read the OTA ZIP")
    else:
        report["status"] = "ota_boot_collection_inspected" if inspector is not None else "zip_directory_inventoried"
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if report["status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
