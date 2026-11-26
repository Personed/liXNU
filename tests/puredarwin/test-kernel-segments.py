#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the free-collection kernel segment layout and writes."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts' / 'free-kc'))
sys.path.insert(0, str(ROOT / 'scripts' / 'kclink'))

from fit_kernel_segments import fit_offsets, parse_link_offsets, render_link_offsets  # noqa: E402
from macho import Segment  # noqa: E402


class KernelSegmentTests(unittest.TestCase):
    def kernel_segments(self, text_exec_size=0xae4000, klddata_start=0x3a74000):
        header = 0x100000000
        return header, [
            Segment(name='__TEXT_EXEC', vmaddr=header + 0x2df0000, vmsize=text_exec_size),
            Segment(name='__KLD', vmaddr=header + 0x3854000, vmsize=0xc000),
            Segment(name='__LAST', vmaddr=header + 0x3860000, vmsize=0x4000),
            Segment(name='__KLDDATA', vmaddr=header + klddata_start, vmsize=0x8000),
        ]

    def test_moves_kld_and_last_past_measured_text_exec(self):
        header, segments = self.kernel_segments()
        fitted = fit_offsets(segments, header, {'__KLD': 0x3854000, '__LAST': 0x3860000})
        self.assertEqual(fitted, {'__KLD': 0x38d4000, '__LAST': 0x38e0000})

    def test_keeps_safe_marker_addresses(self):
        header, segments = self.kernel_segments(text_exec_size=0xa00000)
        old = {'__KLD': 0x3854000, '__LAST': 0x3860000}
        self.assertEqual(fit_offsets(segments, header, old), old)

    def test_fails_when_markers_cannot_fit_before_klddata(self):
        header, segments = self.kernel_segments(text_exec_size=0x1e00000, klddata_start=0x3a74000)
        with self.assertRaisesRegex(ValueError, 'overlap|beyond __KLDDATA'):
            fit_offsets(segments, header, {'__KLD': 0x3854000, '__LAST': 0x3860000})

    def test_rewrites_only_marker_addresses_in_linker_script(self):
        source = 'a __TEXT_EXEC 0x2df0000; a __KLD 0x3854000; a __LAST 0x3860000; o=0x3cc4000;\n'
        self.assertEqual(parse_link_offsets(source), {
            '__TEXT_EXEC': 0x2df0000, '__KLD': 0x3854000, '__LAST': 0x3860000,
        })
        rendered = render_link_offsets(source, {'__KLD': 0x38d4000, '__LAST': 0x38e0000})
        self.assertEqual(rendered, 'a __TEXT_EXEC 0x2df0000; a __KLD 0x38d4000; a __LAST 0x38e0000; o=0x3cc4000;\n')

if __name__ == '__main__':
    unittest.main()
