#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic fixtures for the read-only x86_64 Mach-O inspector."""

import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


FILE_SIZE = 1024
HEADER_SIZE = 32
PAGEZERO_SIZE = 72
TEXT_SIZE = 72 + 80
THREAD_SIZE = 8 + 8 + 42 * 4
COMMAND_BYTES = PAGEZERO_SIZE + TEXT_SIZE + THREAD_SIZE
PAGEZERO_OFF = HEADER_SIZE
TEXT_OFF = PAGEZERO_OFF + PAGEZERO_SIZE
SECTION_OFF = TEXT_OFF + 72
THREAD_OFF = TEXT_OFF + TEXT_SIZE
COMMANDS_END = HEADER_SIZE + COMMAND_BYTES
RIP_OFF = THREAD_OFF + 8 + 8 + 16 * 8

MH_MAGIC_64 = 0xFEEDFACF
MH_EXECUTE = 2
CPU_TYPE_X86_64 = 0x01000007
CPU_SUBTYPE_X86_64_ALL = 3
CPU_SUBTYPE_X86_64_H = 8
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_DYLD_CHAINED_FIXUPS = 0x80000034
X86_THREAD_STATE64 = 4
X86_THREAD_STATE64_COUNT = 42


def put_u32(data, off, value):
    struct.pack_into("<I", data, off, value)


def put_u64(data, off, value):
    struct.pack_into("<Q", data, off, value)


def put_name(data, off, value):
    encoded = value.encode("ascii")
    if len(encoded) > 16:
        raise ValueError("Mach-O fixed name exceeds 16 bytes")
    data[off : off + 16] = encoded.ljust(16, b"\0")


def make_valid_image(subtype=CPU_SUBTYPE_X86_64_ALL):
    data = bytearray(FILE_SIZE)
    for off, value in enumerate(
        (
            MH_MAGIC_64,
            CPU_TYPE_X86_64,
            subtype,
            MH_EXECUTE,
            3,
            COMMAND_BYTES,
            0,
            0,
        )
    ):
        put_u32(data, off * 4, value)

    put_u32(data, PAGEZERO_OFF, LC_SEGMENT_64)
    put_u32(data, PAGEZERO_OFF + 4, PAGEZERO_SIZE)
    put_name(data, PAGEZERO_OFF + 8, "__PAGEZERO")
    put_u64(data, PAGEZERO_OFF + 24, 0)
    put_u64(data, PAGEZERO_OFF + 32, 0x1000)

    put_u32(data, TEXT_OFF, LC_SEGMENT_64)
    put_u32(data, TEXT_OFF + 4, TEXT_SIZE)
    put_name(data, TEXT_OFF + 8, "__TEXT")
    put_u64(data, TEXT_OFF + 24, 0x1000)
    put_u64(data, TEXT_OFF + 32, 0x2000)
    put_u64(data, TEXT_OFF + 40, 0)
    put_u64(data, TEXT_OFF + 48, 0x200)
    put_u32(data, TEXT_OFF + 56, 5)
    put_u32(data, TEXT_OFF + 60, 5)
    put_u32(data, TEXT_OFF + 64, 1)
    put_u32(data, TEXT_OFF + 68, 0)

    put_name(data, SECTION_OFF, "__text")
    put_name(data, SECTION_OFF + 16, "__TEXT")
    put_u64(data, SECTION_OFF + 32, 0x1100)
    put_u64(data, SECTION_OFF + 40, 0x20)
    put_u32(data, SECTION_OFF + 48, 0x100)
    put_u32(data, SECTION_OFF + 52, 4)

    put_u32(data, THREAD_OFF, LC_UNIXTHREAD)
    put_u32(data, THREAD_OFF + 4, THREAD_SIZE)
    put_u32(data, THREAD_OFF + 8, X86_THREAD_STATE64)
    put_u32(data, THREAD_OFF + 12, X86_THREAD_STATE64_COUNT)
    put_u64(data, RIP_OFF, 0x1100)
    return data


def append_command(data, command, command_size):
    put_u32(data, COMMANDS_END, command)
    put_u32(data, COMMANDS_END + 4, command_size)
    put_u32(data, 16, 4)
    put_u32(data, 20, COMMAND_BYTES + command_size)


def run_case(binary, name, data, expected_status, check_report=None):
    with tempfile.TemporaryDirectory(prefix="xnu-macho-inspect-") as tmp:
        path = pathlib.Path(tmp) / "kernel.development"
        path.write_bytes(data)
        before = hashlib.sha256(path.read_bytes()).digest()
        result = subprocess.run(
            [binary, str(path)], check=False, capture_output=True, text=True
        )
        if result.returncode != expected_status:
            raise AssertionError(
                f"{name}: exit {result.returncode}, expected {expected_status}; "
                f"stderr={result.stderr!r}"
            )
        if hashlib.sha256(path.read_bytes()).digest() != before:
            raise AssertionError(f"{name}: inspector modified the input file")
        if check_report is None:
            if result.stdout:
                raise AssertionError(f"{name}: rejected input produced a report")
            if not result.stderr:
                raise AssertionError(f"{name}: rejected input had no diagnostic")
            return
        report = json.loads(result.stdout)
        check_report(report)


