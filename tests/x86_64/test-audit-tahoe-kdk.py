#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
import pathlib
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts/x86_64/audit-tahoe-kdk.py"
IDENTITY = (
    b"Darwin Kernel Version 25.6.0: build time; "
    b"root:xnu-12377.161.15.701.48~2/DEVELOPMENT_X86_64\x00"
)


class AuditTest(unittest.TestCase):
    def run_audit(self, cpu=0x01000007, file_type=2, build="25G342", identity=IDENTITY):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            bundle = root / "KDK_26.7.3_25G342.kdk"
            library = bundle / "System/Library"
            plist = library / "CoreServices/SystemVersion.plist"
            plist.parent.mkdir(parents=True)
            plist.write_bytes(plistlib.dumps({
                "ProductVersion": "26.7.3",
                "ProductBuildVersion": build,
            }))
            kernel = library / "Kernels/kernel.development"
            kernel.parent.mkdir()
            data = struct.pack("<8I", 0xFEEDFACF, cpu, 3, file_type, 0, 0, 0, 0) + identity
            kernel.write_bytes(data)
            report = root / "report.json"
            result = subprocess.run(
                [sys.executable, SCRIPT, bundle, report], capture_output=True, text=True
            )
            if result.returncode:
                self.assertFalse(report.exists())
                return result.stderr
            metadata = json.loads(report.read_text())
            self.assertEqual(metadata["kernel_sha256"], hashlib.sha256(data).hexdigest())
            return metadata

    def test_target_identity(self):
        self.assertEqual(self.run_audit()["xnu_version"], "12377.161.15.701.48~2")

    def test_wrong_target(self):
        self.assertIn("SystemVersion.plist", self.run_audit(build="25G83"))
        self.assertIn("thin x86_64 Mach-O", self.run_audit(cpu=0x0100000C))
        self.assertIn("not MH_EXECUTE", self.run_audit(file_type=12))
        self.assertIn("expected Intel XNU identity", self.run_audit(
            identity=IDENTITY + IDENTITY.replace(b"~2", b"~3")
        ))


if __name__ == "__main__":
    unittest.main()
