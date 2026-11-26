# SPDX-License-Identifier: GPL-2.0-only
"""Adjust the empty free-kernel-collection layout to fit the source-built kernel."""
from copy import deepcopy

KC_PAGE = 0x4000
TEXT_EXEC_SEGMENTS = {'__TEXT_EXEC', '__KLD', '__LAST'}


def _number(value):
    return int(value, 16) if isinstance(value, str) else int(value)


def _align(value, alignment=KC_PAGE):
    return (value + alignment - 1) // alignment * alignment


def adjust_prelink_info(layout, kernel_header, kernel_segments):
    """Move the collection's prelink plist after the kernel's actual executable span.

    The reference layout comes from a shipped macOS kernelcache, while the source-built
    XNU can grow past its __TEXT_EXEC endpoint. The template's prelink plist is small and
    can occupy the free gap before __DATA. Keep all kernel segment addresses unchanged.
    """
    result = deepcopy(layout)
    segments = result['segments']
    prelink_index = next(i for i, segment in enumerate(segments) if segment[0] == '__PRELINK_INFO')
    data_start = next(_number(segment[1]) for segment in segments if segment[0] == '__DATA')
    prelink = segments[prelink_index]
    old_start = _number(prelink[1])
    executable_ends = [segment.vmaddr + segment.vmsize - kernel_header
                       for segment in kernel_segments if segment.name in TEXT_EXEC_SEGMENTS and segment.vmsize]
    if not executable_ends:
        raise ValueError('kernel has no executable segments')
    kernel_exec_end = max(executable_ends)
    new_start = _align(max(old_start, kernel_exec_end))
    if new_start == old_start:
        return result, None
    if new_start >= data_start:
        raise ValueError(f'kernel __TEXT_EXEC ends at {new_start:#x}, leaving no __PRELINK_INFO space before __DATA at {data_start:#x}')
    prelink[1] = f'{new_start:#x}'
    prelink[2] = f'{data_start - new_start:#x}'
    return result, (old_start, new_start)
