#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the installed KDK identity against the pinned XNU build inputs."""

from __future__ import annotations

import argparse
import json
import pathlib
import plistlib
import re
import struct
import sys
import tempfile


class KDKError(ValueError):
    pass


def load_plist(path: pathlib.Path) -> dict[str, object]:
    try:
        with path.open("rb") as source:
            value = plistlib.load(source)
    except (OSError, plistlib.InvalidFileException) as error:
        raise KDKError(f"could not read KDK metadata plist {path}: {error}") from error
    if not isinstance(value, dict):
        raise KDKError(f"KDK metadata plist is not a dictionary: {path}")
    return value


def inspect_kdk(
    root: pathlib.Path,
    expected_version: str,
    expected_build: str,
    expected_xnu_version: str,
) -> dict[str, str]:
    try:
        canonical_root = root.resolve(strict=True)
    except OSError as error:
        raise KDKError(f"KDK path does not resolve: {root}: {error}") from error
    if not canonical_root.is_dir():
        raise KDKError(f"KDK path is not a directory: {canonical_root}")
    expected_name = f"KDK_{expected_version}_{expected_build}.kdk"
    if canonical_root.name != expected_name:
        raise KDKError(
            f"resolved KDK bundle is {canonical_root.name!r}; expected {expected_name!r}"
        )

    system_version = load_plist(
        canonical_root / "System/Library/CoreServices/SystemVersion.plist"
    )
    product_version = system_version.get("ProductVersion")
    product_build = system_version.get("ProductBuildVersion")
    if product_version != expected_version:
        raise KDKError(
            f"KDK ProductVersion is {product_version!r}; expected {expected_version!r}"
        )
    if product_build != expected_build:
        raise KDKError(
            f"KDK ProductBuildVersion is {product_build!r}; expected {expected_build!r}"
        )

    system_kext = load_plist(
        canonical_root / "System/Library/Extensions/System.kext/Info.plist"
    )
    kernel_path = canonical_root / "System/Library/Kernels/kernel"
    kernel = kernel_path.read_bytes()
    # The pinned KDK ships a thin MH_EXECUTE x86_64 release kernel.
    if (len(kernel) < 32
            or struct.unpack_from("<II", kernel) != (0xFEEDFACF, 0x01000007)
            or struct.unpack_from("<I", kernel, 12)[0] != 2):
        raise KDKError("KDK kernel is not a thin x86_64 Mach-O executable")
    versions = {
        (darwin.decode("ascii"), source.decode("ascii"))
        for darwin, source in re.findall(
            rb"Darwin Kernel Version ([0-9]+\.[0-9]+\.[0-9]+): [^\x00\r\n]{1,160}; "
            rb"root:xnu-([0-9]+\.[0-9]+\.[0-9]+)(?:~[0-9]+)?/RELEASE_X86_64\x00",
            kernel,
        )
    }
    if len(versions) != 1:
        raise KDKError("KDK Intel kernel does not contain one unambiguous XNU release identity")
    darwin_version, xnu_version = versions.pop()
    if xnu_version != expected_xnu_version:
        raise KDKError(f"KDK kernel XNU version is {xnu_version!r}; expected {expected_xnu_version!r}")
    if system_kext.get("CFBundleVersion") != darwin_version:
        raise KDKError("KDK System.kext CFBundleVersion does not match the kernel's Darwin version")
    header = canonical_root / (
        "System/Library/Frameworks/Kernel.framework/Versions/A/"
        "PrivateHeaders/platform/TrustCache/API.h"
    )
    if not header.is_file() or header.stat().st_size == 0:
        raise KDKError("KDK lacks the platform TrustCache/API.h")

    return {
        "root": str(canonical_root),
        "product_version": str(product_version),
        "product_build": str(product_build),
        "xnu_version": xnu_version,
        "trustcache_header": str(header),
    }


