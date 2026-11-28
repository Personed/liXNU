#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import hashlib
from pathlib import Path
import runpy
import stat
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]
AUDIT = runpy.run_path(str(ROOT / "scripts/x86_64/audit-tahoe-ota.py"))


class OtaInventoryTests(unittest.TestCase):
    def test_verified_directory_distinguishes_collection_from_delta(self):
        payload_count = AUDIT["MAX_REPORT_ENTRIES"] + 3
        paths = {
            "AssetData/boot/BootKernelExtensions.kc": b"collection stand-in",
            "AssetData/delta/SystemKernelExtensions.kc.bxdiff": b"delta stand-in",
            "AssetData/payloadv2/patches/System/Library/KernelCollections/BootKernelExtensions.kc": b"BXDIFF50",
            "AssetData/payloadv2/ecc_data/System/Library/KernelCollections/BootKernelExtensions.kc": b"BXECC010",
            "AssetData/payloadv2/data_payload": b"opaque",
            "AssetData/post.bom": b"BootKernelExtensions.kc",
            "AssetData/SystemKernelExtensions.kc.bak": b"unrelated",
            "AssetData/unrelated/BootKernelExtensions.kc/": b"",
            **{f"AssetData/payloadv2/payload.{i:03}": b"opaque" for i in range(payload_count)},
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fixture.zip"
            with zipfile.ZipFile(path, "x") as archive:
                for name, data in paths.items():
                    archive.writestr(name, data)
                symlink = zipfile.ZipInfo("AssetData/link/BootKernelExtensions.kc")
                symlink.create_system = 3
                symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(symlink, "target")
            data = path.read_bytes()
            report = {}
            AUDIT["inspect"](path, report, len(data), hashlib.sha256(data).hexdigest())

        self.assertTrue(report["source_digest_verified"])
        self.assertEqual(report["zip_entries"], len(paths) + 1)
        self.assertEqual(report["matches"]["collection_file"], {
            "count": 1,
            "entries": [{"path": "AssetData/boot/BootKernelExtensions.kc", "bytes": 19}],
        })
        self.assertEqual(report["matches"]["collection_delta"]["count"], 2)
        self.assertEqual(report["matches"]["collection_ecc"]["count"], 1)
        self.assertEqual(report["matches"]["bom"]["count"], 1)
        payload = report["matches"]["payload_archive"]
        self.assertEqual(payload["count"], payload_count + 1)
        self.assertEqual(len(payload["entries"]), AUDIT["MAX_REPORT_ENTRIES"])
        self.assertIn("AssetData/payloadv2/data_payload", {entry["path"] for entry in payload["entries"]})

    def test_identity_must_match_before_reading_zip_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fixture.zip"
            path.write_bytes(b"invalid ZIP")
            for size, expected_message in ((12, "byte count"), (11, "SHA-256")):
                with self.subTest(expected_message=expected_message):
                    report = {}
                    with self.assertRaisesRegex(AUDIT["AuditError"], expected_message):
                        AUDIT["inspect"](path, report, size, "0" * 64)
                    self.assertNotIn("matches", report)
                    self.assertNotIn("source_digest_verified", report)


if __name__ == "__main__":
    unittest.main()
