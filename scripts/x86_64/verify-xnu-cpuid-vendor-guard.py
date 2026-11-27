#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Apply or verify the early Intel-vendor refusal in the pinned XNU source."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys


XNU_COMMIT = "ac9718fb1af618d5ce8678d0dc6e8a58f252216f"
SOURCE_FILE = "osfmk/i386/cpuid.c"
EXPECTED_STATUS = f" M {SOURCE_FILE}\n".encode()


class GuardError(Exception):
    pass


def git(source_root: pathlib.Path, *args: str) -> bytes:
    try:
        result = subprocess.run(
            ["git", "-C", str(source_root), *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", b"")
        raise GuardError(f"git {' '.join(args)} failed: {detail.decode(errors='replace')}") from error
    return result.stdout


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"^{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise GuardError(f"pinned source is missing definition of {name}()")

    opening = source.find("{", match.start(), match.end())
    depth = 0
    index = opening
    state = "code"
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and next_char == "*":
                state = "block-comment"
                index += 1
            elif char == "/" and next_char == "/":
                state = "line-comment"
                index += 1
            elif char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[opening + 1:index]
        elif state == "block-comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state in {"string", "character"}:
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "code"
        index += 1

    raise GuardError(f"definition of {name}() has unbalanced braces")


def verify_order(source_root: pathlib.Path) -> None:
    source_path = source_root / SOURCE_FILE
    if not source_path.is_file():
        raise GuardError(f"pinned XNU source is missing {SOURCE_FILE}")
    source = source_path.read_text(encoding="utf-8")

    vendor = function_body(source, "cpuid_vendor_is_intel")
    expected_vendor_steps = (
        "cpuid_fn(0, reg);",
        "bcopy(&reg[ebx], &vendor[0], 4);",
        "bcopy(&reg[edx], &vendor[4], 4);",
        "bcopy(&reg[ecx], &vendor[8], 4);",
        "vendor[sizeof(vendor) - 1] = '\\0';",
        "strncmp(CPUID_VID_INTEL, vendor, sizeof(vendor)) == 0",
    )
    positions = [vendor.find(step) for step in expected_vendor_steps]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise GuardError("vendor helper must assemble leaf-0 EBX/EDX/ECX and accept only GenuineIntel")

    set_info = function_body(source, "cpuid_set_info")
    gate = set_info.find("if (!cpuid_vendor_is_intel())")
    pre_workaround = set_info.find("cpuid_do_precpuid_was();")
    generic_info = set_info.find("cpuid_set_generic_info(info_p);")
    refusal = set_info.find('panic("Unsupported CPU");', gate)
    if min(gate, pre_workaround, generic_info, refusal) < 0 or not gate < refusal < pre_workaround < generic_info:
        raise GuardError("non-Intel vendor refusal must precede cpuid_do_precpuid_was() and cpuid_set_generic_info()")
    if "CPUID_VID_AMD" in vendor or "CPUID_VID_AMD" in set_info:
        raise GuardError("the guard must not admit or special-case an AMD vendor")
    if "cpuid_set_cpufamily(info_p) == CPUFAMILY_UNKNOWN" not in set_info:
        raise GuardError("the existing Intel CPU-family fail-closed check must remain")

    precpuid = function_body(source, "cpuid_do_precpuid_was")
    generic = function_body(source, "cpuid_set_generic_info")
    if "wrmsr64(MSR_IA32_TSX_CTRL" not in precpuid:
        raise GuardError("pinned pre-CPUID Intel MSR path changed; re-audit this guard")
    if "wrmsr64(MSR_IA32_BIOS_SIGN_ID" not in generic or "rdmsr64(MSR_IA32_PLATFORM_ID" not in generic:
        raise GuardError("pinned generic Intel MSR path changed; re-audit this guard")


def expected_patch_state(source_root: pathlib.Path, patch_bytes: bytes) -> None:
    status = git(source_root, "status", "--porcelain", "--untracked-files=all")
    diff = git(source_root, "diff", "--binary", "--no-ext-diff", "--", SOURCE_FILE)
    if status != EXPECTED_STATUS or diff != patch_bytes:
        raise GuardError("XNU source tree must contain exactly the committed vendor-guard patch and no other changes")


def apply_or_verify(source_root: pathlib.Path, patch_path: pathlib.Path, apply: bool) -> str:
    patch_path = patch_path.resolve()
    if git(source_root, "rev-parse", "HEAD").decode().strip() != XNU_COMMIT:
        raise GuardError(f"XNU checkout must be exactly {XNU_COMMIT}")
    patch_bytes = patch_path.read_bytes()
    status = git(source_root, "status", "--porcelain", "--untracked-files=all")
    diff = git(source_root, "diff", "--binary", "--no-ext-diff", "--", SOURCE_FILE)

    if apply and not status and not diff:
        subprocess.run(
            ["git", "-C", str(source_root), "apply", "--check", "--whitespace=error", str(patch_path)],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(source_root), "apply", "--whitespace=error", str(patch_path)],
            check=True,
        )
    expected_patch_state(source_root, patch_bytes)
    verify_order(source_root)
    return hashlib.sha256(patch_bytes).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--patch", type=pathlib.Path, required=True)
    parser.add_argument("--apply", action="store_true", help="apply the patch if the pinned checkout is pristine")
    args = parser.parse_args()

    try:
        digest = apply_or_verify(args.source_root, args.patch, args.apply)
    except (GuardError, OSError, subprocess.CalledProcessError) as error:
        print(f"XNU CPUID vendor guard: {error}", file=sys.stderr)
        return 1
    print(f"XNU CPUID vendor guard order verified; patch sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
