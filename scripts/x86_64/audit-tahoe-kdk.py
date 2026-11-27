#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Identify the Intel development kernel extracted from the Tahoe target KDK."""

import argparse
import hashlib
import json
import pathlib
import plistlib
import re
import struct

PRODUCT_VERSION = "26.7.3"
PRODUCT_BUILD = "25G342"
DARWIN_VERSION = b"25.6.0"
XNU_VERSION = b"12377.161.15.701.48~2"
BUNDLE_NAME = f"KDK_{PRODUCT_VERSION}_{PRODUCT_BUILD}.kdk"
KERNEL_IDENTITY = re.compile(
    rb"Darwin Kernel Version ([0-9]+\.[0-9]+\.[0-9]+): [^\x00\r\n]{1,160}; "
    rb"root:xnu-([0-9]+(?:\.[0-9]+){2,}(?:~[0-9]+)?)/DEVELOPMENT_X86_64\x00"
)


def inspect(bundle):
    if bundle.name != BUNDLE_NAME:
        raise ValueError(f"expected bundle {BUNDLE_NAME}")
    library = bundle / "System/Library"
    with (library / "CoreServices/SystemVersion.plist").open("rb") as source:
        version = plistlib.load(source)
    if not isinstance(version, dict) or (
        version.get("ProductVersion"), version.get("ProductBuildVersion")
    ) != (PRODUCT_VERSION, PRODUCT_BUILD):
        raise ValueError("KDK SystemVersion.plist does not identify the target")

    kernel = (library / "Kernels/kernel.development").read_bytes()
    if len(kernel) < 32 or struct.unpack_from("<II", kernel) != (0xFEEDFACF, 0x01000007):
        raise ValueError("KDK development kernel is not a thin x86_64 Mach-O")
    if struct.unpack_from("<I", kernel, 12)[0] != 2:
        raise ValueError("KDK development kernel is not MH_EXECUTE")
    if set(KERNEL_IDENTITY.findall(kernel)) != {(DARWIN_VERSION, XNU_VERSION)}:
        raise ValueError("KDK development kernel does not have the expected Intel XNU identity")

    return {
        "product_version": PRODUCT_VERSION,
        "product_build": PRODUCT_BUILD,
        "darwin_version": DARWIN_VERSION.decode("ascii"),
        "xnu_version": XNU_VERSION.decode("ascii"),
        "architecture": "x86_64",
        "flavor": "DEVELOPMENT",
        "mach_o_file_type": "MH_EXECUTE",
        "kernel_bytes": len(kernel),
        "kernel_sha256": hashlib.sha256(kernel).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=pathlib.Path)
    parser.add_argument("report", type=pathlib.Path)
    args = parser.parse_args()
    try:
        report = json.dumps(inspect(args.bundle), indent=2) + "\n"
        args.report.write_text(report, encoding="utf-8")
        print(report, end="")
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        parser.exit(1, f"KDK metadata audit: {error}\n")


if __name__ == "__main__":
    main()
