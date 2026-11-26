#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for exact child RSS measurement in run-tcg-e2e.py."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import time
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("run-tcg-e2e.py")
spec = importlib.util.spec_from_file_location("run_tcg_e2e", MODULE_PATH)
run_tcg_e2e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_tcg_e2e)


class RssMeasurementTest(unittest.TestCase):
    def test_macos_maxrss_is_normalized_from_bytes(self):
        self.assertEqual(run_tcg_e2e.max_rss_kib(1024, "darwin"), 1)
        self.assertEqual(run_tcg_e2e.max_rss_kib(1025, "darwin"), 2)
        self.assertEqual(run_tcg_e2e.max_rss_kib(1024, "linux"), 1024)

    def test_fallback_without_wait4_has_no_rss_measurement(self):
        with mock.patch.object(run_tcg_e2e, "can_measure_child_rss", return_value=False):
            returncode, peak_rss_kib, timed_out = run_tcg_e2e.run_with_timeout([
                sys.executable, "-c", "pass",
            ], 10)

        self.assertEqual(returncode, 0)
        self.assertFalse(timed_out)
        self.assertIsNone(peak_rss_kib)

    @unittest.skipUnless(run_tcg_e2e.can_measure_child_rss(), "wait4 child RSS measurement is unavailable")
    def test_child_rss_excludes_unrelated_processes(self):
        with subprocess.Popen([
                sys.executable, "-c",
                "x=bytearray(128*1024*1024); "
                "[x.__setitem__(i, 1) for i in range(0, len(x), 4096)]; "
                "print('ready', flush=True)",
            ], stdout=subprocess.PIPE, text=True) as unrelated:
            self.assertEqual(unrelated.stdout.readline().strip(), "ready")
            self.assertEqual(unrelated.wait(timeout=10), 0)

        returncode, peak_rss_kib, timed_out = run_tcg_e2e.run_with_timeout([
            sys.executable, "-c",
            "x=bytearray(24*1024*1024); import time; time.sleep(0.3)",
        ], 10)

        self.assertEqual(returncode, 0)
        self.assertFalse(timed_out)
        self.assertGreater(peak_rss_kib, 20 * 1024)
        self.assertLess(peak_rss_kib, 80 * 1024)

    def test_qemu_bare_memory_value_uses_mib(self):
        qemu_with_suffix = run_tcg_e2e.parse_memory_size("6G", "--qemu-memory", 1024**2)
        qemu_without_suffix = run_tcg_e2e.parse_memory_size("6144", "--qemu-memory", 1024**2)
        linux_without_suffix = run_tcg_e2e.parse_memory_size("6144", "--linux-memory")

        self.assertEqual(qemu_without_suffix, qemu_with_suffix)
        self.assertEqual(linux_without_suffix, 6144)

    @unittest.skipUnless(run_tcg_e2e.can_measure_child_rss(), "wait4 child RSS measurement is unavailable")
    def test_timeout_terminates_child_and_reports_rss(self):
        started = time.monotonic()
        returncode, peak_rss_kib, timed_out = run_tcg_e2e.run_with_timeout([
            sys.executable, "-c", "import time; time.sleep(30)",
        ], 1)
        elapsed = time.monotonic() - started

        self.assertTrue(timed_out)
        self.assertLess(elapsed, 10)
        self.assertNotEqual(returncode, 0)
        self.assertGreater(peak_rss_kib, 0)


if __name__ == "__main__":
    unittest.main()
