#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the QEMU guest success-marker parser."""

import unittest

from boot import SUCCESS_MARKER, reports_smoke_success


class GuestReportedSuccessTests(unittest.TestCase):
    def test_marker_with_linux_poweroff_text_on_same_line(self):
        serial = f"{SUCCESS_MARKER}[    6.370815] reboot: Power down\n"
        self.assertTrue(reports_smoke_success(serial))

    def test_standalone_marker(self):
        self.assertTrue(reports_smoke_success(f"{SUCCESS_MARKER}\n"))

    def test_missing_marker(self):
        self.assertFalse(reports_smoke_success("Linux booted\nreboot: Power down\n"))

    def test_duplicate_marker(self):
        self.assertFalse(reports_smoke_success(f"{SUCCESS_MARKER}\n{SUCCESS_MARKER}\n"))

    def test_marker_must_begin_a_serial_line(self):
        self.assertFalse(reports_smoke_success(f"prefix {SUCCESS_MARKER}\n"))


if __name__ == "__main__":
    unittest.main()
