#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic fixtures for the read-only x86_64 Mach-O inspector."""

import hashlib
import json
import os
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


def run_case(
    binary, name, data, expected_status, check_report=None, relative_name="kernel.development"
):
    with tempfile.TemporaryDirectory(prefix="xnu-macho-inspect-") as tmp:
        path = os.fsencode(tmp) + b"/" + relative_name.encode("utf-8")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as image_file:
            image_file.write(data)
        with open(path, "rb") as image_file:
            before = hashlib.sha256(image_file.read()).digest()
        result = subprocess.run(
            [os.fsencode(binary), path], check=False, capture_output=True, text=False
        )
        if result.returncode != expected_status:
            raise AssertionError(
                f"{name}: exit {result.returncode}, expected {expected_status}; "
                f"stderr={result.stderr!r}"
            )
        with open(path, "rb") as image_file:
            after = hashlib.sha256(image_file.read()).digest()
        if after != before:
            raise AssertionError(f"{name}: inspector modified the input file")
        if check_report is None:
            if result.stdout:
                raise AssertionError(f"{name}: rejected input produced a report")
            if not result.stderr:
                raise AssertionError(f"{name}: rejected input had no diagnostic")
            return
        report = json.loads(result.stdout)
        if report.get("file") != path.decode("utf-8"):
            raise AssertionError(f"{name}: report path does not round-trip: {report.get('file')!r}")
        if report.get("file_path_encoding") != "UTF-8":
            raise AssertionError(f"{name}: valid UTF-8 path was mislabeled")
        check_report(report)


