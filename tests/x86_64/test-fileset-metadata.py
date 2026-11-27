#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the Recovery fileset JSON helper without an Apple image."""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_X86_64 = 0x01000007
CPU_SUBTYPE_X86_64_ALL = 3
MH_EXECUTE = 2
MH_FILESET = 12
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_FILESET_ENTRY = 0x80000035


def make_fileset():
    member_offset = 128
    image = bytearray(member_offset + 32 + 24)
    struct.pack_into(
        "<8I", image, 0, MH_MAGIC_64, CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL,
        MH_FILESET, 1, 56, 0, 0,
    )
    struct.pack_into(
        "<IIQQII", image, 32, LC_FILESET_ENTRY, 56, 0xFFFFFF8000200000,
        member_offset, 32, 0,
    )
    entry_id = b"com.apple.kernel\0"
    image[64 : 64 + len(entry_id)] = entry_id
    struct.pack_into(
        "<8I", image, member_offset, MH_MAGIC_64, CPU_TYPE_X86_64,
        CPU_SUBTYPE_X86_64_ALL, MH_EXECUTE, 1, 24, 0, 0,
    )
    struct.pack_into("<II", image, member_offset + 32, LC_UUID, 24)
    return image


def verify(binary, image, expected):
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary) / "BootKernelExtensions.kc"
        path.write_bytes(image)
        result = subprocess.run(
            [binary, str(path)], check=True, capture_output=True, text=True, timeout=10
        )
        report = json.loads(result.stdout)
        assert report == expected, report
        assert result.stderr == "", result.stderr
        assert path.read_bytes() == image


def verify_segment_bounds(binary):
    image = make_fileset()
    image.extend(bytes(0x120 - len(image)))
    member = 128
    command = member + 32
    struct.pack_into("<II", image, member + 16, 1, 72)
    name = b"__TEXT"
    struct.pack_into(
        "<II16sQQQQIIII", image, command, LC_SEGMENT_64, 72, name,
        0xFFFFFF8000200000, 0x1000, 0x100, 0x20, 5, 5, 0, 0,
    )
    expected = {
        "status": "accepted",
        "parser_result": 0,
        "file_type": 12,
        "cpu_subtype": 3,
        "load_command_count": 1,
        "load_command_bytes": 56,
        "fileset_entry_count": 1,
        "chained_fixups": None,
        "kernel": {
            "file_type": 2,
            "cpu_subtype": 3,
            "load_command_count": 1,
            "load_command_bytes": 72,
            "fileoff": "0x0000000000000080",
            "vmaddr": "0xffffff8000200000",
            "chained_fixups": None,
            "segments": [{
                "name_bytes_hex": name.ljust(16, b"\0").hex(),
                "vmaddr": "0xffffff8000200000",
                "vmsize": "0x0000000000001000",
                "fileoff": "0x0000000000000100",
                "filesize": "0x0000000000000020",
                "section_count": 0,
            }],
        },
    }
    # This range ends exactly at EOF only with container-relative fileoff.
    verify(binary, image, expected)

    # Insert one section_64 record before the original segment bytes.
    sectioned = bytearray(image)
    section_offset = command + 72
    sectioned[section_offset:section_offset] = bytes(80)
    struct.pack_into("<I", sectioned, member + 20, 152)
    struct.pack_into("<I", sectioned, command + 4, 152)
    struct.pack_into("<Q", sectioned, command + 40, 0x150)
    struct.pack_into("<I", sectioned, command + 64, 1)
    struct.pack_into(
        "<16s16sQQIIIIIIII", sectioned, section_offset,
        b"__text", name, 0xFFFFFF8000200000, 0x20, 0x150, 0, 0, 0, 0, 0, 0, 0,
    )
    kernel = expected["kernel"]
    kernel["load_command_bytes"] = 152
    kernel["segments"][0].update(fileoff="0x0000000000000150", section_count=1)
    verify(binary, sectioned, expected)

    rejected = {"status": "parser_rejected", "parser_result": -8}
    verify(binary, image[:-1], rejected)
    invalid = bytearray(image)
    struct.pack_into("<I", invalid, command + 64, 1)  # missing section record
    verify(binary, invalid, rejected)
    invalid = bytearray(image)
    struct.pack_into("<Q", invalid, command + 24, 0xFFFFFFFFFFFFFFF0)
    verify(binary, invalid, rejected)

    count = 25  # Above the fixed descriptor table capacity.
    expanded = image[:command] + image[command : command + 72] * count
    struct.pack_into("<II", expanded, member + 16, count, 72 * count)
    verify(binary, expanded, {"status": "parser_rejected", "parser_result": -95})


