#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the unified xnulinux CLI and install.sh."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CLI = ROOT / "scripts" / "xnulinux"
INSTALLER = ROOT / "scripts" / "install.sh"


class TestCliAndInstaller(unittest.TestCase):
    def test_help_doctor_status(self):
        for cmd in ("help", "doctor", "status"):
            res = subprocess.run([str(CLI), cmd], capture_output=True, text=True, check=True)
            self.assertTrue(res.stdout.strip())

    def test_screenshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            fb = os.path.join(tmp, "fb.raw")
            out = os.path.join(tmp, "out.png")
            with open(fb, "wb") as f:
                f.write(b"\x00\x80\xff\xff" * 4)
            env = {**os.environ, "FB_DUMP": fb}
            subprocess.run([str(CLI), "screenshot", out, "2", "2"], env=env, check=True, capture_output=True)
            with open(out, "rb") as f:
                self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")

    def test_missing_optional_scripts_report_cleanly(self):
        res_vnc = subprocess.run([str(CLI), "vnc"], capture_output=True, text=True)
        self.assertNotEqual(res_vnc.returncode, 0)
        self.assertIn("xnu-vnc.py", res_vnc.stderr)

        res_vm = subprocess.run([str(CLI), "vm"], capture_output=True, text=True)
        self.assertNotEqual(res_vm.returncode, 0)
        self.assertIn("qemu-vm.py", res_vm.stderr)

        res_con = subprocess.run([str(CLI), "console", "--local"], capture_output=True, text=True)
        self.assertNotEqual(res_con.returncode, 0)
        self.assertIn("--local", res_con.stderr)

    def test_non_arm64_guards_boot_and_setup_mem(self):
        if os.uname().machine == "aarch64":
            self.skipTest("host is aarch64")
        res_boot = subprocess.run([str(CLI), "boot"], capture_output=True, text=True)
        self.assertNotEqual(res_boot.returncode, 0)
        self.assertIn("ARM64", res_boot.stderr)

        res_mem = subprocess.run([str(CLI), "setup-mem", "--dry-run"], capture_output=True, text=True)
        self.assertNotEqual(res_mem.returncode, 0)
        self.assertIn("ARM64", res_mem.stderr)

    def test_install_and_uninstall(self):
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([str(INSTALLER), "--prefix", tmp], check=True, capture_output=True, text=True)
            installed = pathlib.Path(tmp) / "bin" / "xnulinux"
            self.assertTrue(installed.is_file())
            res = subprocess.run([str(installed), "doctor"], check=True, capture_output=True, text=True)
            self.assertIn("=== xnulinux PC check ===", res.stdout)
            subprocess.run([str(INSTALLER), "--prefix", tmp, "--uninstall"], check=True, capture_output=True, text=True)
            self.assertFalse(installed.exists())


if __name__ == "__main__":
    unittest.main()