def expect_valid_all(report):
    assert report["architecture"] == "x86_64", report
    assert report["cpu_subtype"] == "X86_64_ALL", report
    assert report["format"] == "Mach-O 64-bit little-endian MH_EXECUTE", report
    assert report["entry"] == {
        "source": "LC_UNIXTHREAD.x86_THREAD_STATE64.RIP",
        "unslid": "0x0000000000001100",
    }, report
    assert report["vm_range"] == {
        "low": "0x0000000000001000",
        "high": "0x0000000000003000",
    }, report
    assert report["segment_count"] == 2, report
    assert report["mapped_segment_count"] == 1, report
    assert [segment["name"] for segment in report["segments"]] == [
        "__PAGEZERO",
        "__TEXT",
    ], report
    assert report["segments"][0]["mapped"] is False, report
    assert report["segments"][1] == {
        "name": "__TEXT",
        "vmaddr": "0x0000000000001000",
        "vmsize": "0x0000000000002000",
        "fileoff": "0x0000000000000000",
        "filesize": "0x0000000000000200",
        "maxprot": "r-x",
        "initprot": "r-x",
        "nsects": 1,
        "flags": "0x00000000",
        "mapped": True,
    }, report
    assert report["load_status"] == "not_loaded", report
    assert report["fixups_interpreted"] is False, report
    assert report["execution_attempted"] is False, report


def expect_valid_fixups_are_not_interpreted(report):
    assert report["inspection_flags"] == 1 << 2, report
    assert report["linkedit_metadata"]["has_chained_fixups"] is True, report
    assert report["linkedit_data_command_count"] == 1, report
    assert report["load_status"] == "not_loaded", report
    assert report["fixups_interpreted"] is False, report
    assert report["execution_attempted"] is False, report


def expect_x86_64_h(report):
    assert report["cpu_subtype"] == "X86_64_H", report


def expect_unknown_optional_is_reported(report):
    assert report["unclassified_optional_commands"] == 1, report
    assert report["execution_attempted"] is False, report


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} INSPECTOR", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    run_case(binary, "valid x86_64 ALL", make_valid_image(), 0, expect_valid_all)
    run_case(
        binary,
        "valid x86_64 H",
        make_valid_image(CPU_SUBTYPE_X86_64_H),
        0,
        expect_x86_64_h,
    )

    chained = make_valid_image()
    append_command(chained, LC_DYLD_CHAINED_FIXUPS, 16)
    put_u32(chained, COMMANDS_END + 8, 512)
    put_u32(chained, COMMANDS_END + 12, 32)
    run_case(
        binary,
        "bounded chained-fixup metadata",
        chained,
        0,
        expect_valid_fixups_are_not_interpreted,
    )

    bad_fixup_extent = make_valid_image()
    append_command(bad_fixup_extent, LC_DYLD_CHAINED_FIXUPS, 16)
    put_u32(bad_fixup_extent, COMMANDS_END + 8, FILE_SIZE - 8)
    put_u32(bad_fixup_extent, COMMANDS_END + 12, 32)
    run_case(binary, "malformed chained-fixup extent", bad_fixup_extent, 3)

    unknown_optional = make_valid_image()
    append_command(unknown_optional, 0x1234, 8)
    run_case(
        binary,
        "unknown optional command is surfaced",
        unknown_optional,
        0,
        expect_unknown_optional_is_reported,
    )

    wrong_cpu = make_valid_image()
    put_u32(wrong_cpu, 4, 0x0100000C)
    run_case(binary, "unsupported CPU", wrong_cpu, 4)

    fileset = make_valid_image()
    put_u32(fileset, 12, 12)
    run_case(binary, "unsupported fileset", fileset, 4)

    unknown_required = make_valid_image()
    append_command(unknown_required, 0x80001234, 8)
    run_case(binary, "unknown required command", unknown_required, 4)

    bad_command = make_valid_image()
    put_u32(bad_command, PAGEZERO_OFF + 4, 0xFFFFFFFF)
    run_case(binary, "malformed command size", bad_command, 3)

    bad_entry = make_valid_image()
    put_u64(bad_entry, RIP_OFF, 0x3100)
    run_case(binary, "entry outside executable file range", bad_entry, 3)

    print("x86_64 Mach-O inspector fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
