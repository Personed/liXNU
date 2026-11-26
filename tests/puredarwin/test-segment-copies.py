#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for file-backed kernel segment copies into a collection."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts' / 'kclink'))
from segment_copies import check_kernel_segment_copies


class Segment:
    def __init__(self, name, vmaddr, vmsize, fileoff, filesize):
        self.name = name
        self.vmaddr = vmaddr
        self.vmsize = vmsize
        self.fileoff = fileoff
        self.filesize = filesize


class KernelSegmentCopyTests(unittest.TestCase):
    def test_identical_file_backed_overlap_is_reported_and_safe(self):
        kernel = b'abcdefghijfghij12345'
        segments = [Segment('__TEXT_EXEC', 0x100a, 10, 0, 10),
                    Segment('__KLD', 0x100f, 10, 10, 10)]
        output = bytearray(0x40)
        output[10:20] = kernel[0:10]
        output[15:25] = kernel[10:20]

        ranges, overlaps = check_kernel_segment_copies(segments, kernel, output, 0x1000)

        self.assertEqual([r[0] for r in ranges], ['__TEXT_EXEC', '__KLD'])
        self.assertEqual(overlaps, [('__TEXT_EXEC', '__KLD', 0xf, 0x14)])

    def test_conflicting_file_backed_overlap_fails(self):
        kernel = b'abcdefghijXXXXX12345'
        segments = [Segment('__TEXT_EXEC', 0x100a, 10, 0, 10),
                    Segment('__KLD', 0x100f, 10, 10, 10)]
        output = bytearray(0x40)
        output[10:20] = kernel[0:10]
        output[15:25] = kernel[10:20]

        with self.assertRaisesRegex(
                ValueError,
                r'conflicting file-backed overlap at 0xf-0x14; mapped kernel segment ranges: '
                r'__TEXT_EXEC vmaddr=0x100a vmsize=0xa fileoff=0x0 filesize=0xa collection=0xa-0x14; '
                r'__KLD vmaddr=0x100f vmsize=0xa fileoff=0xa filesize=0xa collection=0xf-0x19'):
            check_kernel_segment_copies(segments, kernel, output, 0x1000)

    def test_output_must_preserve_each_file_backed_segment(self):
        kernel = b'abcdefghij'
        segments = [Segment('__TEXT_EXEC', 0x100a, 10, 0, 10)]
        output = bytearray(0x40)
        output[10:20] = b'XXXXXXXXXX'

        with self.assertRaisesRegex(ValueError, 'copied bytes were overwritten'):
            check_kernel_segment_copies(segments, kernel, output, 0x1000)

    def test_linkedit_is_not_checked_as_a_kernel_segment_copy(self):
        kernel = b''
        segments = [Segment('__LINKEDIT', 0x1000, 0x100, 0, 0x100)]

        ranges, overlaps = check_kernel_segment_copies(segments, kernel, bytearray(), 0x1000)

        self.assertEqual(ranges, [])
        self.assertEqual(overlaps, [])


if __name__ == '__main__':
    unittest.main()
