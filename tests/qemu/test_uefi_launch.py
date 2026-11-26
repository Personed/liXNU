#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Start QEMU through the launcher and verify its UEFI guest stays running."""

import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UefiLaunchTest(unittest.TestCase):
    def start_guest(self, graphical=False):
        if not shutil.which("qemu-system-aarch64") or not shutil.which("qemu-img"):
            self.skipTest("QEMU system emulation tools are not installed")
        firmware_code = Path(os.environ.get(
            "QEMU_FIRMWARE_CODE", "/usr/share/AAVMF/AAVMF_CODE.fd"
        ))
        firmware_vars_template = Path(os.environ.get(
            "QEMU_FIRMWARE_VARS", "/usr/share/AAVMF/AAVMF_VARS.fd"
        ))
        self.assertTrue(firmware_code.is_file(), f"missing UEFI code image: {firmware_code}")
        self.assertTrue(firmware_vars_template.is_file(), f"missing UEFI variables template: {firmware_vars_template}")

        with tempfile.TemporaryDirectory() as directory:
            vm_dir = Path(directory)
            disk = vm_dir / "guest.qcow2"
            firmware_vars = vm_dir / "guest-vars.fd"
            subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", str(disk), "1G"], check=True)
            firmware_vars.write_bytes(firmware_vars_template.read_bytes())

            command = [
                sys.executable,
                str(ROOT / "scripts/qemu-vm.py"),
                str(disk),
                "--firmware-code", str(firmware_code),
                "--firmware-vars", str(firmware_vars),
                "--memory", "1024",
                "--cpus", "1",
            ]
            if graphical:
                command.append("--graphical")
            qemu = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                qemu.wait(timeout=15)
            except subprocess.TimeoutExpired:
                qemu.send_signal(signal.SIGTERM)
                output, _ = qemu.communicate(timeout=10)
                self.assertEqual(qemu.returncode, 0, output)
                self.assertIn("UEFI Interactive Shell", output)
            else:
                output, _ = qemu.communicate()
                self.fail(f"QEMU exited before the UEFI guest could be stopped ({qemu.returncode}):\n{output}")

    def test_launcher_starts_headless_uefi_guest(self):
        self.start_guest()

    @unittest.skipUnless(os.environ.get("QEMU_TEST_GRAPHICAL") == "1", "graphical QEMU CI is not enabled")
    def test_launcher_starts_graphical_uefi_guest(self):
        self.start_guest(graphical=True)


if __name__ == "__main__":
    unittest.main()
