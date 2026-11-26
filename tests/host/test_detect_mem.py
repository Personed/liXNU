#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for scripts/detect-mem.py."""

import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("detect_mem", ROOT / "scripts" / "detect-mem.py")
detect_mem = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(detect_mem)


class TestDetectMem(unittest.TestCase):
    def test_16g_qemu_virt(self):
        iomem = "40000000-43fffffff : System RAM\n  40200000-41ffffff : Kernel code\n"
        layout = detect_mem.compute_layout(detect_mem.parse_iomem(iomem))
        self.assertEqual(layout["linux_mem"], "7G")
        self.assertEqual(layout["carve_base"], "0x200000000")
        self.assertEqual(layout["carve_mb"], 8156)
        self.assertEqual(layout["ram_mb"], 7900)

    def test_24g_qemu_virt(self):
        iomem = "40000000-63fffffff : System RAM\n"
        layout = detect_mem.compute_layout(detect_mem.parse_iomem(iomem))
        self.assertEqual(layout["linux_mem"], "15G")
        self.assertEqual(layout["carve_base"], "0x400000000")
        self.assertEqual(layout["carve_mb"], 8156)
        self.assertEqual(layout["ram_mb"], 7900)

    def test_8g_zero_based_sbc(self):
        iomem = "00000000-1ffffffff : System RAM\n"
        layout = detect_mem.compute_layout(detect_mem.parse_iomem(iomem))
        self.assertEqual(layout["linux_mem"], "3G")
        self.assertEqual(layout["carve_base"], "0xc0000000")
        self.assertEqual(layout["carve_mb"], 4096)
        self.assertEqual(layout["ram_mb"], 3840)

    def test_32g_2g_based(self):
        iomem = "80000000-87fffffff : System RAM\n"
        layout = detect_mem.compute_layout(detect_mem.parse_iomem(iomem))
        self.assertEqual(layout["linux_mem"], "23G")
        self.assertEqual(layout["carve_base"], "0x640000000")
        self.assertEqual(layout["carve_mb"], 8156)

    def test_fragmented_two_regions(self):
        iomem = (
            "80000000-ffffffff : System RAM\n"
            "100000000-17fffffff : PCI Bus\n"
            "880000000-c7fffffff : System RAM\n"
        )
        layout = detect_mem.compute_layout(detect_mem.parse_iomem(iomem))
        self.assertEqual(layout["linux_mem"], "9G")
        # 2 GiB in first region + 7 GiB in second region (0x880000000 + 0x1c0000000 = 0xa40000000)
        self.assertEqual(layout["carve_base"], "0xa40000000")
        self.assertEqual(layout["carve_mb"], 8156)

    def test_fragmented_regions_with_rounding_below_prior_memory(self):
        iomem = (
            "80000000-17fffffff : System RAM\n"
            "200000000-31fffffff : System RAM\n"
            "800010000-a4000ffff : System RAM\n"
        )
        ranges = detect_mem.parse_iomem(iomem)
        layout = detect_mem.compute_layout(ranges)
        carve_base = int(layout["carve_base"], 16)
        carve_end = carve_base + layout["carve_mb"] * detect_mem.MIB
        self.assertEqual(layout["linux_mem"], "8G")
        self.assertTrue(any(start <= carve_base and carve_end <= end for start, end in ranges))
        self.assertEqual(carve_base, detect_mem.align_up(ranges[-1][0], detect_mem.ALIGN_2M))

    def test_validate_detected_16g_layout(self):
        ranges = detect_mem.parse_iomem("40000000-43fffffff : System RAM\n")
        detect_mem.validate_layout(ranges, "7G", "0x200000000", "8156", "7900")

    def test_reject_linux_mem_that_reaches_into_carve_out(self):
        ranges = detect_mem.parse_iomem("40000000-43fffffff : System RAM\n")
        with self.assertRaisesRegex(ValueError, "lower LINUX_MEM or move CARVE_BASE higher"):
            detect_mem.validate_layout(ranges, "8G", "0x200000000", "8156")

    def test_validate_explicit_carve_and_linux_overrides(self):
        ranges = detect_mem.parse_iomem("40000000-43fffffff : System RAM\n")
        detect_mem.validate_layout(ranges, "8G", "0x240000000", "2048", "1800")

    def test_reject_ram_mb_larger_than_carve_supports(self):
        ranges = detect_mem.parse_iomem("40000000-43fffffff : System RAM\n")
        with self.assertRaisesRegex(ValueError, "supported limit of 1920 MiB"):
            detect_mem.validate_layout(ranges, "8G", "0x240000000", "2048", "1921")

    def test_masked_iomem_raises(self):
        iomem = "00000000-00000000 : System RAM\n00000000-00000000 : System RAM\n"
        with self.assertRaisesRegex(ValueError, "masked"):
            detect_mem.parse_iomem(iomem)

    def test_too_small_ram_raises(self):
        iomem = "40000000-bfffffff : System RAM\n"
        with self.assertRaisesRegex(ValueError, "at least 4 GiB"):
            detect_mem.compute_layout(detect_mem.parse_iomem(iomem))


if __name__ == "__main__":
    unittest.main()
