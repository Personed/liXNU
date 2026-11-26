# SPDX-License-Identifier: GPL-2.0-only
"""Check file-backed kernel segments after they are copied into a collection."""


def check_kernel_segment_copies(segments, kernel, collection, header):
    """Return mapped ranges and identical overlaps, or reject overwritten segment bytes.

    kclink maps each kernel segment at ``vmaddr - header``. Two Mach-O segments can
    therefore name overlapping file-backed ranges inside one collection region. Such
    aliases are safe only when both source ranges contain the same bytes and the final
    collection still contains those bytes for every segment.
    """
    mapped = []
    for segment in segments:
        if segment.name == '__LINKEDIT' or not segment.filesize:
            continue
        if segment.filesize > segment.vmsize:
            raise ValueError(f'{segment.name}: filesize {segment.filesize:#x} exceeds vmsize {segment.vmsize:#x}')
        source_start, source_end = segment.fileoff, segment.fileoff + segment.filesize
        dest_start = segment.vmaddr - header
        dest_end = dest_start + segment.filesize
        if source_start < 0 or source_end > len(kernel):
            raise ValueError(f'{segment.name}: source file range {source_start:#x}-{source_end:#x} is outside the kernel')
        if dest_start < 0 or dest_end > len(collection):
            raise ValueError(f'{segment.name}: collection range {dest_start:#x}-{dest_end:#x} is outside the output')
        mapped.append((segment, source_start, source_end, dest_start, dest_end))

    overlaps = []
    for index, first in enumerate(mapped):
        a, a_file, _, a_dest, a_end = first
        for second in mapped[index + 1:]:
            b, b_file, _, b_dest, b_end = second
            start, end = max(a_dest, b_dest), min(a_end, b_end)
            if start >= end:
                continue
            a_bytes = kernel[a_file + start - a_dest:a_file + end - a_dest]
            b_bytes = kernel[b_file + start - b_dest:b_file + end - b_dest]
            if a_bytes != b_bytes:
                raise ValueError(f'{a.name} and {b.name}: conflicting file-backed overlap at {start:#x}-{end:#x}')
            overlaps.append((a.name, b.name, start, end))

    for segment, source_start, source_end, dest_start, dest_end in mapped:
        if collection[dest_start:dest_end] != kernel[source_start:source_end]:
            raise ValueError(f'{segment.name}: copied bytes were overwritten at {dest_start:#x}-{dest_end:#x}')

    ranges = [(s.name, s.vmaddr, s.vmsize, s.fileoff, s.filesize, d0, d1)
              for s, _, _, d0, d1 in mapped]
    return ranges, overlaps
