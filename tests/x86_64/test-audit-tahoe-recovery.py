#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check Recovery inspection outcomes without mounting an Apple image."""

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/x86_64/audit-tahoe-recovery.py"
SPEC = importlib.util.spec_from_file_location("audit_tahoe_recovery", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class RecoveryDownloadTests(unittest.TestCase):
    def test_timeout_retry_uses_remaining_budget_and_a_fresh_directory(self):
        report = {"apple_chunklist_verified": False}
        steps = []

        def run_attempt(command, *, timeout):
            directory = Path(command[command.index("-o") + 1])
            steps.append((directory, timeout))
            (directory / f"{audit.RECOVERY_BASENAME}.chunklist").write_bytes(b"signed")
            image = directory / f"{audit.RECOVERY_BASENAME}.dmg"
            if len(steps) == 1:
                image.write_bytes(b"partial")
                return subprocess.CompletedProcess(command, 1, "https://example.test/signed?token=private", "timed out")
            self.assertFalse(steps[0][0].exists())
            self.assertFalse(image.exists())
            image.write_bytes(b"verified")
            return subprocess.CompletedProcess(command, 0, "Verifying image with chunklist...\n", "")

        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            with (
                patch.object(audit, "run", side_effect=run_attempt),
                patch.object(audit.time, "monotonic", side_effect=[0, 0, 120]),
            ):
                image = audit.download_recovery(report, workspace / "macrecovery.py", workspace)
            self.assertEqual(image.read_bytes(), b"verified")
            self.assertEqual([timeout for _, timeout in steps], [3000, 2880])
            self.assertTrue(report["apple_chunklist_verified"])
            self.assertEqual(report["recovery_download_attempts"], [
                {"phase": "image_download", "image_bytes": 7, "chunklist_bytes": 6,
                 "exit_code": 1, "failure_class": "network_timeout"},
                {"phase": "chunklist_verification", "image_bytes": 8, "chunklist_bytes": 6,
                 "exit_code": 0, "failure_class": None},
            ])
            self.assertNotIn("private", json.dumps(report))

    def test_failure_never_claims_verification(self):
        cases = (
            (124, "", "timeout", "network_timeout", "session_or_catalog"),
            (1, "HTTP Error 403", "", "apple_endpoint_forbidden", "session_or_catalog"),
            (1, "Verifying image with chunklist...\nImage verification failed", "",
             "apple_chunklist_verification_failed", "chunklist_verification"),
        )
        for code, stdout, stderr, failure, phase in cases:
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                workspace = Path(temporary)
                report = {"apple_chunklist_verified": False}
                with patch.object(audit, "run", return_value=subprocess.CompletedProcess([], code, stdout, stderr)) as run:
                    self.assertIsNone(audit.download_recovery(report, workspace / "macrecovery.py", workspace))
                run.assert_called_once()
                self.assertFalse(report["apple_chunklist_verified"])
                self.assertEqual(report["failure_class"], failure)
                self.assertEqual(report["macrecovery_exit_code"], code)
                self.assertEqual(report["recovery_download_attempts"][0]["phase"], phase)

    def test_launcher_sets_socket_timeout_and_preserves_downloader_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            downloader = workspace / "macrecovery.py"
            downloader.write_text(
                "import socket, sys\n"
                f"assert socket.getdefaulttimeout() == {audit.RECOVERY_SOCKET_TIMEOUT_SECONDS}\n"
                "assert sys.argv[0].endswith('macrecovery.py')\n"
                "assert sys.argv[-3:] == ['-os', 'latest', 'download']\n"
                "sys.exit(42)\n",
                encoding="utf-8",
            )
            report = {"apple_chunklist_verified": False}
            self.assertIsNone(audit.download_recovery(report, downloader, workspace))
            self.assertEqual(report["macrecovery_exit_code"], 42)
            self.assertFalse(report["apple_chunklist_verified"])


class RecoveryInspectionTests(unittest.TestCase):
    def test_intel_release_version_candidates(self):
        image_bytes = b"\0".join((
            b"root:xnu-12377.161.14~5/RELEASE_X86_64",
            b"root:xnu-12377.161.15.701.48~2/RELEASE_X86_64",
            b"root:xnu-12377.161.15.701.48/RELEASE_X86_64",
            b"root:xnu-12377.161.15.701.48~2/DEVELOPMENT_X86_64",
            b"root:xnu-12377.161.15.701.48~2/RELEASE_ARM64",
            b"root:xnu-12377.161../RELEASE_X86_64",
            b"root:xnu-12377.161/RELEASE_X86_64",
        ))
        self.assertEqual(audit.XNU_RELEASE_PATTERN.findall(image_bytes), [
            b"12377.161.14~5",
            b"12377.161.15.701.48~2",
            b"12377.161.15.701.48",
        ])

    def test_inspection_outcomes(self):
        cases = (
            ("accepted", True, False, "recovery_downloaded_and_inspected"),
            ("parser_rejected", True, False, "recovery_downloaded_and_inspected"),
            ("not_fileset", True, False, "recovery_downloaded_and_inspected"),
            ("helper_failed", True, False, "recovery_downloaded_but_inspection_failed"),
            ("accepted", False, False, "recovery_mount_cleanup_failed"),
            ("accepted", True, True, "recovery_mount_cleanup_failed"),
        )
        for helper_status, detached, residue, expected in cases:
            with self.subTest(helper_status=helper_status, detached=detached, residue=residue):
                with tempfile.TemporaryDirectory() as workspace:
                    temporary = Path(workspace) / "download"
                    temporary.mkdir()
                    image = temporary / "verified-recovery.dmg"
                    helper = temporary / "xnu-fileset-metadata"
                    mounted = {"boot_kernel_collection_fileset": {"status": helper_status}}
                    report = {}

                    def detach_image(_hdiutil, _image, mount):
                        if residue:
                            (mount / "leftover").touch()
                        return detached

                    with (
                        patch.object(audit.shutil, "which", return_value="/usr/bin/hdiutil"),
                        patch.object(audit, "run", return_value=subprocess.CompletedProcess([], 0)),
                        patch.object(Path, "is_mount", return_value=True),
                        patch.object(audit, "inspect_mounted_image", return_value=mounted) as inspect,
                        patch.object(audit, "detach_recovery", side_effect=detach_image) as detach,
                    ):
                        audit.inspect_recovery(report, image, helper, temporary)

                    mount = inspect.call_args.args[0]
                    inspect.assert_called_once_with(mount, image, helper)
                    detach.assert_called_once_with("/usr/bin/hdiutil", image, mount)
                    self.assertEqual(report["audit_status"], expected)
                    self.assertEqual(report["recovery_image"], mounted)
                    self.assertEqual(report["recovery_detached"], detached)
                    self.assertEqual(report["temporary_mount_directory_removed"], not residue)
                    self.assertEqual(mount.exists(), residue)


if __name__ == "__main__":
    unittest.main()
