#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify a built x86_64 XNU Mach-O artifact without loading or executing it."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile


MAX_ARTIFACT_SIZE = 1 << 30
MAX_MANIFEST_SIZE = 4 << 20
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
SUPPORTED_SUBTYPES = {"X86_64_ALL", "X86_64_H"}
ENTRY_SOURCE = "LC_UNIXTHREAD.x86_THREAD_STATE64.RIP"
# The pinned XNU linker selects __start, defined in __HIB,__text by start.s.
ENTRY_SEGMENT = "__HIB"
HIB_ENTRY_SECTION = "__HIB,__text"
INSPECTOR_INVALID_IMAGE = 3
LAYOUT_PROFILE = "xnu-development-kernel-v1"
MANIFEST_CONTRACT = "build-xnu-kernel-result-v1"
XNU_SOURCE_PATCH = "scripts/x86_64/tahoe-xnu-cpuid-vendor-guard.patch"
LEGACY_XNU_SOURCE_PATCH = "xnu-patches/0015-x86-cpuid-vendor-early-refusal.patch"
XNU_COMPAT_PATCH = "scripts/x86_64/tahoe-xnu-pointer-casts.patch"
NON_INTEL_VENDOR_POLICY = "refuse-before-cpuid_do_precpuid_was-and-cpuid_set_generic_info"
EXPECTED_BUILD_IDENTITY = {
    "xnu_commit": "ac9718fb1af618d5ce8678d0dc6e8a58f252216f",
    "xnu_tag": "xnu-12377.121.6",
    "xnu_source_version": "12377.121.6",
    "xnu_source_release": "26.5",
    "sdk_version": "26.5",
    "xcode_version": "26.6",
    "runtime_target_os_version": "26.7.3",
    "runtime_target_os_build": "25G342",
    "platform": "MacOSX",
    "machine_config": "NONE",
}


class VerificationError(Exception):
    pass


def regular_file(path: pathlib.Path, label: str, maximum_size: int) -> pathlib.Path:
    try:
        st = path.lstat()
    except OSError as error:
        raise VerificationError(f"cannot stat {label} {str(path)!r}: {error}") from error
    if not stat.S_ISREG(st.st_mode):
        raise VerificationError(f"{label} must be a regular file, not a symlink or special file")
    if st.st_size <= 0 or st.st_size > maximum_size:
        raise VerificationError(f"{label} must be non-empty and no larger than {maximum_size} bytes")
    return path.absolute()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError(f"build-result.json contains duplicate key {key!r}")
        result[key] = value
    return result


def load_manifest(path: pathlib.Path) -> tuple[dict[str, object], str]:
    flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
        with os.fdopen(fd, "rb") as stream:
            st = os.fstat(stream.fileno())
            if not stat.S_ISREG(st.st_mode) or st.st_size <= 0 or st.st_size > MAX_MANIFEST_SIZE:
                raise VerificationError("build-result.json must be a non-empty regular file under the size limit")
            raw = stream.read(MAX_MANIFEST_SIZE + 1)
            if len(raw) > MAX_MANIFEST_SIZE:
                raise VerificationError("build-result.json exceeds the size limit")
    except OSError as error:
        raise VerificationError(f"cannot read build-result.json: {error}") from error
    try:
        manifest = json.loads(raw, object_pairs_hook=reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"build-result.json is not valid UTF-8 JSON: {error}") from error
    if not isinstance(manifest, dict):
        raise VerificationError("build-result.json must contain a JSON object")
    return manifest, hashlib.sha256(raw).hexdigest()


def require_string(manifest: dict[str, object], key: str, expected: str | None = None) -> str:
    value = manifest.get(key)
    if not isinstance(value, str) or not value:
        raise VerificationError(f"build-result.json field {key!r} must be a non-empty string")
    if expected is not None and value != expected:
        raise VerificationError(f"build-result.json field {key!r} must be {expected!r}, got {value!r}")
    return value


