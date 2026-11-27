#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check Recovery inspection outcomes without mounting an Apple image."""

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/x86_64/audit-tahoe-recovery.py"
SPEC = importlib.util.spec_from_file_location("audit_tahoe_recovery", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


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
