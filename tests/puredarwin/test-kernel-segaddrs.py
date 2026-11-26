#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for relocating the source kernel's executable link slots."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts' / 'free-kc'))
import importlib.util
spec = importlib.util.spec_from_file_location('kernel_segaddrs', ROOT / 'scripts' / 'free-kc' / 'kernel-segaddrs.py')
kernel_segaddrs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel_segaddrs)
plan_executable_slots = kernel_segaddrs.plan_executable_slots
rewrite_segaddrs = kernel_segaddrs.rewrite_segaddrs


BASE = 0xfffffff007004000
OFFSETS = {
    '__TEXT': 0x4000,
    '__TEXT_EXEC': 0x2df0000,
    '__KLD': 0x3854000,
    '__LAST': 0x3860000,
    '__KLDDATA': 0x3a74000,
}


def segments(text_size=0xae4000, kld=None, last=None):
    return {
        '__TEXT': (BASE + OFFSETS['__TEXT'], 0x8c000),
        '__TEXT_EXEC': (BASE + OFFSETS['__TEXT_EXEC'], text_size),
        '__KLD': (BASE + OFFSETS['__KLD'], 0x4000) if kld is None else kld,
        '__LAST': (BASE + OFFSETS['__LAST'], 0) if last is None else last,
    }


class KernelSegaddrsTests(unittest.TestCase):
    def test_moves_kld_after_oversized_text_exec(self):
        plan = plan_executable_slots(segments(), BASE, OFFSETS)

        self.assertEqual(plan['offsets']['__KLD'], 0x38d4000)
        self.assertEqual(plan['offsets']['__LAST'], 0x38d8000)
        self.assertEqual(plan['new']['__KLD'], BASE + 0x38d4000)
        self.assertEqual(plan['new']['__LAST'], BASE + 0x38d8000)

    def test_keeps_separated_slots(self):
        local = dict(OFFSETS)
        local['__KLD'] = 0x38d4000
        local['__LAST'] = 0x38d8000
        moved = segments(kld=(BASE + local['__KLD'], 0x4000), last=(BASE + local['__LAST'], 0))

        self.assertIsNone(plan_executable_slots(moved, BASE, local))

    def test_fails_when_relocation_reaches_data(self):
        local = dict(OFFSETS)
        local['__KLDDATA'] = 0x38d4000

        with self.assertRaisesRegex(ValueError, 'reach __KLDDATA'):
            plan_executable_slots(segments(), BASE, local)

    def test_rewrite_replaces_only_named_assignments(self):
        source = 'a __TEXT 0x4000; a __KLD 0x3854000; a __LAST 0x3860000;\n'

        rewritten = rewrite_segaddrs(source, {'__KLD': 0x38d4000, '__LAST': 0x38d8000})

        self.assertEqual(rewritten, 'a __TEXT 0x4000; a __KLD 0x38d4000; a __LAST 0x38d8000;\n')


if __name__ == '__main__':
    unittest.main()
