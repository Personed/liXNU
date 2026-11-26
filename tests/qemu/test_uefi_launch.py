#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Start QEMU through the launcher and verify its UEFI guest stays running."""

import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UefiLaunchTest(unittest.TestCase):
    def terminate_qemu(self, qemu, force_tree=False):
        if os.name == "nt":
            if qemu.poll() is None or force_tree:
                try:
                    subprocess.run(
                        ["taskkill", "/PID", str(qemu.pid), "/T", "/F"],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                        timeout=10,
                        check=False,
                    )
                except (OSError, subprocess.TimeoutExpired):
                    if qemu.poll() is None:
                        qemu.kill()
        elif qemu.poll() is None:
            qemu.send_signal(signal.SIGTERM)

    def firmware_pair(self):
        code_override = os.environ.get("QEMU_FIRMWARE_CODE")
        vars_override = os.environ.get("QEMU_FIRMWARE_VARS")
        if code_override or vars_override:
            self.assertTrue(code_override and vars_override, "set both QEMU_FIRMWARE_CODE and QEMU_FIRMWARE_VARS")
            code, variables = Path(code_override), Path(vars_override)
            self.assertTrue(code.is_file(), f"missing UEFI code image: {code}")
            self.assertTrue(variables.is_file(), f"missing UEFI variables template: {variables}")
            return code, variables

        pairs = (
            ("AAVMF_CODE.fd", "AAVMF_VARS.fd"),
            ("edk2-aarch64-code.fd", "edk2-arm-vars.fd"),
            ("edk2-aarch64-code.fd", "edk2-aarch64-vars.fd"),
        )
        directories = [Path("/usr/share/AAVMF")]
        qemu = shutil.which("qemu-system-aarch64")
        if qemu:
            result = subprocess.run(
                [qemu, "-L", "help"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                check=False,
            )
            directories.extend(Path(line.strip()) for line in (result.stdout or "").splitlines()
                               if line.strip())
        for directory in directories:
            for code_name, vars_name in pairs:
                code = directory / code_name
                variables = directory / vars_name
                if code.is_file() and variables.is_file():
                    return code, variables
        self.fail("could not find matching AArch64 UEFI images; see `qemu-system-aarch64 -L help`")

    def wait_for_qemu_window(self, qemu, expected_title):
        deadline = time.monotonic() + 15
        tree_output = ""
        title_pattern = re.compile(r'^\s*(0x[0-9a-fA-F]+)\s+"([^\"]+)"')
        while time.monotonic() < deadline and qemu.poll() is None:
            try:
                tree = subprocess.run(
                    ["xwininfo", "-root", "-tree"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    timeout=2,
                    check=False,
                )
                tree_output = tree.stdout or ""
                for line in tree_output.splitlines():
                    match = title_pattern.match(line)
                    if not match or match.group(2) != expected_title:
                        continue
                    detail = subprocess.run(
                        ["xwininfo", "-id", match.group(1)],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                        timeout=2,
                        check=False,
                    )
                    detail_output = detail.stdout or ""
                    width = self._window_dimension(detail_output, "Width")
                    height = self._window_dimension(detail_output, "Height")
                    if (
                        detail.returncode == 0
                        and "Map State: IsViewable" in detail_output
                        and width >= 640
                        and height >= 480
                    ):
                        return True, tree_output
            except (OSError, subprocess.TimeoutExpired) as error:
                tree_output = f"{tree_output}\nxwininfo failed: {error}"
            time.sleep(0.25)
        return False, tree_output

    @staticmethod
    def _window_dimension(output, name):
        match = re.search(rf"^\s*{name}:\s*(\d+)\s*$", output, re.MULTILINE)
        return int(match.group(1)) if match else 0

    def start_guest(self, graphical=False):
        qemu_binary = shutil.which("qemu-system-aarch64")
        missing = []
        if not qemu_binary:
            missing.append("qemu-system-aarch64")
        if not shutil.which("qemu-img"):
            missing.append("qemu-img")
        if missing:
            message = "missing QEMU executable(s): " + ", ".join(missing)
            if os.environ.get("QEMU_TEST_REQUIRED") == "1":
                self.fail(message)
            self.skipTest(message)
        if graphical and not shutil.which("xwininfo"):
            if os.environ.get("QEMU_TEST_REQUIRED") == "1":
                self.fail("missing xwininfo; install x11-utils to validate the graphical window")
            self.skipTest("xwininfo is not installed")
        firmware_code, firmware_vars_template = self.firmware_pair()

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
                "--qemu", qemu_binary,
                "--firmware-code", str(firmware_code),
                "--firmware-vars", str(firmware_vars),
                "--memory", "1024",
                "--cpus", "1",
            ]
            window_title = "xnulinux-uefi-window-test"
            if graphical:
                command += ["--graphical", "--window-title", window_title]
            qemu = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            window_found = False
            window_output = ""
            timed_out = False
            try:
                if graphical:
                    window_found, window_output = self.wait_for_qemu_window(qemu, window_title)
                try:
                    qemu.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    timed_out = True
            finally:
                self.terminate_qemu(qemu)
                try:
                    output, _ = qemu.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    self.terminate_qemu(qemu, force_tree=True)
                    if qemu.poll() is None:
                        qemu.kill()
                    output, _ = qemu.communicate(timeout=10)
            if not timed_out:
                self.fail(f"QEMU exited before the UEFI guest could be stopped ({qemu.returncode}):\n{output}")
            self.assertIsNotNone(qemu.returncode, output)
            if os.name != "nt":
                self.assertEqual(qemu.returncode, 0, output)
            self.assertIn("UEFI Interactive Shell", output)
            if graphical:
                self.assertTrue(window_found, "visible QEMU top-level window did not appear in Xvfb:\n" + window_output)

    def test_launcher_starts_headless_uefi_guest(self):
        self.start_guest()

    @unittest.skipUnless(os.environ.get("QEMU_TEST_GRAPHICAL") == "1", "graphical QEMU CI is not enabled")
    def test_launcher_starts_graphical_uefi_guest(self):
        self.start_guest(graphical=True)


if __name__ == "__main__":
    unittest.main()
