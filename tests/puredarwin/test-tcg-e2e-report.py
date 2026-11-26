#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the cross-host PureDarwin boot report."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "tests" / "puredarwin" / "run-tcg-e2e.py"
INIT_REPORT = ROOT / "tests" / "puredarwin" / "init-host-report.sh"


def input_files(directory: Path, *, with_disk: bool) -> tuple[Path, Path, Path, Path]:
    inputs = directory / "inputs"
    inputs.mkdir()
    kernel = inputs / "Image"
    initramfs = inputs / "initramfs.cpio.gz"
    disk = inputs / "pd.img"
    kernel.write_bytes(b"test kernel")
    initramfs.write_bytes(b"test initramfs")
    if with_disk:
        disk.write_bytes(b"test PureDarwin disk")

    disk_hash = hashlib.sha256(disk.read_bytes()).hexdigest() if with_disk else "a" * 64
    (inputs / "build-manifest.txt").write_text(
        f"puredarwin_disk_sha256={disk_hash}\n", encoding="utf-8"
    )
    (inputs / "guest-manifest.txt").write_text(
        "arm64_guest_kernel_release=test-kernel\n"
        "arm64_guest_page_size=4096\n"
        "arm64_guest_pac=available\n"
        "arm64_guest_exception_level=EL1\n",
        encoding="utf-8",
    )
    return kernel, initramfs, disk, inputs


def environment_for(temp: Path) -> dict[str, str]:
    runner_temp = temp / "runner-temp"
    runner_temp.mkdir()
    env = os.environ.copy()
    env.update({
        "RUNNER_TEMP": str(runner_temp),
        "RUNNER_OS": "Linux",
        "XNULINUX_RUNNER_LABEL": "report-test",
        "GITHUB_STEP_SUMMARY": str(temp / "summary.md"),
    })
    (runner_temp / "puredarwin-tcg-boot-report.json").write_text(
        json.dumps({"host": {"memory_available_before_artifact_preparation_bytes": 9_876_543_210}}),
        encoding="utf-8",
    )
    return env


class TcgE2eReportTests(unittest.TestCase):
    def test_posix_preflight_reports_decimal_byte_counts_and_phase(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            env = environment_for(temp)
            subprocess.run(["bash", str(INIT_REPORT)], env=env, check=True, capture_output=True, text=True)

            report_path = Path(env["RUNNER_TEMP"]) / "puredarwin-tcg-boot-report.json"
            report = json.loads(report_path.read_text(encoding="utf-8"))
            host = report["host"]

            self.assertIsInstance(host["memory_total_bytes"], int)
            self.assertIsInstance(host["memory_available_before_artifact_preparation_bytes"], int)
            self.assertNotIn("memory_available_at_report_bytes", host)
            self.assertIn("before artifact preparation", report["preparation_steps"]["status"])

    def test_missing_disk_hash_records_failure_instead_of_raising(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            kernel, initramfs, disk, _ = input_files(temp, with_disk=False)
            env = environment_for(temp)
            serial_log = temp / "serial.log"
            result = subprocess.run(
                [sys.executable, str(RUNNER), str(kernel), str(initramfs), str(disk), str(serial_log),
                 "--qemu", "qemu-that-does-not-exist"],
                env=env, capture_output=True, text=True, check=False,
            )

            report = json.loads(
                (Path(env["RUNNER_TEMP"]) / "puredarwin-tcg-boot-report.json").read_text(encoding="utf-8")
            )
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("Traceback", result.stderr)
            self.assertIn("could not be computed", " ".join(report["result"]["errors"]))
            self.assertEqual(report["host"]["memory_available_before_artifact_preparation_bytes"], 9_876_543_210)
            self.assertIsNone(report["host"]["memory_available_before_qemu_bytes"])
            self.assertIsNone(report["host"]["memory_available_after_qemu_attempt_bytes"])

    def test_report_labels_memory_before_and_after_qemu_attempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            kernel, initramfs, disk, inputs = input_files(temp, with_disk=True)
            env = environment_for(temp)
            fake_qemu = temp / "qemu-system-aarch64"
            fake_qemu.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = --version ]; then echo 'QEMU emulator version report-test'; exit 0; fi\n"
                "exit 0\n",
                encoding="utf-8",
            )
            fake_qemu.chmod(0o755)
            serial_log = temp / "serial.log"
            result = subprocess.run(
                [sys.executable, str(RUNNER), str(kernel), str(initramfs), str(disk), str(serial_log),
                 "--timeout-seconds", "5", "--qemu", str(fake_qemu)],
                env=env, capture_output=True, text=True, check=False,
            )

            report = json.loads(
                (Path(env["RUNNER_TEMP"]) / "puredarwin-tcg-boot-report.json").read_text(encoding="utf-8")
            )
            summary = Path(env["GITHUB_STEP_SUMMARY"]).read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 1)  # The fake QEMU emits no guest markers.
            self.assertEqual(report["qemu_version"], "QEMU emulator version report-test")
            self.assertEqual(report["host"]["memory_available_before_artifact_preparation_bytes"], 9_876_543_210)
            self.assertIsInstance(report["host"]["memory_available_before_qemu_bytes"], int)
            self.assertIsInstance(report["host"]["memory_available_after_qemu_attempt_bytes"], int)
            self.assertIn("before artifact preparation", summary)
            self.assertIn("before QEMU", summary)
            self.assertIn("after QEMU attempt", summary)


if __name__ == "__main__":
    unittest.main()
