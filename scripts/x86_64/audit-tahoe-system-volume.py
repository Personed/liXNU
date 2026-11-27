#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect the boot collection on a locally supplied Intel Tahoe System volume."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import tempfile
from xml.etree import ElementTree
from xml.parsers.expat import ExpatError


TARGET_VERSION = "26.7.3"
TARGET_BUILD = "25G342"
VERSION_PLIST = "System/Library/CoreServices/SystemVersion.plist"
BOOT_COLLECTION = "System/Library/KernelCollections/BootKernelExtensions.kc"
REPO_ROOT = Path(__file__).resolve().parents[2]


class AuditError(Exception):
    pass


def snapshot(root, relative_path, limit, destination):
    """Hash exactly the bytes copied from a bounded regular file."""
    path = root / relative_path
    try:
        if not path.resolve(strict=True).is_relative_to(root):
            raise AuditError(f"{relative_path} resolves outside the System volume")
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as source:
            before = os.fstat(source.fileno())
            if before.st_dev != root.stat().st_dev:
                raise AuditError(f"{relative_path} is on a different filesystem from the System volume")
            if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
                raise AuditError(f"{relative_path} must be a nonempty regular file within the size limit")
            digest = hashlib.sha256()
            remaining = before.st_size
            while remaining:
                block = source.read(min(remaining, 1 << 20))
                if not block:
                    raise AuditError(f"{relative_path} was truncated during inspection")
                destination.write(block)
                digest.update(block)
                remaining -= len(block)
            after = os.fstat(source.fileno())
            if source.read(1) or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns
            ):
                raise AuditError(f"{relative_path} changed during inspection")
    except (OSError, RuntimeError) as error:
        raise AuditError(f"unable to read {relative_path}") from error
    return {"path": relative_path, "bytes": before.st_size, "sha256": digest.hexdigest()}


def inspect_collection(root, collection_report):
    with tempfile.TemporaryDirectory(prefix="lixnu-tahoe-collection-") as directory:
        scratch = Path(directory)
        collection = scratch / "BootKernelExtensions.kc"
        with collection.open("xb") as destination:
            collection_report.update(snapshot(root, BOOT_COLLECTION, 1 << 30, destination))
        helper = scratch / "xnu-fileset-metadata"
        try:
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(REPO_ROOT / "tools/dev/x86_64/xnu-fileset-metadata.c"),
                str(REPO_ROOT / "xnu-ko/macho_parse_x86_64.c"), "-o", str(helper),
            ], check=True, stdout=subprocess.DEVNULL, timeout=120)
        except (OSError, subprocess.SubprocessError) as error:
            raise AuditError("unable to build the fileset inspector") from error
        try:
            result = subprocess.run([str(helper), str(collection)], check=True,
                                    capture_output=True, text=True, timeout=120)
            return json.loads(result.stdout)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            raise AuditError("unable to run the fileset inspector") from error


def decode_version_plist(data):
    try:
        # plistlib also rejects entity declarations before we check the XML envelope.
        product = plistlib.loads(data)
        if not data.startswith(b"bplist00"):
            envelope = ElementTree.fromstring(data)
            if envelope.tag != "plist" or len(envelope) != 1 or envelope[0].tag != "dict":
                raise ValueError("expected a single plist dictionary")
    except (ValueError, ExpatError, ElementTree.ParseError, AttributeError,
            IndexError, TypeError, OverflowError, RecursionError) as error:
        raise AuditError("invalid SystemVersion.plist") from error
    if not isinstance(product, dict):
        raise AuditError("SystemVersion.plist must contain a dictionary")
    return product


def audit(root, report):
    if not root.is_dir():
        raise AuditError("the supplied System volume root is not a directory")
    plist_bytes = io.BytesIO()
    report["system_version_plist"] = snapshot(root, VERSION_PLIST, 1 << 20, plist_bytes)
    product = decode_version_plist(plist_bytes.getvalue())
    for key in ("ProductVersion", "ProductBuildVersion"):
        if isinstance(product.get(key), str):
            report[key] = product[key]
    if (report.get("ProductVersion"), report.get("ProductBuildVersion")) != (TARGET_VERSION, TARGET_BUILD):
        raise AuditError(f"SystemVersion.plist must report macOS {TARGET_VERSION} ({TARGET_BUILD})")

    report["boot_kernel_collection"] = {}
    fileset = inspect_collection(root, report["boot_kernel_collection"])
    report["fileset"] = fileset
    if fileset.get("status") != "accepted" or fileset.get("parser_result") != 0:
        raise AuditError("the fileset inspector rejected the boot collection")
    if not fileset.get("kernel", {}).get("segments"):
        raise AuditError("the fileset inspector found no nested kernel segments")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system-root", type=Path, required=True,
                        help="root of the prepared macOS System volume mounted read-only")
    args = parser.parse_args()
    report = {"schema_version": 1, "source": "operator_supplied_system_volume",
              "apple_provenance_verified": False, "boot_verified": False}
    try:
        audit(args.system_root.resolve(), report)
    except AuditError as error:
        report.update(audit_status="rejected", error=str(error))
    except (OSError, RuntimeError):
        report.update(audit_status="rejected", error="unable to inspect the supplied System volume")
    else:
        report["audit_status"] = "accepted"
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["audit_status"] == "accepted" else 1


if __name__ == "__main__":
    raise SystemExit(main())
