#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic accepted/rejected cases for the XNU artifact verifier."""

import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


FILE_SIZE = 0x600
HEADER_SIZE = 32
PAGEZERO_SIZE = 72
TEXT_SIZE = 72 + 80
THREAD_SIZE = 8 + 8 + 42 * 4
COMMAND_BYTES = PAGEZERO_SIZE + 2 * TEXT_SIZE + THREAD_SIZE
PAGEZERO_OFF = HEADER_SIZE
TEXT_OFF = PAGEZERO_OFF + PAGEZERO_SIZE
HIB_OFF = TEXT_OFF + TEXT_SIZE
THREAD_OFF = HIB_OFF + TEXT_SIZE
RIP_OFF = THREAD_OFF + 8 + 8 + 16 * 8

MH_EXECUTE = 2
CPU_TYPE_X86_64 = 0x01000007
CPU_SUBTYPE_X86_64_ALL = 3
CPU_SUBTYPE_X86_64_H = 8
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_OPTIONAL_UNKNOWN = 0x77
X86_THREAD_STATE64 = 4
X86_THREAD_STATE64_COUNT = 42
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
XNU_SOURCE_PATCH = "xnu-patches/0015-x86-cpuid-vendor-early-refusal.patch"
XNU_COMPAT_PATCH = "scripts/x86_64/tahoe-xnu-pointer-casts.patch"
NON_INTEL_VENDOR_POLICY = "refuse-before-cpuid_do_precpuid_was-and-cpuid_set_generic_info"


def put_u32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def put_u64(data, offset, value):
    struct.pack_into("<Q", data, offset, value)


def put_name(data, offset, value):
    encoded = value.encode("ascii")
    data[offset : offset + 16] = encoded.ljust(16, b"\0")


def make_valid_image(subtype=CPU_SUBTYPE_X86_64_ALL):
    data = bytearray(FILE_SIZE)
    for index, value in enumerate(
        (
            0xFEEDFACF,
            CPU_TYPE_X86_64,
            subtype,
            MH_EXECUTE,
            4,
            COMMAND_BYTES,
            0,
            0,
        )
    ):
        put_u32(data, index * 4, value)

    put_u32(data, PAGEZERO_OFF, LC_SEGMENT_64)
    put_u32(data, PAGEZERO_OFF + 4, PAGEZERO_SIZE)
    put_name(data, PAGEZERO_OFF + 8, "__PAGEZERO")
    put_u64(data, PAGEZERO_OFF + 24, 0)
    put_u64(data, PAGEZERO_OFF + 32, 0x1000)

    put_u32(data, TEXT_OFF, LC_SEGMENT_64)
    put_u32(data, TEXT_OFF + 4, TEXT_SIZE)
    put_name(data, TEXT_OFF + 8, "__TEXT")
    put_u64(data, TEXT_OFF + 24, 0x4000)
    put_u64(data, TEXT_OFF + 32, 0x2000)
    put_u64(data, TEXT_OFF + 40, 0)
    put_u64(data, TEXT_OFF + 48, 0x400)
    put_u32(data, TEXT_OFF + 56, 5)
    put_u32(data, TEXT_OFF + 60, 5)
    put_u32(data, TEXT_OFF + 64, 1)
    put_u32(data, TEXT_OFF + 68, 0)

    put_name(data, TEXT_OFF + 72, "__text")
    put_name(data, TEXT_OFF + 88, "__TEXT")
    put_u64(data, TEXT_OFF + 104, 0x4300)
    put_u64(data, TEXT_OFF + 112, 0x20)
    put_u32(data, TEXT_OFF + 120, 0x300)
    put_u32(data, TEXT_OFF + 124, 4)

    # XNU places the startup code and bootstrap data in __HIB below __TEXT.
    put_u32(data, HIB_OFF, LC_SEGMENT_64)
    put_u32(data, HIB_OFF + 4, TEXT_SIZE)
    put_name(data, HIB_OFF + 8, "__HIB")
    put_u64(data, HIB_OFF + 24, 0x1000)
    put_u64(data, HIB_OFF + 32, 0x2000)
    put_u64(data, HIB_OFF + 40, 0x400)
    put_u64(data, HIB_OFF + 48, 0x200)
    put_u32(data, HIB_OFF + 56, 7)
    put_u32(data, HIB_OFF + 60, 7)
    put_u32(data, HIB_OFF + 64, 1)
    put_name(data, HIB_OFF + 72, "__text")
    put_name(data, HIB_OFF + 88, "__HIB")
    put_u64(data, HIB_OFF + 104, 0x1100)
    put_u64(data, HIB_OFF + 112, 0x20)
    put_u32(data, HIB_OFF + 120, 0x500)
    put_u32(data, HIB_OFF + 124, 4)

    put_u32(data, THREAD_OFF, LC_UNIXTHREAD)
    put_u32(data, THREAD_OFF + 4, THREAD_SIZE)
    put_u32(data, THREAD_OFF + 8, X86_THREAD_STATE64)
    put_u32(data, THREAD_OFF + 12, X86_THREAD_STATE64_COUNT)
    put_u64(data, RIP_OFF, 0x1100)
    return data


