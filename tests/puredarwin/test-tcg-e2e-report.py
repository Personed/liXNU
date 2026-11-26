#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the cross-host PureDarwin boot report."""

import hashlib
import importlib.util
import io
import json
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "tests" / "puredarwin" / "run-tcg-e2e.py"
INIT_REPORT = ROOT / "tests" / "puredarwin" / "init-host-report.sh"
PYTHON_MEMORY_SAMPLES = (
    5 * 1024 ** 3 + 101,
    6 * 1024 ** 3 + 202,
    7 * 1024 ** 3 + 303,
)


def input_files(directory: Path, *, with_disk: bool) -> tuple[Path, Path, Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
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


def environment_for(temp: Path, *, write_preflight: bool = True) -> dict[str, str]:
    runner_temp = temp / "runner-temp"
    runner_temp.mkdir()
    env = os.environ.copy()
    runner_os = {"Linux": "Linux", "Darwin": "macOS"}.get(platform.system(), "Linux")
    env.update({
        "RUNNER_TEMP": str(runner_temp),
        "RUNNER_OS": runner_os,
        "XNULINUX_RUNNER_LABEL": "report-test",
        "GITHUB_STEP_SUMMARY": str(temp / "summary.md"),
    })
    if write_preflight:
        (runner_temp / "puredarwin-tcg-boot-report.json").write_text(
            json.dumps({"host": {"memory_available_before_artifact_preparation_bytes": 9_876_543_210}}),
            encoding="utf-8",
        )
    return env


def fake_qemu(path: Path) -> Path:
    path.write_text(
        "#!/bin/sh\n"
        "if [ \"$1\" = --version ]; then echo 'QEMU emulator version report-test'; exit 0; fi\n"
        "exit 0\n",
        encoding="utf-8",
    )
    path.chmod(0o755)
    return path


def run_runner_with_memory_samples(arguments: list[str], env: dict[str, str]) -> int:
    spec = importlib.util.spec_from_file_location(
        "puredarwin_tcg_e2e_runner_under_test", RUNNER
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load PureDarwin TCG runner")
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    host_facts = [
        {
            "system": "Linux",
            "release": "report-test",
            "version": "report-test",
            "architecture": "x86_64",
            "processor": "report-test",
            "logical_cpus": 2,
            "memory_total_bytes": 32 * 1024 ** 3,
            "memory_available_bytes": available,
        }
        for available in PYTHON_MEMORY_SAMPLES
    ]
    with (
        mock.patch.dict(os.environ, env, clear=False),
        mock.patch.object(sys, "argv", [str(RUNNER), *arguments]),
        mock.patch.object(runner, "host_facts", side_effect=host_facts),
        redirect_stdout(io.StringIO()),
        redirect_stderr(io.StringIO()),
    ):
        return runner.main()


class TcgE2eReportTests(unittest.TestCase):
    def test_posix_preflight_reports_decimal_byte_counts_and_phase(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            env = environment_for(temp, write_preflight=False)
            subprocess.run(["bash", str(INIT_REPORT)], env=env, check=True, capture_output=True, text=True)

            report_path = Path(env["RUNNER_TEMP"]) / "puredarwin-tcg-boot-report.json"
            report = json.loads(report_path.read_text(encoding="utf-8"))
            host = report["host"]

            self.assertIsInstance(host["memory_total_bytes"], int)
            self.assertIsInstance(host["memory_available_before_artifact_preparation_bytes"], int)
            self.assertNotIn("memory_available_at_report_bytes", host)
            self.assertIn("before artifact preparation", report["preparation_steps"]["status"])

            # Pass the actual POSIX producer output through the runner before it overwrites the report.
            kernel, initramfs, disk, _ = input_files(temp / "artifacts", with_disk=True)
            qemu = fake_qemu(temp / "qemu-system-aarch64")
            result = run_runner_with_memory_samples(
                [str(kernel), str(initramfs), str(disk), str(temp / "serial.log"),
                 "--timeout-seconds", "5", "--qemu", str(qemu)],
                env,
            )
            final_report = json.loads(report_path.read_text(encoding="utf-8"))
            summary = Path(env["GITHUB_STEP_SUMMARY"]).read_text(encoding="utf-8")
            final_host = final_report["host"]
            self.assertEqual(result, 1)  # The fake QEMU emits no guest markers.
            self.assertEqual(
                final_host["memory_available_before_artifact_preparation_bytes"],
                host["memory_available_before_artifact_preparation_bytes"],
            )
            python_samples = (
                final_host["memory_available_at_runner_start_bytes"],
                final_host["memory_available_before_qemu_bytes"],
                final_host["memory_available_after_qemu_attempt_bytes"],
            )
            for sample in python_samples:
                self.assertIs(type(sample), int)
            self.assertEqual(python_samples, PYTHON_MEMORY_SAMPLES)
            self.assertEqual(len(set(python_samples)), 3)
            self.assertIn("at Python runner start", summary)

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
            self.assertIsInstance(report["host"]["memory_available_at_runner_start_bytes"], int)
            expected_capacity = (
                report["host"]["memory_total_bytes"] >= 20 * 1024 ** 3
                and report["host"]["memory_available_at_runner_start_bytes"] >= 4 * 1024 ** 3
            )
            self.assertEqual(report["ram_capacity"]["capacity_screen_passed"], expected_capacity)
            self.assertIsNone(report["host"]["memory_available_before_qemu_bytes"])
            self.assertIsNone(report["host"]["memory_available_after_qemu_attempt_bytes"])

    def test_report_labels_memory_before_and_after_qemu_attempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            kernel, initramfs, disk, _ = input_files(temp, with_disk=True)
            env = environment_for(temp)
            qemu = fake_qemu(temp / "qemu-system-aarch64")
            serial_log = temp / "serial.log"
            result = subprocess.run(
                [sys.executable, str(RUNNER), str(kernel), str(initramfs), str(disk), str(serial_log),
                 "--timeout-seconds", "5", "--qemu", str(qemu)],
                env=env, capture_output=True, text=True, check=False,
            )

            report = json.loads(
                (Path(env["RUNNER_TEMP"]) / "puredarwin-tcg-boot-report.json").read_text(encoding="utf-8")
            )
            summary = Path(env["GITHUB_STEP_SUMMARY"]).read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 1)  # The fake QEMU emits no guest markers.
            self.assertEqual(report["qemu_version"], "QEMU emulator version report-test")
            self.assertEqual(report["host"]["memory_available_before_artifact_preparation_bytes"], 9_876_543_210)
            self.assertIsInstance(report["host"]["memory_available_at_runner_start_bytes"], int)
            self.assertIsInstance(report["host"]["memory_available_before_qemu_bytes"], int)
            self.assertIsInstance(report["host"]["memory_available_after_qemu_attempt_bytes"], int)
            self.assertIn("before artifact preparation", summary)
            self.assertIn("at Python runner start", summary)
            self.assertIn("before QEMU", summary)
            self.assertIn("after QEMU attempt", summary)


if __name__ == "__main__":
    unittest.main()
