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
    def test_inspection_outcomes(self):
        cases = (
            ("accepted", True, "recovery_downloaded_and_inspected"),
            ("parser_rejected", True, "recovery_downloaded_and_inspected"),
            ("not_fileset", True, "recovery_downloaded_and_inspected"),
            ("helper_failed", True, "recovery_downloaded_but_inspection_failed"),
            ("accepted", False, "recovery_mount_cleanup_failed"),
        )
        for helper_status, detached, expected in cases:
            with self.subTest(helper_status=helper_status, detached=detached):
                with tempfile.TemporaryDirectory() as workspace:
                    temporary = Path(workspace) / "download"
                    temporary.mkdir()
                    image = temporary / "verified-recovery.dmg"
                    helper = temporary / "xnu-fileset-metadata"
                    mounted = {"boot_kernel_collection_fileset": {"status": helper_status}}
                    report = {}
                    with (
                        patch.object(audit.shutil, "which", return_value="/usr/bin/hdiutil"),
                        patch.object(audit, "run", return_value=subprocess.CompletedProcess([], 0)),
                        patch.object(Path, "is_mount", return_value=True),
                        patch.object(audit, "inspect_mounted_image", return_value=mounted) as inspect,
                        patch.object(audit, "detach_recovery", return_value=detached) as detach,
                    ):
                        audit.inspect_recovery(report, image, helper, temporary)

                    mount = inspect.call_args.args[0]
                    inspect.assert_called_once_with(mount, image, helper)
                    detach.assert_called_once_with("/usr/bin/hdiutil", image, mount)
                    self.assertEqual(report["audit_status"], expected)
                    self.assertEqual(report["recovery_image"], mounted)
                    self.assertEqual(report["recovery_detached"], detached)
                    self.assertTrue(report["temporary_mount_directory_removed"])
                    self.assertFalse(mount.exists())


if __name__ == "__main__":
    unittest.main()
