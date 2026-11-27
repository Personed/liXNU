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
LC_UUID = 0x1B
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
            "kernel": {
                "file_type": 2,
                "cpu_subtype": 3,
                "load_command_count": 1,
                "load_command_bytes": 24,
                "fileoff": "0x0000000000000080",
                "vmaddr": "0xffffff8000200000",
            },
        },
    )
    verify(binary, image[:-1], {"status": "parser_rejected", "parser_result": -8})

    struct.pack_into("<I", image, 4, 0x0100000C)  # unsupported ARM64 container
    verify(binary, image, {"status": "parser_rejected", "parser_result": -95})

    struct.pack_into("<I", image, 12, MH_EXECUTE)
    verify(binary, image, {"status": "not_fileset", "parser_result": None})
    print("Recovery fileset metadata fixtures passed")


if __name__ == "__main__":
    main()