def verify_fixup_extents(binary):
    image = make_fileset()
    image.extend(bytes(768 - len(image)))
    outer = 88
    member = 256
    nested = member + 32
    outer_va = 0xFFFFFF8000300000
    nested_va = 0xFFFFFF8000500000
    struct.pack_into("<II", image, 16, 3, 56 + 72 + 16)
    struct.pack_into("<Q", image, 48, member)
    struct.pack_into(
        "<8I", image, member, MH_MAGIC_64, CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL,
        MH_EXECUTE, 2, 16 + 72, 0, 0,
    )
    # Reverse command order for the inner member. Both extents exactly end
    # at their own file and VM limit; the inner extent also reaches EOF.
    struct.pack_into("<II16sQQQQIIII", image, outer, LC_SEGMENT_64, 72,
                     b"__LINKEDIT", outer_va, 64, 512, 64, 1, 1, 0, 0)
    outer_fixups = outer + 72
    struct.pack_into("<4I", image, outer_fixups, LC_DYLD_CHAINED_FIXUPS, 16, 544, 32)
    struct.pack_into("<4I", image, nested, LC_DYLD_CHAINED_FIXUPS, 16, 736, 32)
    nested_linkedit = nested + 16
    struct.pack_into("<II16sQQQQIIII", image, nested_linkedit, LC_SEGMENT_64, 72,
                     b"__LINKEDIT", nested_va, 64, 704, 64, 1, 1, 0, 0)

    def hex_value(value):
        return f"0x{value:016x}"

    def fixup_report(vmaddr, fileoff, dataoff):
        return {
            "dataoff": hex_value(dataoff),
            "datasize": hex_value(32),
            "linkedit_relative_offset": hex_value(dataoff - fileoff),
            "linkedit": {
                "vmaddr": hex_value(vmaddr), "vmsize": hex_value(64),
                "fileoff": hex_value(fileoff), "filesize": hex_value(64),
            },
        }

    inner_report = fixup_report(nested_va, 704, 736)
    expected = {
        "status": "accepted",
        "parser_result": 0,
        "file_type": 12,
        "cpu_subtype": 3,
        "load_command_count": 3,
        "load_command_bytes": 144,
        "fileset_entry_count": 1,
        "chained_fixups": fixup_report(outer_va, 512, 544),
        "kernel": {
            "file_type": 2, "cpu_subtype": 3,
            "load_command_count": 2, "load_command_bytes": 88,
            "fileoff": hex_value(member), "vmaddr": hex_value(0xFFFFFF8000200000),
            "chained_fixups": inner_report,
            "segments": [{
                **inner_report["linkedit"],
                "name_bytes_hex": b"__LINKEDIT".ljust(16, b"\0").hex(),
                "section_count": 0,
            }],
        },
    }
    verify(binary, image, expected)

    # An unrelated outer segment is outside this metadata check, even when
    # the collection has a valid fixup command. Its framing still has to fit.
    unrelated_outer = bytearray(image)
    struct.pack_into("<II", unrelated_outer, 16, 4, 144 + 72)
    struct.pack_into("<II16sQQQQIIII", unrelated_outer, outer_fixups + 16,
                     LC_SEGMENT_64, 72, b"__DATA", outer_va, 64, 9999, 64, 1, 1, 0, 0)
    verify(binary, unrelated_outer, {
        **expected, "load_command_count": 4, "load_command_bytes": 216,
    })

    # With no outer fixups, don't read the outer __LINKEDIT extent.
    no_outer_fixups = bytearray(image)
    struct.pack_into("<II", no_outer_fixups, 16, 2, 128)
    struct.pack_into("<Q", no_outer_fixups, outer + 40, 9999)
    verify(binary, no_outer_fixups, {
        **expected, "load_command_count": 2, "load_command_bytes": 128,
        "chained_fixups": None,
    })

    rejected = {"status": "parser_rejected", "parser_result": -8}
    for fmt, offset, value in (
        ("<I", outer_fixups + 8, 511),  # Before __LINKEDIT.
        ("<I", outer_fixups + 8, 736),  # Within the other header's __LINKEDIT.
        ("<Q", outer + 48, 63),  # File extent alone is one byte too short.
        ("<I", outer_fixups + 12, 27),  # Fixups header would be truncated.
        ("<Q", outer + 32, 63),  # VM extent is one byte too short.
        ("<I", nested + 12, 33),  # Fixups extend past the containing file.
        ("<16s", nested_linkedit + 8, b"__DATA"),  # Missing matching __LINKEDIT.
    ):
        invalid = bytearray(image)
        struct.pack_into(fmt, invalid, offset, value)
        verify(binary, invalid, rejected)

    duplicate_fixups = bytearray(image)
    struct.pack_into("<II", duplicate_fixups, 16, 4, 144 + 16)
    duplicate_fixups[outer_fixups + 16 : outer_fixups + 32] = image[outer_fixups : outer_fixups + 16]
    verify(binary, duplicate_fixups, rejected)

    duplicate_linkedit = bytearray(image)
    struct.pack_into("<II", duplicate_linkedit, member + 16, 3, 88 + 72)
    duplicate_linkedit[nested_linkedit + 72 : nested_linkedit + 144] = image[nested_linkedit : nested_linkedit + 72]
    verify(binary, duplicate_linkedit, rejected)


def main():
    binary = sys.argv[1]
    image = make_fileset()
    verify(
        binary, image,
        {
            "status": "accepted",
            "parser_result": 0,
            "file_type": 12,
            "cpu_subtype": 3,
            "load_command_count": 1,
            "load_command_bytes": 56,
            "fileset_entry_count": 1,
            "chained_fixups": None,
            "kernel": {
                "file_type": 2,
                "cpu_subtype": 3,
                "load_command_count": 1,
                "load_command_bytes": 24,
                "fileoff": "0x0000000000000080",
                "vmaddr": "0xffffff8000200000",
                "chained_fixups": None,
                "segments": [],
            },
        },
    )
    verify(binary, image[:-1], {"status": "parser_rejected", "parser_result": -8})

    wrong_cpu = bytearray(image)
    struct.pack_into("<I", wrong_cpu, 4, 0x0100000C)  # unsupported ARM64 container
    verify(binary, wrong_cpu, {"status": "parser_rejected", "parser_result": -95})

    struct.pack_into("<I", image, 12, MH_EXECUTE)
    verify(binary, image, {"status": "not_fileset", "parser_result": None})
    verify_segment_bounds(binary)
    verify_fixup_extents(binary)
    print("Recovery fileset metadata fixtures passed")


if __name__ == "__main__":
    main()