def build_manifest(image):
    source_patch_hash = hashlib.sha256((REPO_ROOT / XNU_SOURCE_PATCH).read_bytes()).hexdigest()
    compat_patch_hash = hashlib.sha256((REPO_ROOT / XNU_COMPAT_PATCH).read_bytes()).hexdigest()
    return {
        "build_result_schema_version": 1,
        "artifact": "/stale/runner/path/kernel.development",
        "artifact_sha256": hashlib.sha256(image).hexdigest(),
        "artifact_size_bytes": len(image),
        "artifact_type": "XNU development kernel",
        "architecture": "x86_64",
        "target_architecture": "x86_64",
        "xnu_commit": "ac9718fb1af618d5ce8678d0dc6e8a58f252216f",
        "xnu_tag": "xnu-12377.121.6",
        "xnu_source_version": "12377.121.6",
        "xnu_source_release": "26.5",
        "sdk_version": "26.5",
        "xcode_version": "26.6",
        "platform": "MacOSX",
        "machine_config": "NONE",
        "xnu_source_modified": True,
        "xnu_source_patch": XNU_SOURCE_PATCH,
        "xnu_source_patch_sha256": source_patch_hash,
        "xnu_source_compat_patch_sha256": compat_patch_hash,
        "xnu_non_intel_vendor_policy": NON_INTEL_VENDOR_POLICY,
        "build_host_arch": "x86_64",
        "build_host_mode": "native",
        "build_process_arch": "x86_64",
        "build_process_translated": False,
        "runtime_target_os_version": "26.7.3",
        "runtime_target_os_build": "25G342",
        "kernel_collection_built": False,
        "boot_verified": False,
        "runtime_compatibility_verified": False,
        "kernel_source_runtime_match": False,
    }