def verify_manifest(
    manifest: dict[str, object], artifact_sha256: str, artifact_size: int,
    repo_root: pathlib.Path,
) -> dict[str, object]:
    if type(manifest.get("build_result_schema_version")) is not int or manifest["build_result_schema_version"] != 1:
        raise VerificationError("build-result.json must declare build_result_schema_version=1")
    recorded_size = manifest.get("artifact_size_bytes")
    if type(recorded_size) is not int or recorded_size != artifact_size:
        raise VerificationError("kernel.development size does not match build-result.json artifact_size_bytes")
    require_string(manifest, "artifact_type", "XNU development kernel")
    require_string(manifest, "architecture", "x86_64")
    require_string(manifest, "target_architecture", "x86_64")

    identity = {
        key: require_string(manifest, key, expected)
        for key, expected in EXPECTED_BUILD_IDENTITY.items()
    }

    if manifest.get("xnu_source_modified") is not True:
        raise VerificationError("build-result.json must attest that the pinned XNU patch was applied")
    source_patch = require_string(manifest, "xnu_source_patch")
    if source_patch not in {XNU_SOURCE_PATCH, LEGACY_XNU_SOURCE_PATCH}:
        raise VerificationError("build-result.json does not name the pinned XNU vendor guard patch")
    vendor_policy = require_string(manifest, "xnu_non_intel_vendor_policy", NON_INTEL_VENDOR_POLICY)
    source_patch_sha256 = require_string(manifest, "xnu_source_patch_sha256")
    compat_patch_sha256 = require_string(manifest, "xnu_source_compat_patch_sha256")
    if not SHA256_RE.fullmatch(source_patch_sha256):
        raise VerificationError("build-result.json xnu_source_patch_sha256 must be a 64-digit SHA-256")
    if not SHA256_RE.fullmatch(compat_patch_sha256):
        raise VerificationError("build-result.json xnu_source_compat_patch_sha256 must be a 64-digit SHA-256")
    # Both manifest paths identify the same checked-out patch bytes.
    expected_source_patch_sha256 = sha256_file(repo_root / XNU_SOURCE_PATCH)
    if source_patch_sha256.lower() != expected_source_patch_sha256:
        raise VerificationError("XNU source patch SHA-256 does not match the checked-out patch")
    expected_compat_patch_sha256 = sha256_file(repo_root / XNU_COMPAT_PATCH)
    if compat_patch_sha256.lower() != expected_compat_patch_sha256:
        raise VerificationError("XNU compatibility patch SHA-256 does not match the checked-out patch")

    host_identity = {
        "build_host_arch": require_string(manifest, "build_host_arch", "x86_64"),
        "build_host_mode": require_string(manifest, "build_host_mode", "native"),
        "build_process_arch": require_string(manifest, "build_process_arch", "x86_64"),
    }
    if manifest.get("build_process_translated") is not False:
        raise VerificationError("build-result.json must attest build_process_translated=false")

    recorded_hash = require_string(manifest, "artifact_sha256")
    if not SHA256_RE.fullmatch(recorded_hash):
        raise VerificationError("build-result.json artifact_sha256 must be a 64-digit SHA-256")
    if recorded_hash.lower() != artifact_sha256:
        raise VerificationError("kernel.development SHA-256 does not match build-result.json")

    for key in (
        "kernel_collection_built",
        "boot_verified",
        "runtime_compatibility_verified",
        "kernel_source_runtime_match",
    ):
        if manifest.get(key) is not False:
            raise VerificationError(
                f"build-result.json must keep {key}=false; this verifier proves structure only"
            )

    return {
        "manifest_contract": MANIFEST_CONTRACT,
        "build_result_schema_version": 1,
        "artifact_size_bytes": artifact_size,
        **identity,
        "xnu_source_modified": True,
        "xnu_source_patch": source_patch,
        "xnu_source_patch_sha256": expected_source_patch_sha256,
        "xnu_source_compat_patch_sha256": expected_compat_patch_sha256,
        "xnu_non_intel_vendor_policy": vendor_policy,
        **host_identity,
        "build_process_translated": False,
        "artifact_type": "XNU development kernel",
        "architecture": "x86_64",
        "kernel_collection_built": False,
        "boot_verified": False,
        "runtime_compatibility_verified": False,
        "kernel_source_runtime_match": False,
    }


def parse_hex(value: object, field: str) -> int:
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value):
        raise VerificationError(f"inspector field {field!r} is not a 64-bit hexadecimal address")
    return int(value, 16)