def _write_plist(path: pathlib.Path, data: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as target:
        plistlib.dump(data, target)


def self_test() -> None:
    expected_version = "26.5"
    expected_build = "25F71"
    expected_xnu_version = "12377.121.6"
    expected_name = f"KDK_{expected_version}_{expected_build}.kdk"
    with tempfile.TemporaryDirectory(prefix="xnu-kdk-check-") as temporary:
        root = pathlib.Path(temporary) / expected_name
        _write_plist(
            root / "System/Library/CoreServices/SystemVersion.plist",
            {"ProductVersion": expected_version, "ProductBuildVersion": expected_build},
        )
        xnu_info = root / "System/Library/Extensions/System.kext/Info.plist"
        _write_plist(xnu_info, {"CFBundleVersion": "25.5.0"})
        kernel_path = root / "System/Library/Kernels/kernel"
        kernel_path.parent.mkdir(parents=True)
        def image(version: str) -> bytes:
            return struct.pack("<8I", 0xFEEDFACF, 0x01000007, 3, 2, 0, 0, 0, 0) + (
                f"Darwin Kernel Version 25.5.0: date; root:xnu-{version}~2/RELEASE_X86_64\0"
            ).encode("ascii")
        kernel_path.write_bytes(image(expected_xnu_version))
        header = root / ("System/Library/Frameworks/Kernel.framework/Versions/A/"
                         "PrivateHeaders/platform/TrustCache/API.h")
        header.parent.mkdir(parents=True)
        header.write_text("test fixture", encoding="utf-8")
        metadata = inspect_kdk(root, expected_version, expected_build, expected_xnu_version)
        if metadata["product_build"] != expected_build:
            raise KDKError("matching KDK metadata fixture was not accepted")

        _write_plist(
            root / "System/Library/CoreServices/SystemVersion.plist",
            {"ProductVersion": expected_version, "ProductBuildVersion": "25F70"},
        )
        try:
            inspect_kdk(root, expected_version, expected_build, expected_xnu_version)
        except KDKError as error:
            if "ProductBuildVersion" not in str(error):
                raise
        else:
            raise KDKError("mismatched KDK build fixture was accepted")

        _write_plist(
            root / "System/Library/CoreServices/SystemVersion.plist",
            {"ProductVersion": "26.4", "ProductBuildVersion": expected_build},
        )
        try:
            inspect_kdk(root, expected_version, expected_build, expected_xnu_version)
        except KDKError as error:
            if "ProductVersion" not in str(error):
                raise
        else:
            raise KDKError("mismatched KDK product version fixture was accepted")

        _write_plist(
            root / "System/Library/CoreServices/SystemVersion.plist",
            {"ProductVersion": expected_version, "ProductBuildVersion": expected_build},
        )
        kernel_path.write_bytes(image("12377.121.5"))
        try:
            inspect_kdk(root, expected_version, expected_build, expected_xnu_version)
        except KDKError as error:
            if "kernel XNU version" not in str(error):
                raise
        else:
            raise KDKError("mismatched XNU version fixture was accepted")

        kernel_path.write_bytes(image(expected_xnu_version))
        _write_plist(xnu_info, {"CFBundleVersion": "25.5.1"})
        try:
            inspect_kdk(root, expected_version, expected_build, expected_xnu_version)
        except KDKError as error:
            if "CFBundleVersion" not in str(error):
                raise
        else:
            raise KDKError("mismatched Darwin version fixture was accepted")
        _write_plist(xnu_info, {"CFBundleVersion": "25.5.0"})
        header.unlink()
        try:
            inspect_kdk(root, expected_version, expected_build, expected_xnu_version)
        except KDKError as error:
            if "platform TrustCache/API.h" not in str(error):
                raise
        else:
            raise KDKError("missing platform TrustCache header fixture was accepted")

        wrong_target = pathlib.Path(temporary) / "KDK_26.5_25F70.kdk"
        wrong_target.mkdir()
        alias_parent = pathlib.Path(temporary) / "alias"
        alias_parent.mkdir()
        alias = alias_parent / expected_name
        alias.symlink_to(wrong_target, target_is_directory=True)
        try:
            inspect_kdk(alias, expected_version, expected_build, expected_xnu_version)
        except KDKError as error:
            if "resolved KDK bundle" not in str(error):
                raise
        else:
            raise KDKError("KDK symlink to a differently named bundle was accepted")

    print("KDK identity self-tests passed: bundle path, OS build, kernel XNU and Darwin versions, and platform TrustCache")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--kdk-root", type=pathlib.Path)
    parser.add_argument("--expected-version")
    parser.add_argument("--expected-build")
    parser.add_argument("--expected-xnu-version")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    required = (args.kdk_root, args.expected_version, args.expected_build, args.expected_xnu_version)
    if any(value is None for value in required):
        parser.error("inspection requires --kdk-root, --expected-version, --expected-build, and --expected-xnu-version")
    try:
        print(json.dumps(inspect_kdk(args.kdk_root, args.expected_version, args.expected_build, args.expected_xnu_version)))
    except (KDKError, OSError) as error:
        print(f"XNU KDK verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
