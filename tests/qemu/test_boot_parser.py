#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the QEMU console success marker parser."""

import unittest

from boot import reports_smoke_success


class SmokeMarkerTests(unittest.TestCase):
    def test_standalone_marker(self):
        self.assertTrue(reports_smoke_success("xnulinux QEMU smoke test passed\n"))

    def test_real_shutdown_line_appended(self):
        console = "xnulinux QEMU smoke test passed[    6.206859] reboot: Power down\n"
        self.assertTrue(reports_smoke_success(console))

    def test_shutdown_line_without_kernel_timestamp(self):
        console = "xnulinux QEMU smoke test passedreboot: Power down\n"
        self.assertTrue(reports_smoke_success(console))

    def test_arbitrary_suffix_rejected(self):
        self.assertFalse(reports_smoke_success("xnulinux QEMU smoke test passedFAILURE\n"))

    def test_arbitrary_timestamped_suffix_rejected(self):
        console = "xnulinux QEMU smoke test passed[    6.206859] unrelated output\n"
        self.assertFalse(reports_smoke_success(console))

    def test_failure_marker_rejected(self):
        self.assertFalse(reports_smoke_success("xnulinux QEMU smoke test failed\n"))

    def test_embedded_marker_rejected(self):
        self.assertFalse(reports_smoke_success("kernel: xnulinux QEMU smoke test passed\n"))


if __name__ == "__main__":
    unittest.main()