def verify_inspection(
    report: object, artifact_name: str, artifact_size: int, expected_subtype: str,
    *, hib_metadata: bool,
) -> dict[str, object]:
    if not isinstance(report, dict):
        raise VerificationError("xnu-macho-inspect did not return a JSON object")
    if report.get("file") != artifact_name:
        raise VerificationError("inspector report does not identify the input artifact by name")
    if report.get("file_size_bytes") != artifact_size:
        raise VerificationError("inspector report size does not match kernel.development")
    if report.get("format") != "Mach-O 64-bit little-endian MH_EXECUTE":
        raise VerificationError("artifact is not a 64-bit little-endian MH_EXECUTE image")
    if report.get("architecture") != "x86_64":
        raise VerificationError("artifact does not have CPU_TYPE_X86_64")
    subtype = report.get("cpu_subtype")
    if subtype != expected_subtype:
        raise VerificationError(
            f"CPU subtype {subtype!r} does not match the selected policy {expected_subtype!r}"
        )

    entry = report.get("entry")
    if not isinstance(entry, dict) or entry.get("source") != ENTRY_SOURCE:
        raise VerificationError("artifact lacks the required x86_THREAD_STATE64 thread entry")
    entry_address = parse_hex(entry.get("unslid"), "entry.unslid")

    segments = report.get("segments")
    if not isinstance(segments, list):
        raise VerificationError("inspector report has no segment list")
    segments_by_name = {}
    for index, segment in enumerate(segments):
        if not isinstance(segment, dict):
            raise VerificationError("inspector report contains a malformed segment")
        name = segment.get("name")
        if not isinstance(name, str) or not name:
            raise VerificationError("inspector report contains a segment without a name")
        if name in segments_by_name:
            raise VerificationError("XNU development layout contains duplicate segment names")
        if name == "__PAGEZERO" and index != 0:
            raise VerificationError("XNU development layout permits one leading __PAGEZERO segment")
        segments_by_name[name] = segment
    text_segment = segments_by_name.get("__TEXT")
    if text_segment is None:
        raise VerificationError("XNU development layout must contain exactly one __TEXT segment")
    if report.get("segment_count") != len(segments):
        raise VerificationError("inspector segment_count does not match the segment list")
    if report.get("mapped_segment_count") != sum(
        segment.get("mapped") is True for segment in segments if isinstance(segment, dict)
    ):
        raise VerificationError("inspector mapped_segment_count does not match the segment list")

    layout_observed = {
        key: text_segment.get(key)
        for key in ("vmaddr", "vmsize", "fileoff", "filesize", "initprot", "maxprot", "nsects", "mapped")
    }
    if (
        text_segment.get("mapped") is not True
        or text_segment.get("fileoff") != "0x0000000000000000"
        or text_segment.get("filesize") in (None, "0x0000000000000000")
        or text_segment.get("initprot") != "r-x"
        or text_segment.get("maxprot") != "r-x"
        or not isinstance(text_segment.get("nsects"), int)
        or text_segment.get("nsects", 0) < 1
    ):
        raise VerificationError(
            "XNU development layout requires one file-backed, sectioned, read/execute-only "
            f"__TEXT at file offset zero; observed {layout_observed!r}"
        )
    text_vmsize = parse_hex(text_segment.get("vmsize"), "__TEXT.vmsize")
    text_filesize = parse_hex(text_segment.get("filesize"), "__TEXT.filesize")
    if text_vmsize == 0 or text_filesize > text_vmsize:
        raise VerificationError(
            "__TEXT must have a non-empty file-backed range within its VM range; "
            f"observed {layout_observed!r}"
        )
    entry_segment = segments_by_name.get(ENTRY_SEGMENT)
    protections = ("rw-",) if hib_metadata else ("r-x", "rwx")
    if (
        entry_segment is None
        or entry_segment.get("mapped") is not True
        or entry_segment.get("initprot") not in protections
        or (hib_metadata and entry_segment.get("maxprot") != "rw-")
        or not isinstance(entry_segment.get("nsects"), int)
        or entry_segment.get("nsects", 0) < 1
    ):
        raise VerificationError(
            f"XNU entry requires a mapped, sectioned {ENTRY_SEGMENT} with protections {protections}"
        )
    entry_vmaddr = parse_hex(entry_segment.get("vmaddr"), f"{ENTRY_SEGMENT}.vmaddr")
    entry_vmsize = parse_hex(entry_segment.get("vmsize"), f"{ENTRY_SEGMENT}.vmsize")
    entry_filesize = parse_hex(entry_segment.get("filesize"), f"{ENTRY_SEGMENT}.filesize")
    entry_delta = entry_address - entry_vmaddr
    if not 0 <= entry_delta < min(entry_vmsize, entry_filesize):
        raise VerificationError(f"thread RIP is not inside the file-backed {ENTRY_SEGMENT} segment")

    if hib_metadata:
        hib_entry = report.get("hib_entry")
        if not isinstance(hib_entry, dict) or hib_entry.get("section") != HIB_ENTRY_SECTION:
            raise VerificationError(f"inspector did not identify the {HIB_ENTRY_SECTION} entry section")
        file_offset = parse_hex(hib_entry.get("file_offset"), "hib_entry.file_offset")
        segment_offset = parse_hex(entry_segment.get("fileoff"), f"{ENTRY_SEGMENT}.fileoff")
        if file_offset != segment_offset + entry_delta or file_offset >= artifact_size:
            raise VerificationError(f"invalid file offset for the {HIB_ENTRY_SECTION} entry")
    elif "hib_entry" in report:
        raise VerificationError("standard inspection unexpectedly reported hibernation metadata")

    if report.get("unclassified_optional_commands") != 0:
        raise VerificationError(
            "artifact has unclassified optional load commands; structural review is incomplete"
        )

    if report.get("load_status") != "not_loaded":
        raise VerificationError("inspector report must state that the image was not loaded")
    if report.get("fixups_interpreted") is not False:
        raise VerificationError("inspector report must state that fixups were not interpreted")
    if report.get("execution_attempted") is not False:
        raise VerificationError("inspector report must state that execution was not attempted")
    return report


