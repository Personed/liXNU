#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Move the source kernel's __KLD/__LAST link slots after __TEXT_EXEC when they overlap.

    kernel-segaddrs.py KERNEL SEGADDRS [--write]

The k27 kernel is linked at fixed collection offsets by k27/segaddrs.sh. XNU can grow past the
static __KLD slot, making two file-backed Mach-O segments occupy the same bytes in the collection.
This helper measures the linked kernel, plans non-overlapping executable slots in the free-KC layout,
and optionally rewrites the generated segaddrs assignments. Without --write it fails while the kernel
still overlaps.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

PAGE = 0x4000
EXECUTABLE_SEGMENTS = ('__TEXT_EXEC', '__KLD', '__LAST')


def align(value, alignment=PAGE):
    return (value + alignment - 1) // alignment * alignment


def read_macho_segments(path):
    data = Path(path).read_bytes()
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe':
        raise ValueError(f'{path}: not a 64-bit little-endian Mach-O')
    count, = struct.unpack_from('<I', data, 16)
    segments = {}
    offset = 32
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            name = data[offset + 8:offset + 24].split(b'\0')[0].decode()
            vmaddr, vmsize = struct.unpack_from('<QQ', data, offset + 24)
            segments[name] = (vmaddr, vmsize)
        offset += length
    if offset > len(data):
        raise ValueError(f'{path}: truncated Mach-O load commands')
    return segments


def read_segaddrs(path):
    text = Path(path).read_text()
    match = re.search(r'^KB=(0x[0-9a-fA-F]+)$', text, re.MULTILINE)
    if not match:
        raise ValueError(f'{path}: missing KB assignment')
    offsets = {name: int(value, 16) for name, value in
               re.findall(r'\ba\s+(__[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+)\s*;', text)}
    missing = [name for name in ('__TEXT', '__TEXT_EXEC', '__KLD', '__LAST', '__KLDDATA') if name not in offsets]
    if missing:
        raise ValueError(f'{path}: missing segment assignments: {", ".join(missing)}')
    return text, int(match.group(1), 16), offsets


def plan_executable_slots(segments, base, offsets):
    missing = [name for name in EXECUTABLE_SEGMENTS if name not in segments]
    if missing:
        raise ValueError(f'kernel is missing executable segments: {", ".join(missing)}')
    text_exec, kld, last = (segments[name] for name in EXECUTABLE_SEGMENTS)
    text_end = text_exec[0] + text_exec[1]
    kld_end = kld[0] + kld[1]
    if kld[0] >= text_end and last[0] >= align(kld_end):
        return None

    expected_base = segments['__TEXT'][0] - offsets['__TEXT']
    if expected_base != base:
        raise ValueError(f'kernel __TEXT implies base {expected_base:#x}, not segaddrs KB {base:#x}')
    if segments['__TEXT_EXEC'][0] != base + offsets['__TEXT_EXEC']:
        raise ValueError('kernel __TEXT_EXEC does not match the existing segaddrs layout')

    new_kld = align(text_end)
    new_last = align(new_kld + align(kld[1]))
    executable_end = align(new_last + align(last[1]))
    new_offsets = {
        '__KLD': new_kld - base,
        '__LAST': new_last - base,
    }
    if executable_end - base > offsets['__KLDDATA']:
        raise ValueError('relocated executable segments reach __KLDDATA; the free-KC data layout must move first')
    return {
        'offsets': new_offsets,
        'changed': new_offsets != {name: offsets[name] for name in new_offsets},
        'old': {name: base + offsets[name] for name in new_offsets},
        'new': {name: base + new_offsets[name] for name in new_offsets},
    }


def rewrite_segaddrs(text, replacements):
    for name, value in replacements.items():
        pattern = rf'(\ba\s+{re.escape(name)}\s+)0x[0-9a-fA-F]+(\s*;)'
        text, count = re.subn(pattern, rf'\g<1>{value:#x}\g<2>', text, count=1)
        if count != 1:
            raise ValueError(f'could not rewrite the {name} assignment')
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kernel')
    parser.add_argument('segaddrs')
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args(argv)

    try:
        segments = read_macho_segments(args.kernel)
        text, base, offsets = read_segaddrs(args.segaddrs)
        plan = plan_executable_slots(segments, base, offsets)
        if plan is None:
            print('kernel executable segments are separated')
            return 0
        if not args.write:
            raise ValueError('kernel executable segments overlap; rerun with --write and relink the kernel')
        replacements = plan['offsets']
        if plan['changed']:
            Path(args.segaddrs).write_text(rewrite_segaddrs(text, replacements))
            for name in ('__KLD', '__LAST'):
                print(f'moved {name}: {plan["old"][name]:#x} -> {plan["new"][name]:#x}')
        print('relink kernel with relocated executable slots')
        return 0
    except ValueError as error:
        print(f'kernel-segaddrs: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
