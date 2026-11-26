#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for exact child RSS measurement in run-tcg-e2e.py."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import time
import unittest


MODULE_PATH = Path(__file__).with_name("run-tcg-e2e.py")
spec = importlib.util.spec_from_file_location("run_tcg_e2e", MODULE_PATH)
run_tcg_e2e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_tcg_e2e)


class RssMeasurementTest(unittest.TestCase):
    def test_child_rss_excludes_unrelated_processes(self):
        unrelated = subprocess.Popen([
            sys.executable, "-c",
            "x=bytearray(128*1024*1024); import time; time.sleep(3)",
        ])
        try:
            returncode, peak_rss_kib, timed_out = run_tcg_e2e.run_with_timeout([
                sys.executable, "-c",
                "x=bytearray(24*1024*1024); import time; time.sleep(0.3)",
            ], 10)
        finally:
            unrelated.terminate()
            unrelated.wait()

        self.assertEqual(returncode, 0)
        self.assertFalse(timed_out)
        self.assertGreater(peak_rss_kib, 20 * 1024)
        self.assertLess(peak_rss_kib, 80 * 1024)

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