def run_case(verifier, name, image, expected_success, mutate_manifest=None,
             output_alias=False, expected_subtype="X86_64_ALL", policy_subtype=None,
             expected_diagnostic=None):
    with tempfile.TemporaryDirectory(prefix="xnu-artifact-verifier-") as temp:
        root = pathlib.Path(temp)
        artifact = root / "kernel.development"
        manifest_path = root / "build-result.json"
        output_path = artifact if output_alias else root / "artifact-report.json"
        artifact.write_bytes(image)
        manifest = build_manifest(image)
        if mutate_manifest:
            mutate_manifest(manifest)
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        artifact_before = hashlib.sha256(artifact.read_bytes()).digest()
        manifest_before = hashlib.sha256(manifest_path.read_bytes()).digest()

        result = subprocess.run(
            [
                sys.executable,
                str(verifier),
                "--output",
                str(output_path),
                "--expected-subtype",
                policy_subtype or expected_subtype,
                str(artifact),
                str(manifest_path),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        expected_status = 0 if expected_success else 1
        if result.returncode != expected_status:
            raise AssertionError(
                f"{name}: exit {result.returncode}, expected {expected_status}; "
                f"stdout={result.stdout!r}; stderr={result.stderr!r}"
            )
        if hashlib.sha256(artifact.read_bytes()).digest() != artifact_before:
            raise AssertionError(f"{name}: kernel.development was modified")
        if hashlib.sha256(manifest_path.read_bytes()).digest() != manifest_before:
            raise AssertionError(f"{name}: build-result.json was modified")

        if not expected_success:
            if not result.stderr:
                raise AssertionError(f"{name}: rejection had no diagnostic")
            if expected_diagnostic and expected_diagnostic not in result.stderr:
                raise AssertionError(
                    f"{name}: expected diagnostic {expected_diagnostic!r}, got {result.stderr!r}"
                )
            if output_alias:
                if output_path.read_bytes() != image:
                    raise AssertionError(f"{name}: output alias changed kernel.development")
            elif output_path.exists():
                raise AssertionError(f"{name}: rejection left a partial report")
            return

        report = json.loads(output_path.read_text(encoding="utf-8"))
        if report["schema"] != "xnulinux-xnu-artifact-inspection-v1":
            raise AssertionError(f"{name}: unexpected report schema")
        if report["artifact"]["name"] != "kernel.development":
            raise AssertionError(f"{name}: artifact name was not recorded")
        if report["artifact"]["sha256"] != hashlib.sha256(image).hexdigest():
            raise AssertionError(f"{name}: report omitted the verified artifact SHA-256")
        if report["artifact"]["size_bytes"] != len(image):
            raise AssertionError(f"{name}: report omitted the verified artifact size")
        if report["build_result"]["metadata"]["build_result_schema_version"] != 1:
            raise AssertionError(f"{name}: manifest schema version was not recorded")
        if report["build_result"]["path_used_for_verification"] is not False:
            raise AssertionError(f"{name}: stale manifest artifact path was trusted")
        if report["build_result"]["metadata"]["runtime_target_os_version"] != "26.7.3":
            raise AssertionError(f"{name}: runtime target metadata was not preserved")
        metadata = report["build_result"]["metadata"]
        expected_provenance = {
            "xnu_source_modified": True,
            "xnu_source_patch": XNU_SOURCE_PATCH,
            "xnu_source_patch_sha256": hashlib.sha256((REPO_ROOT / XNU_SOURCE_PATCH).read_bytes()).hexdigest(),
            "xnu_source_compat_patch_sha256": hashlib.sha256((REPO_ROOT / XNU_COMPAT_PATCH).read_bytes()).hexdigest(),
            "xnu_non_intel_vendor_policy": NON_INTEL_VENDOR_POLICY,
            "build_host_arch": "x86_64",
            "build_host_mode": "native",
            "build_process_arch": "x86_64",
            "build_process_translated": False,
        }
        for key, value in expected_provenance.items():
            if metadata.get(key) != value:
                raise AssertionError(f"{name}: provenance field {key!r} was not preserved")
        if report["validation_scope"]["runtime_compatibility_verified"] is not False:
            raise AssertionError(f"{name}: report overstated runtime evidence")
        if report["validation_scope"]["load_attempted"] is not False:
            raise AssertionError(f"{name}: report did not preserve the read-only contract")
        if report["inspection"]["execution_attempted"] is not False:
            raise AssertionError(f"{name}: inspector report did not prove no execution")
        if expected_subtype and report["inspection"]["cpu_subtype"] != expected_subtype:
            raise AssertionError(f"{name}: wrong accepted CPU subtype")
        if report["validation_scope"]["layout_profile"] != "xnu-development-kernel-v1":
            raise AssertionError(f"{name}: structural layout policy was not recorded")
        if report["validation_scope"]["entry_segment"] != "__HIB":
            raise AssertionError(f"{name}: startup segment was not recorded")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} VERIFIER", file=sys.stderr)
        return 2
    verifier = pathlib.Path(sys.argv[1])

    run_case(verifier, "x86_64 ALL", make_valid_image(), True,
             expected_subtype="X86_64_ALL")
    run_case(verifier, "x86_64 H", make_valid_image(CPU_SUBTYPE_X86_64_H), True,
             expected_subtype="X86_64_H")
    run_case(verifier, "pinned subtype mismatch", make_valid_image(CPU_SUBTYPE_X86_64_H),
             False, expected_subtype="X86_64_H", policy_subtype="X86_64_ALL")

    wrong_file_type = make_valid_image()
    put_u32(wrong_file_type, 12, 6)  # MH_DYLIB
    run_case(verifier, "non-MH_EXECUTE", wrong_file_type, False)

    wrong_cpu = make_valid_image()
    put_u32(wrong_cpu, 4, 0x0100000C)  # CPU_TYPE_ARM64
    run_case(verifier, "non-x86_64 CPU", wrong_cpu, False)

    unsupported_subtype = make_valid_image()
    put_u32(unsupported_subtype, 8, 9)
    run_case(verifier, "unsupported subtype", unsupported_subtype, False)

    bad_thread_flavor = make_valid_image()
    put_u32(bad_thread_flavor, THREAD_OFF + 8, 1)
    run_case(verifier, "wrong thread-state flavor", bad_thread_flavor, False)

    bad_thread_count = make_valid_image()
    put_u32(bad_thread_count, THREAD_OFF + 12, X86_THREAD_STATE64_COUNT - 1)
    run_case(verifier, "wrong thread-state count", bad_thread_count, False)

    rip_outside_file = make_valid_image()
    put_u64(rip_outside_file, RIP_OFF, 0x2500)
    run_case(verifier, "RIP outside file-backed bootstrap segment", rip_outside_file, False)

    non_executable_hib = make_valid_image()
    put_u32(non_executable_hib, HIB_OFF + 60, 3)  # r/w, no execute
    run_case(verifier, "RIP in non-executable bootstrap segment", non_executable_hib, False)

    rip_in_text = make_valid_image()
    put_u64(rip_in_text, RIP_OFF, 0x4300)
    run_case(verifier, "RIP in __TEXT instead of __HIB", rip_in_text, False,
             expected_diagnostic="thread RIP is not inside the file-backed executable __HIB")

    unexpected_text_maxprot = make_valid_image()
    put_u32(unexpected_text_maxprot, TEXT_OFF + 56, 7)  # writable maxprot; parser accepts, policy rejects
    run_case(
        verifier,
        "provisional __TEXT policy diagnostics",
        unexpected_text_maxprot,
        False,
        expected_diagnostic="observed {'vmaddr':",
    )

    renamed_text = make_valid_image()
    put_name(renamed_text, TEXT_OFF + 8, "__KERNEL")
    put_name(renamed_text, TEXT_OFF + 88, "__KERNEL")
    run_case(verifier, "unexpected segment layout", renamed_text, False)

    unknown_command = make_valid_image()
    put_u32(unknown_command, 16, 5)
    put_u32(unknown_command, 20, COMMAND_BYTES + 8)
    put_u32(unknown_command, THREAD_OFF + THREAD_SIZE, LC_OPTIONAL_UNKNOWN)
    put_u32(unknown_command, THREAD_OFF + THREAD_SIZE + 4, 8)
    run_case(verifier, "unclassified optional load command", unknown_command, False)

    valid = make_valid_image()
    run_case(
        verifier,
        "manifest artifact hash mismatch",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"artifact_sha256": "0" * 64}),
    )
    run_case(
        verifier,
        "manifest architecture mismatch",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"target_architecture": "arm64"}),
    )
    run_case(
        verifier,
        "runtime claim rejected",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"runtime_compatibility_verified": True}),
    )
    bad_provenance = (
        ("XNU patch not applied", "xnu_source_modified", False),
        ("XNU patch path mismatch", "xnu_source_patch", "xnu-patches/unexpected.patch"),
        ("XNU patch digest mismatch", "xnu_source_patch_sha256", "0" * 64),
        ("compatibility patch digest mismatch", "xnu_source_compat_patch_sha256", "0" * 64),
        ("vendor policy mismatch", "xnu_non_intel_vendor_policy", "allow-generic-cpuid"),
        ("host architecture mismatch", "build_host_arch", "arm64"),
        ("host mode mismatch", "build_host_mode", "translated"),
        ("process architecture mismatch", "build_process_arch", "arm64"),
        ("translated process rejected", "build_process_translated", True),
    )
    for name, key, value in bad_provenance:
        run_case(
            verifier,
            name,
            valid,
            False,
            mutate_manifest=lambda manifest, key=key, value=value: manifest.update({key: value}),
        )
    run_case(verifier, "output cannot overwrite input", valid, False,
             output_alias=True)
    run_case(
        verifier,
        "manifest size mismatch",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"artifact_size_bytes": len(valid) + 1}),
    )
    run_case(
        verifier,
        "manifest schema mismatch",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"build_result_schema_version": 2}),
    )
    run_case(
        verifier,
        "manifest tag and version mismatch",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"xnu_source_version": "12377.121.7"}),
    )
    run_case(
        verifier,
        "manifest commit mismatch",
        valid,
        False,
        mutate_manifest=lambda manifest: manifest.update({"xnu_commit": "deadbeef"}),
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
