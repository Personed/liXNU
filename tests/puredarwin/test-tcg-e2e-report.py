#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for PureDarwin TCG artifact verification and reports."""

import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "run_tcg_e2e", ROOT / "tests" / "puredarwin" / "run-tcg-e2e.py"
)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


def manifest_fixtures():
    disk_sha = "a" * 64
    kernel_sha = "b" * 64
    initramfs_sha = "c" * 64
    build_manifest = {"puredarwin_disk_sha256": disk_sha}
    guest_manifest = {
        "arm64_guest_kernel_release": "7.0.0-44-generic",
        "arm64_guest_page_size": "4096 (CONFIG_ARM64_4K_PAGES=y)",
        "arm64_guest_pac": "runtime checked",
        "arm64_guest_exception_level": "EL1",
        "arm64_guest_image_sha256": kernel_sha,
        "arm64_guest_initramfs_sha256": initramfs_sha,
    }
    inputs = {
        "disk_sha256": disk_sha,
        "kernel_sha256": kernel_sha,
        "initramfs_sha256": initramfs_sha,
    }
    return inputs, build_manifest, guest_manifest


class ArtifactManifestTests(unittest.TestCase):
    def test_accepts_matching_disk_kernel_and_initramfs_hashes(self):
        inputs, build, guest = manifest_fixtures()

        errors = RUNNER.manifest_input_errors(
            inputs,
            [],
            "puredarwin_disk_sha256=" + build["puredarwin_disk_sha256"],
            build,
            "guest manifest",
            guest,
        )

        self.assertEqual(errors, [])

    def test_missing_disk_records_an_error_instead_of_crashing(self):
        inputs, build, guest = manifest_fixtures()
        inputs["disk_sha256"] = None

        errors = RUNNER.manifest_input_errors(
            inputs,
            [Path("pd.img")],
            "puredarwin_disk_sha256=" + build["puredarwin_disk_sha256"],
            build,
            "guest manifest",
            guest,
        )

        self.assertTrue(any("missing input file: pd.img" in error for error in errors))
        self.assertTrue(any("cannot verify PureDarwin disk SHA-256" in error for error in errors))

    def test_kernel_and_initramfs_must_match_guest_manifest(self):
        inputs, build, guest = manifest_fixtures()
        inputs["kernel_sha256"] = "d" * 64
        inputs["initramfs_sha256"] = "e" * 64

        errors = RUNNER.manifest_input_errors(
            inputs,
            [],
            "puredarwin_disk_sha256=" + build["puredarwin_disk_sha256"],
            build,
            "guest manifest",
            guest,
        )

        self.assertIn("ARM64 guest kernel SHA-256 does not match its manifest", errors)
        self.assertIn("ARM64 guest initramfs SHA-256 does not match its manifest", errors)

    def test_requires_valid_guest_kernel_and_initramfs_hashes(self):
        inputs, build, guest = manifest_fixtures()
        guest["arm64_guest_image_sha256"] = "not-a-sha256"
        del guest["arm64_guest_initramfs_sha256"]

        errors = RUNNER.manifest_input_errors(
            inputs,
            [],
            "puredarwin_disk_sha256=" + build["puredarwin_disk_sha256"],
            build,
            "guest manifest",
            guest,
        )

        self.assertIn("manifest ARM64 guest kernel is not a valid SHA-256 digest", errors)
        self.assertIn("manifest is missing ARM64 guest initramfs SHA-256", errors)