def expect_valid_all(report):
    assert report["architecture"] == "x86_64", report
    assert report["file_path_encoding"] == "UTF-8", report
    assert report["file_path_bytes_hex"] == report["file"].encode("utf-8").hex(), report
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
    assert report["segments"][0]["name_bytes_hex"] == b"__PAGEZERO\0".ljust(16, b"\0").hex(), report
    assert report["segments"][1] == {
        "name": "__TEXT",
        "name_bytes_hex": b"__TEXT\0".ljust(16, b"\0").hex(),
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


def expected_path_for_byte_filename(path_bytes):
    decoded = path_bytes.decode("utf-8", errors="surrogateescape")
    return "".join(
        chr(ord(character) - 0xDC00)
        if 0xDC80 <= ord(character) <= 0xDCFF
        else character
        for character in decoded
    )


def expect_invalid_path_bytes_are_escaped(binary):
    if not sys.platform.startswith("linux"):
        return
    with tempfile.TemporaryDirectory(prefix="xnu-macho-inspect-") as tmp:
        parent = os.fsencode(tmp) + b"/bad-\xff"
        os.mkdir(parent)
        path = parent + b"/kernel.development"
        data = make_valid_image()
        with open(path, "wb") as image_file:
            image_file.write(data)
        result = subprocess.run(
            [os.fsencode(binary), path], check=False, capture_output=True
        )
        if result.returncode != 0:
            raise AssertionError(f"invalid UTF-8 filename failed: {result.stderr!r}")
        report = json.loads(result.stdout)
        if report["file"] != expected_path_for_byte_filename(path):
            raise AssertionError("invalid filename bytes were not represented reversibly")
        if report["file_path_bytes_hex"] != path.hex():
            raise AssertionError("invalid filename bytes were not preserved in hexadecimal")
        if report["file_path_encoding"] != "invalid bytes escaped as U+00XX":
            raise AssertionError("invalid filename encoding was not identified")
        if open(path, "rb").read() != data:
            raise AssertionError("invalid-path input file was modified")


def expect_ambiguous_display_bytes_preserve_originals(binary):
    if not sys.platform.startswith("linux"):
        return
    with tempfile.TemporaryDirectory(prefix="xnu-macho-inspect-") as tmp:
        root = os.fsencode(tmp)
        image = make_valid_image()
        paths = [
            root + b"/bad-\xff-\xe9/kernel.development",
            root + b"/bad-\xff-\xc3\xa9/kernel.development",
        ]
        reports = []
        for path in paths:
            os.makedirs(os.path.dirname(path))
            with open(path, "wb") as image_file:
                image_file.write(image)
            result = subprocess.run(
                [os.fsencode(binary), path], check=False, capture_output=True
            )
            if result.returncode != 0:
                raise AssertionError(f"ambiguous path failed: {result.stderr!r}")
            reports.append(json.loads(result.stdout))
        if reports[0]["file"] != reports[1]["file"]:
            raise AssertionError("fixture no longer reproduces the display collision")
        if [report["file_path_bytes_hex"] for report in reports] != [p.hex() for p in paths]:
            raise AssertionError("colliding display paths lost their original bytes")

        images = [make_valid_image(), make_valid_image()]
        names = [b"\xff-\xe9", b"\xff-\xc3\xa9"]
        segment_reports = []
        for data, name in zip(images, names):
            padded_name = name.ljust(16, b"\0")
            data[TEXT_OFF + 8 : TEXT_OFF + 24] = padded_name
            data[SECTION_OFF + 16 : SECTION_OFF + 32] = padded_name
            path = pathlib.Path(tmp) / ("segment-" + str(len(segment_reports)))
            path.write_bytes(data)
            result = subprocess.run(
                [binary, str(path)],
                check=False,
                capture_output=True,
                env={**os.environ, "LC_ALL": "C", "PYTHONUTF8": "0"},
            )
            if result.returncode != 0:
                raise AssertionError(f"ambiguous segment name failed: {result.stderr!r}")
            segment_reports.append(json.loads(result.stdout)["segments"][1])
        if segment_reports[0]["name"] != segment_reports[1]["name"]:
            raise AssertionError("fixture no longer reproduces the segment-name collision")
        if [report["name_bytes_hex"] for report in segment_reports] != [
            name.ljust(16, b"\0").hex() for name in names
        ]:
            raise AssertionError("colliding segment names lost their original bytes")


def expect_nonregular_input_does_not_block(binary):
    with tempfile.TemporaryDirectory(prefix="xnu-macho-inspect-") as tmp:
        fifo = pathlib.Path(tmp) / "no-writer.fifo"
        os.mkfifo(fifo)
        try:
            result = subprocess.run(
                [binary, str(fifo)], check=False, capture_output=True, text=True, timeout=2
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError("FIFO input blocked instead of failing closed") from error
        if result.returncode != 2 or result.stdout or not result.stderr:
            raise AssertionError("FIFO input was not rejected cleanly")


def expect_output_write_error(binary):
    if not os.path.exists("/dev/full"):
        return
    with tempfile.TemporaryDirectory(prefix="xnu-macho-inspect-") as tmp:
        path = pathlib.Path(tmp) / "kernel.development"
        path.write_bytes(make_valid_image())
        with open("/dev/full", "wb", buffering=0) as sink:
            result = subprocess.run(
                [binary, str(path)], check=False, stdout=sink, stderr=subprocess.PIPE
            )
        if result.returncode != 6 or b"failed to write JSON report" not in result.stderr:
            raise AssertionError("output write failure did not produce the I/O error status")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} INSPECTOR", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    run_case(binary, "valid x86_64 ALL", make_valid_image(), 0, expect_valid_all)
    run_case(
        binary,
        "UTF-8 path round trip",
        make_valid_image(),
        0,
        expect_valid_all,
        relative_name="núcleo/kernel.development",
    )
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
    put_u32(chained, COMMANDS_END + 12, 40)
    payload = 512
    starts = payload + 28
    put_u32(chained, payload, 0)  # fixups_version
    put_u32(chained, payload + 4, 28)  # starts_offset
    put_u32(chained, payload + 8, 40)  # empty imports after starts table
    put_u32(chained, payload + 12, 40)  # empty symbols after imports
    put_u32(chained, payload + 16, 0)  # imports_count
    put_u32(chained, payload + 20, 1)  # imports_format
    put_u32(chained, payload + 24, 0)  # uncompressed symbol strings
    put_u32(chained, starts, 2)  # PAGEZERO and TEXT, no starts records
    put_u32(chained, starts + 4, 0)
    put_u32(chained, starts + 8, 0)
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

    expect_invalid_path_bytes_are_escaped(binary)
    expect_ambiguous_display_bytes_preserve_originals(binary)
    expect_nonregular_input_does_not_block(binary)
    expect_output_write_error(binary)

    print("x86_64 Mach-O inspector fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
