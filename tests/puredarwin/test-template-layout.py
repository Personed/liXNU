#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for fitting the empty free-KC template around source-built XNU."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts' / 'free-kc'))
from template_layout import adjust_prelink_info


class Segment:
    def __init__(self, name, vmaddr, vmsize):
        self.name, self.vmaddr, self.vmsize = name, vmaddr, vmsize


def fixture_layout():
    return {'segments': [
        ['__TEXT', '0x0', '0x4000', 1, None],
        ['__TEXT_EXEC', '0xf94000', '0x28d0000', 5, None],
        ['__PRELINK_INFO', '0x3864000', '0x2fc000', 3, '__info'],
        ['__DATA', '0x3b60000', '0x2c0000', 3, None],
    ]}


class TemplateLayoutTests(unittest.TestCase):
    def test_moves_prelink_after_an_oversized_kernel_text_exec(self):
        layout = fixture_layout()
        header = 0x100000000
        kernel = [Segment('__TEXT_EXEC', header + 0x2df0000, 0xae4000),
                  Segment('__KLD', header + 0x3854000, 0xc000),
                  Segment('__LAST', header + 0x3860000, 0x4000)]

        adjusted, moved = adjust_prelink_info(layout, header, kernel)

        self.assertEqual(moved, (0x3864000, 0x38d4000))
        prelink = next(s for s in adjusted['segments'] if s[0] == '__PRELINK_INFO')
        data = next(s for s in adjusted['segments'] if s[0] == '__DATA')
        self.assertEqual(int(prelink[1], 16), 0x38d4000)
        self.assertEqual(int(prelink[2], 16), int(data[1], 16) - 0x38d4000)
        self.assertEqual(layout['segments'][2][1], '0x3864000')

    def test_keeps_reference_layout_when_kernel_fits(self):
        layout = fixture_layout()
        header = 0x100000000
        kernel = [Segment('__TEXT_EXEC', header + 0x2df0000, 0xa00000),
                  Segment('__KLD', header + 0x3854000, 0xc000),
                  Segment('__LAST', header + 0x3860000, 0x4000)]

        adjusted, moved = adjust_prelink_info(layout, header, kernel)

        self.assertIsNone(moved)
        self.assertEqual(adjusted['segments'][2][1], '0x3864000')

    def test_aligns_growth_to_collection_page_size(self):
        layout = fixture_layout()
        header = 0x100000000
        kernel = [Segment('__TEXT_EXEC', header + 0x2df0000, 0xae4001)]

        adjusted, moved = adjust_prelink_info(layout, header, kernel)

        self.assertEqual(moved, (0x3864000, 0x38d8000))
        self.assertEqual(int(adjusted['segments'][2][1], 16), 0x38d8000)

    def test_fails_when_kernel_consumes_prelink_space(self):
        layout = fixture_layout()
        header = 0x100000000
        kernel = [Segment('__TEXT_EXEC', header + 0x2df0000, 0x2d00000)]

        with self.assertRaisesRegex(ValueError, 'leaving no __PRELINK_INFO space'):
            adjust_prelink_info(layout, header, kernel)

    def test_fails_without_an_executable_kernel_segment(self):
        with self.assertRaisesRegex(ValueError, 'kernel has no executable segments'):
            adjust_prelink_info(fixture_layout(), 0x100000000, [Segment('__TEXT', 0x100004000, 0x4000)])


if __name__ == '__main__':
    unittest.main()