class BootRunnerFailureTests(unittest.TestCase):
    def test_missing_disk_writes_a_host_report_before_returning(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            kernel = root / "Image"
            initramfs = root / "initramfs.cpio.gz"
            disk = root / "missing-pd.img"
            serial_log = root / "serial.log"
            kernel.write_bytes(b"kernel image")
            initramfs.write_bytes(b"initramfs")
            kernel_sha = RUNNER.sha256_file(kernel)
            initramfs_sha = RUNNER.sha256_file(initramfs)
            disk_sha = "a" * 64
            (root / "build-manifest.txt").write_text(
                f"puredarwin_disk_sha256={disk_sha}\n", encoding="utf-8"
            )
            (root / "guest-manifest.txt").write_text(
                "\n".join(
                    [
                        "arm64_guest_kernel_release=7.0.0-44-generic",
                        "arm64_guest_page_size=4096",
                        "arm64_guest_pac=runtime checked",
                        "arm64_guest_exception_level=EL1",
                        f"arm64_guest_image_sha256={kernel_sha}",
                        f"arm64_guest_initramfs_sha256={initramfs_sha}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            report_path = root / "puredarwin-tcg-boot-report.json"
            fake_host = {
                "system": "Linux",
                "release": "test",
                "version": "test",
                "architecture": "x86_64",
                "processor": "test CPU",
                "logical_cpus": 4,
                "memory_total_bytes": 24 * 1024**3,
                "memory_available_bytes": 8 * 1024**3,
            }
            argv = [
                str(ROOT / "tests" / "puredarwin" / "run-tcg-e2e.py"),
                str(kernel),
                str(initramfs),
                str(disk),
                str(serial_log),
                "--qemu",
                "missing-qemu-for-test",
            ]
            env = {
                "RUNNER_TEMP": temp_dir,
                "XNULINUX_RUNNER_LABEL": "missing-disk-test",
                "GITHUB_STEP_SUMMARY": str(root / "summary.md"),
            }

            with mock.patch.dict(os.environ, env, clear=False), \
                 mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(RUNNER, "host_facts", return_value=fake_host), \
                 mock.patch.object(RUNNER, "available_memory_bytes", return_value=8 * 1024**3), \
                 mock.patch.object(RUNNER.shutil, "which", return_value=None), \
                 contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                result = RUNNER.main()

            report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(result, 1)
        self.assertTrue(any("missing input file:" in error for error in report["result"]["errors"]))
        self.assertTrue(
            any("cannot verify PureDarwin disk SHA-256" in error for error in report["result"]["errors"])
        )
        self.assertEqual(
            report["ram_capacity"]["measurement_phase"],
            "python_runner_after_preparation_before_qemu",
        )
        self.assertEqual(
            report["host"]["memory_available_at_report_phase"],
            "validation_failed_before_qemu",
        )


@unittest.skipUnless(platform.system() == "Linux", "Linux host preflight test")
class HostPreflightTests(unittest.TestCase):
    def test_linux_preflight_writes_integer_ram_and_measurement_phase(self):
        script = ROOT / "tests" / "puredarwin" / "init-host-report.sh"
        with tempfile.TemporaryDirectory() as temp_dir:
            env = os.environ.copy()
            env["RUNNER_TEMP"] = temp_dir
            env["RUNNER_OS"] = "Linux"
            env["XNULINUX_RUNNER_LABEL"] = "report-test"
            subprocess.run(["bash", str(script)], check=True, env=env, capture_output=True, text=True)
            report_path = Path(temp_dir) / "puredarwin-tcg-boot-report.json"
            report = json.loads(report_path.read_text(encoding="utf-8"))

        host = report["host"]
        self.assertIsInstance(host["memory_total_bytes"], int)
        self.assertIsInstance(host["memory_available_bytes"], int)
        self.assertEqual(
            report["ram_capacity"]["measurement_phase"],
            "host_preflight_before_artifact_downloads",
        )
        self.assertEqual(
            host["memory_available_at_report_phase"],
            "host_preflight_before_artifact_downloads",
        )

    def test_macos_preflight_writes_decimal_memory_with_large_page_counts(self):
        script = ROOT / "tests" / "puredarwin" / "init-host-report.sh"
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            (fake_bin / "sw_vers").write_text(
                "#!/bin/sh\nprintf '15.0\\n'\n", encoding="utf-8"
            )
            (fake_bin / "sysctl").write_text(
                "#!/bin/sh\n"
                "case \"$2\" in\n"
                "  hw.memsize) printf '17179869184\\n' ;;\n"
                "  hw.pagesize) printf '4096\\n' ;;\n"
                "  machdep.cpu.brand_string) printf 'Intel CPU test\\n' ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            (fake_bin / "vm_stat").write_text(
                "#!/bin/sh\n"
                "cat <<'EOF'\n"
                "Mach Virtual Memory Statistics: (page size of 4096 bytes)\n"
                "Pages free: 250000.\n"
                "Pages inactive: 3000000.\n"
                "Pages speculative: 100000.\n"
                "Pages purgeable: 50000.\n"
                "EOF\n",
                encoding="utf-8",
            )
            for executable in (fake_bin / "sw_vers", fake_bin / "sysctl", fake_bin / "vm_stat"):
                executable.chmod(0o755)

            env = os.environ.copy()
            env["RUNNER_TEMP"] = temp_dir
            env["RUNNER_OS"] = "macOS"
            env["XNULINUX_RUNNER_LABEL"] = "macos-report-test"
            env["PATH"] = str(fake_bin) + os.pathsep + env["PATH"]
            subprocess.run(["bash", str(script)], check=True, env=env, capture_output=True, text=True)
            report = json.loads(
                (root / "puredarwin-tcg-boot-report.json").read_text(encoding="utf-8")
            )

        host = report["host"]
        self.assertEqual(host["system"], "Darwin")
        self.assertEqual(host["memory_total_bytes"], 17179869184)
        self.assertEqual(host["memory_available_bytes"], (250000 + 3000000 + 100000 + 50000) * 4096)
        self.assertIsInstance(host["memory_available_bytes"], int)


if __name__ == "__main__":
    unittest.main()
