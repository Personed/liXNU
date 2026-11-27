#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Move the free-KC kernel's linker markers after its measured executable segment."""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'kclink'))
from macho import Image, KC_PAGE  # noqa: E402

LINK_ADDR = re.compile(r'(\ba\s+(__[A-Z0-9_]+)\s+)0x([0-9a-fA-F]+)')
MARKERS = ('__KLD', '__LAST')


def align(value, alignment=KC_PAGE):
    return (value + alignment - 1) // alignment * alignment


def fit_offsets(segments, kernel_header, offsets):
    """Return safe offsets for __KLD and __LAST, or fail if the layout still collides."""
    by_name = {}
    for segment in segments:
        if segment.name in by_name:
            raise ValueError(f'kernel has duplicate {segment.name} segments')
        by_name[segment.name] = segment
    # __LAST can be a zero-sized linker marker and may not be emitted as a
    # Mach-O segment at all. Its address still lives in segaddrs.sh.
    missing = [name for name in ('__TEXT', '__TEXT_EXEC', '__KLD', '__KLDDATA') if name not in by_name]
    if missing:
        raise ValueError(f'kernel is missing required segments: {", ".join(missing)}')

    result = dict(offsets)
    for name in MARKERS:
        if name not in result:
            raise ValueError(f'linker address list has no {name} address')

    text_exec = by_name['__TEXT_EXEC']
    text_exec_start = text_exec.vmaddr - kernel_header
    text_exec_end = text_exec_start + text_exec.vmsize
    result['__KLD'] = align(max(result['__KLD'], text_exec_end))

    kld = by_name['__KLD']
    kld_end = result['__KLD'] + kld.vmsize
    result['__LAST'] = align(max(result['__LAST'], kld_end))

    intervals = []
    for segment in segments:
        if not segment.vmsize:
            continue
        # The only ranges the fitter changes are the marker segments. Use the
        # addresses measured from Mach-O for every other range so stale or
        # incorrect segaddrs.sh entries cannot hide a real overlap.
        start = result[segment.name] if segment.name in MARKERS else segment.vmaddr - kernel_header
        end = start + segment.vmsize
        intervals.append((start, end, segment.name))
    intervals.sort()
    for (start, end, name), (next_start, next_end, next_name) in zip(intervals, intervals[1:]):
        if next_start < end:
            raise ValueError(f'kernel segments {name} ({start:#x}-{end:#x}) and '
                             f'{next_name} ({next_start:#x}-{next_end:#x}) overlap after relink placement')

    last = by_name.get('__LAST')
    last_end = result['__LAST'] + (last.vmsize if last else 0)
    klddata_start = by_name['__KLDDATA'].vmaddr - kernel_header
    if last_end > klddata_start:
        raise ValueError(f'kernel __LAST ends at {last_end:#x}, beyond __KLDDATA at {klddata_start:#x}')
    return result


def relink_needed(segments, kernel_header, fitted):
    """Whether the current Mach-O has not yet been linked at the fitted markers."""
    by_name = {segment.name: segment for segment in segments}
    for name in MARKERS:
        segment = by_name.get(name)
        # An absent __LAST is a valid representation of its marker, but an
        # emitted zero-sized segment still carries the address XNU consumes.
        if name == '__LAST' and segment is None:
            continue
        if segment is None or segment.vmaddr - kernel_header != fitted[name]:
            return True
    return False


def parse_link_offsets(text):
    offsets = {}
    for match in LINK_ADDR.finditer(text):
        name, value = match.group(2), int(match.group(3), 16)
        if name in offsets:
            raise ValueError(f'linker address list has duplicate {name} entries')
        offsets[name] = value
    return offsets


def render_link_offsets(text, offsets):
    seen = set()

    def replace(match):
        name = match.group(2)
        if name not in MARKERS:
            return match.group(0)
        seen.add(name)
        return f'{match.group(1)}0x{offsets[name]:x}'

    rendered = LINK_ADDR.sub(replace, text)
    missing = set(MARKERS) - seen
    if missing:
        raise ValueError(f'linker address list has no {", ".join(sorted(missing))} command')
    return rendered


def report_segments(segments, kernel_header):
    for segment in segments:
        if segment.name in ('__TEXT_EXEC', '__KLD', '__LAST'):
            print(f'kernel segment {segment.name}: vmaddr={segment.vmaddr:#x} '
                  f'vmsize={segment.vmsize:#x} fileoff={segment.fileoff:#x} '
                  f'filesize={segment.filesize:#x} kc_offset={segment.vmaddr - kernel_header:#x}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('segaddrs', type=Path)
    parser.add_argument('--check', action='store_true', help='report and fail unless the linked ranges are already safe')
    args = parser.parse_args()

    kernel = Image(args.kernel.read_bytes())
    text = kernel.segment('__TEXT')
    if text is None:
        raise SystemExit(f'{args.kernel}: no __TEXT segment')
    kernel_header = text.vmaddr - 0x4000
    report_segments(kernel.segments, kernel_header)
    original = args.segaddrs.read_text()
    offsets = parse_link_offsets(original)
    fitted = fit_offsets(kernel.segments, kernel_header, offsets)
    stale_kernel = relink_needed(kernel.segments, kernel_header, fitted)
    if args.check:
        if fitted != offsets or stale_kernel:
            raise SystemExit('kernel linker addresses still need adjustment or relinking')
        print('kernel segment ranges are disjoint and __LAST ends before __KLDDATA')
        return

    changed = fitted != offsets
    if changed:
        args.segaddrs.write_text(render_link_offsets(original, fitted))
    if changed or stale_kernel:
        print('relink kernel: fitted marker addresses or linked Mach-O are stale')
    else:
        print('kernel segment linker addresses already fit')
    for name in MARKERS:
        if fitted[name] != offsets[name]:
            print(f'moved {name}: {offsets[name]:#x} -> {fitted[name]:#x}')


if __name__ == '__main__':
    main()