def run_inspector(
    inspector: pathlib.Path, artifact: pathlib.Path, *options: str,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            [str(inspector), *options, artifact.name],
            cwd=artifact.parent,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=300,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(f"could not run xnu-macho-inspect: {error}") from error


def inspect_artifact(
    inspector: pathlib.Path, artifact: pathlib.Path, size: int, expected_subtype: str
) -> dict[str, object]:
    result = run_inspector(inspector, artifact)
    # The normal parser refuses an entry in RW __HIB. Try the dedicated
    # userspace metadata mode after a parse rejection, without changing that parser.
    hib_metadata = result.returncode == INSPECTOR_INVALID_IMAGE
    standard_diagnostic = result.stderr.strip()[:2000] if hib_metadata else ""
    if hib_metadata:
        result = run_inspector(inspector, artifact, "--hib-entry-metadata")
    if result.returncode != 0:
        diagnostic = result.stderr.strip()[:4000]
        if hib_metadata:
            raise VerificationError(
                f"xnu-macho-inspect rejected kernel.development in standard mode: {standard_diagnostic}; "
                f"HIB metadata mode (status {result.returncode}): {diagnostic or 'no diagnostic'}"
            )
        raise VerificationError(
            f"xnu-macho-inspect rejected kernel.development (status {result.returncode}): "
            f"{diagnostic or 'no diagnostic'}"
        )
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise VerificationError(f"xnu-macho-inspect returned invalid JSON: {error}") from error
    return verify_inspection(report, artifact.name, size, expected_subtype, hib_metadata=hib_metadata)


def snapshot_artifact(source: pathlib.Path, destination: pathlib.Path) -> tuple[int, str]:
    flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0)
    try:
        source_fd = os.open(source, flags)
    except OSError as error:
        raise VerificationError(f"cannot open kernel.development without following links: {error}") from error
    digest = hashlib.sha256()
    size = 0
    try:
        before = os.fstat(source_fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size > MAX_ARTIFACT_SIZE:
            raise VerificationError("kernel.development must be a non-empty regular file under the size limit")
        with destination.open("xb") as target:
            while True:
                chunk = os.read(source_fd, 1024 * 1024)
                if not chunk:
                    break
                next_size = size + len(chunk)
                if next_size > before.st_size:
                    raise VerificationError(
                        "kernel.development grew beyond its initial size while its private snapshot was created"
                    )
                size = next_size
                digest.update(chunk)
                target.write(chunk)
            target.flush()
            os.fsync(target.fileno())
            os.fchmod(target.fileno(), 0o400)
        after = os.fstat(source_fd)
        if (
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
            or size != before.st_size
        ):
            raise VerificationError("kernel.development changed while its private snapshot was created")
    except OSError as error:
        raise VerificationError(f"cannot create private kernel.development snapshot: {error}") from error
    finally:
        os.close(source_fd)
    return size, digest.hexdigest()


def compile_trusted_inspector(repo_root: pathlib.Path, build_dir: pathlib.Path) -> pathlib.Path:
    compiler = shutil.which("cc")
    if compiler is None:
        raise VerificationError("C compiler 'cc' is required to build the trusted Mach-O inspector")
    output = build_dir / "xnu-macho-inspect"
    sources = (
        repo_root / "tools/dev/x86_64/xnu-macho-inspect.c",
        repo_root / "xnu-ko/macho_parse_x86_64.c",
    )
    try:
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", *(str(s) for s in sources), "-o", str(output)],
            cwd=repo_root,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=300,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(f"could not compile trusted xnu-macho-inspect: {error}") from error
    if result.returncode != 0:
        raise VerificationError(
            "could not compile trusted xnu-macho-inspect: " + result.stderr.strip()[:4000]
        )
    return output


def hash_snapshot(snapshot: pathlib.Path) -> str:
    return sha256_file(snapshot)


def ensure_output_is_separate(output: pathlib.Path, *inputs: pathlib.Path) -> pathlib.Path:
    if output.is_symlink():
        raise VerificationError("report output must not be a symlink")
    for input_path in inputs:
        try:
            if output.exists() and os.path.samefile(output, input_path):
                raise VerificationError("report output must not overwrite an input")
        except OSError as error:
            raise VerificationError(f"cannot compare report output with input: {error}") from error
        if output.resolve(strict=False) == input_path.resolve(strict=False):
            raise VerificationError("report output must not overwrite an input")
    if not output.parent.is_dir():
        raise VerificationError("report output parent directory must already exist")
    if output.exists():
        raise VerificationError("report output already exists; refusing to overwrite it")
    return output


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Read-only structural verification for kernel.development. "
            "This never loads or executes XNU and does not validate runtime compatibility."
        )
    )
    parser.add_argument("--output", required=True, type=pathlib.Path,
                        help="new JSON report file to create")
    parser.add_argument(
        "--expected-subtype",
        required=True,
        choices=sorted(SUPPORTED_SUBTYPES),
        help="pinned CPU subtype policy for this artifact (X86_64_ALL or X86_64_H)",
    )
    parser.add_argument("kernel_development", type=pathlib.Path)
    parser.add_argument("build_result_json", type=pathlib.Path)
    args = parser.parse_args(argv)

    try:
        artifact = regular_file(args.kernel_development, "kernel.development", MAX_ARTIFACT_SIZE)
        manifest_path = regular_file(args.build_result_json, "build-result.json", MAX_MANIFEST_SIZE)
        output = ensure_output_is_separate(args.output, artifact, manifest_path)
        manifest, manifest_sha256 = load_manifest(manifest_path)
        repo_root = pathlib.Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory(prefix="xnu-artifact-preflight-") as temporary:
            temporary_dir = pathlib.Path(temporary)
            snapshot = temporary_dir / "kernel.development"
            artifact_size, artifact_sha256 = snapshot_artifact(artifact, snapshot)
            build_metadata = verify_manifest(manifest, artifact_sha256, artifact_size, repo_root)
            inspector = compile_trusted_inspector(repo_root, temporary_dir)
            inspection = inspect_artifact(
                inspector, snapshot, artifact_size, args.expected_subtype
            )
            if hash_snapshot(snapshot) != artifact_sha256:
                raise VerificationError("private kernel.development snapshot changed during inspection")

        report = {
            "schema": "xnulinux-xnu-artifact-inspection-v1",
            "artifact": {
                "name": artifact.name,
                "size_bytes": artifact_size,
                "sha256": artifact_sha256,
            },
            "build_result": {
                "name": manifest_path.name,
                "sha256": manifest_sha256,
                "path_used_for_verification": False,
                "metadata": build_metadata,
            },
            "inspection": inspection,
            "validation_scope": {
                "layout_profile": LAYOUT_PROFILE,
                "expected_cpu_subtype": args.expected_subtype,
                "structural_macho_validation": True,
                "unclassified_optional_commands": 0,
                "entry_is_file_backed": True,
                "entry_is_file_backed_executable": "hib_entry" not in inspection,
                "entry_segment": ENTRY_SEGMENT,
                "load_attempted": False,
                "fixups_applied": False,
                "execution_attempted": False,
                "runtime_compatibility_verified": False,
                "boot_verified": False,
            },
        }
        serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
        try:
            fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                stream.write(serialized)
        except OSError as error:
            raise VerificationError(f"cannot create report {str(output)!r}: {error}") from error
        print(f"wrote read-only artifact inspection report: {output}")
        return 0
    except VerificationError as error:
        print(f"verify-xnu-artifact: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
