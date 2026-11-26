#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the free-collection kernel segment layout and writes."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts' / 'free-kc'))
sys.path.insert(0, str(ROOT / 'scripts' / 'kclink'))

from fit_kernel_segments import fit_offsets, parse_link_offsets, relink_needed, render_link_offsets  # noqa: E402
from macho import Segment  # noqa: E402


class KernelSegmentTests(unittest.TestCase):
    def kernel_segments(self, text_exec_size=0xae4000, klddata_start=0x3a74000, kld_size=0xc000,
                        last_size=0x4000, include_last=True):
        header = 0x100000000
        segments = [
            Segment(name='__TEXT', vmaddr=header, vmsize=0x10000),
            Segment(name='__TEXT_EXEC', vmaddr=header + 0x2df0000, vmsize=text_exec_size),
            Segment(name='__KLD', vmaddr=header + 0x3854000, vmsize=kld_size),
            Segment(name='__KLDDATA', vmaddr=header + klddata_start, vmsize=0x8000),
        ]
        if include_last:
            segments.append(Segment(name='__LAST', vmaddr=header + 0x3860000, vmsize=last_size))
        return header, segments

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

    def test_nonmarker_segments_use_measured_macho_addresses(self):
        header, segments = self.kernel_segments(kld_size=0x4000)
        # The Mach-O puts __DATA directly on the fitted __KLD. A stale linker
        # script address must not move it out of the overlap check.
        segments.append(Segment(name='__DATA', vmaddr=header + 0x38d4000, vmsize=0x4000))
        offsets = {'__KLD': 0x3854000, '__LAST': 0x3860000, '__DATA': 0x3900000}
        with self.assertRaisesRegex(ValueError, 'overlap after relink placement'):
            fit_offsets(segments, header, offsets)

    def test_zero_size_last_marker_can_be_fitted(self):
        header, segments = self.kernel_segments(kld_size=0x4000, last_size=0)
        fitted = fit_offsets(segments, header, {'__KLD': 0x3854000, '__LAST': 0x3860000})
        self.assertEqual(fitted, {'__KLD': 0x38d4000, '__LAST': 0x38d8000})

    def test_missing_last_segment_can_be_fitted(self):
        header, segments = self.kernel_segments(kld_size=0x4000, include_last=False)
        fitted = fit_offsets(segments, header, {'__KLD': 0x3854000, '__LAST': 0x3860000})
        self.assertEqual(fitted, {'__KLD': 0x38d4000, '__LAST': 0x38d8000})

    def test_updated_linker_script_with_stale_kernel_requests_relink(self):
        header, segments = self.kernel_segments(kld_size=0x4000, last_size=0)
        fitted = {'__KLD': 0x38d4000, '__LAST': 0x38d8000}
        self.assertEqual(fit_offsets(segments, header, fitted), fitted)
        self.assertTrue(relink_needed(segments, header, fitted))

        segments[2].vmaddr = header + fitted['__KLD']
        segments[-1].vmaddr = header + fitted['__LAST']
        self.assertFalse(relink_needed(segments, header, fitted))

    def test_zero_size_last_at_stale_address_still_requests_relink(self):
        header, segments = self.kernel_segments(kld_size=0x4000, last_size=0)
        fitted = {'__KLD': 0x38d4000, '__LAST': 0x38d8000}
        segments[2].vmaddr = header + fitted['__KLD']
        self.assertTrue(relink_needed(segments, header, fitted))

        segments[-1].vmaddr = header + fitted['__LAST']
        self.assertFalse(relink_needed(segments, header, fitted))

    def test_missing_last_does_not_make_already_relinked_kernel_stale(self):
        header, segments = self.kernel_segments(kld_size=0x4000, include_last=False)
        fitted = {'__KLD': 0x38d4000, '__LAST': 0x38d8000}
        segments[2].vmaddr = header + fitted['__KLD']
        self.assertFalse(relink_needed(segments, header, fitted))

    def test_rewrites_only_marker_addresses_in_linker_script(self):
        source = 'a __TEXT_EXEC 0x2df0000; a __KLD 0x3854000; a __LAST 0x3860000; o=0x3cc4000;\n'
        self.assertEqual(parse_link_offsets(source), {
            '__TEXT_EXEC': 0x2df0000, '__KLD': 0x3854000, '__LAST': 0x3860000,
        })
        rendered = render_link_offsets(source, {'__KLD': 0x38d4000, '__LAST': 0x38e0000})
        self.assertEqual(rendered, 'a __TEXT_EXEC 0x2df0000; a __KLD 0x38d4000; a __LAST 0x38e0000; o=0x3cc4000;\n')

if __name__ == '__main__':
    unittest.main()
