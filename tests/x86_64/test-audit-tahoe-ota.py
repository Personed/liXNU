#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import hashlib
from pathlib import Path
import plistlib
import runpy
import stat
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]
AUDIT = runpy.run_path(str(ROOT / "scripts/x86_64/audit-tahoe-ota.py"))


class OtaInventoryTests(unittest.TestCase):
    def test_installed_delta_requires_the_matching_base_digest(self):
        base = b"prerequisite boot collection"
        one = b"\x01\x00\x00\x00\x00\x00\x00\x00"
        length = b"\x80\x00\x00\x00\x00\x00\x00\x00"
        header = b"BXDIFF50" + one + length + one + one + b"r" * 20 + one + hashlib.sha1(base).digest()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plist = root / "System/Library/CoreServices/SystemVersion.plist"
            collection = root / AUDIT["SYSTEM_BOOTKC"]
            plist.parent.mkdir(parents=True)
            collection.parent.mkdir(parents=True)
            path = root / "fixture.zip"
            with zipfile.ZipFile(path, "x") as archive:
                archive.writestr(AUDIT["SYSTEM_BOOTKC_DELTA"], header + b"abc")
            plist.write_bytes(plistlib.dumps({"ProductBuildVersion": "25G83"}))
            with zipfile.ZipFile(path) as archive:
                result = AUDIT["probe_delta_base"](archive, root)
                self.assertEqual(result["base_status"], "build_mismatch")
                for malformed in (
                    b"<plist><key>ProductBuildVersion</key></plist>",
                    b"<plist><dict><key>date</key><date>invalid</date></dict></plist>",
                    plistlib.dumps({}),
                ):
                    plist.write_bytes(malformed)
                    self.assertEqual(AUDIT["probe_delta_base"](archive, root)["base_status"], "build_unavailable")
                plist.write_bytes(plistlib.dumps({"ProductBuildVersion": "25G76"}))
                self.assertEqual(AUDIT["probe_delta_base"](archive, root)["base_status"], "collection_unavailable")
                collection.write_bytes(base + b"x")
                self.assertEqual(AUDIT["probe_delta_base"](archive, root)["base_status"], "hash_mismatch")
                collection.write_bytes(base)
                result = AUDIT["probe_delta_base"](archive, root)
                self.assertEqual(result["base_status"], "hash_match")
                self.assertEqual(result["declared_result_sha1"], (b"r" * 20).hex())
                self.assertEqual(result["declared_result_bytes"], 128)
                self.assertEqual(collection.read_bytes(), base)
            compare = AUDIT["compare_declared_delta_result"]
            direct = {"sha1": (b"r" * 20).hex(), "bytes": 128}
            self.assertEqual(compare(direct, result), "hash_and_size_match")
            self.assertEqual(compare({**direct, "bytes": 127}, result), "mismatch")
            self.assertEqual(compare({**direct, "sha1": "0" * 40}, result), "mismatch")
            self.assertEqual(compare({}, result), "unavailable")

    def test_inspect_only_the_direct_boot_collection(self):
        fixtures = runpy.run_path(str(ROOT / "tests/x86_64/test-fileset-metadata.py"))
        image = bytes(fixtures["make_segmented_fileset"]()) + b"xnu-12377.161.15.701.48~2/RELEASE_X86_64\0"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            helper = directory / "xnu-fileset-metadata"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(ROOT / "tools/dev/x86_64/xnu-fileset-metadata.c"),
                str(ROOT / "xnu-ko/macho_parse_x86_64.c"), "-o", str(helper),
            ], check=True)
            path = directory / "fixture.zip"
            with zipfile.ZipFile(path, "x") as archive:
                archive.writestr(
                    "AssetData/payloadv2/patches/System/Library/KernelCollections/BootKernelExtensions.kc", image,
                )
            with zipfile.ZipFile(path) as archive:
                with self.assertRaisesRegex(AUDIT["AuditError"], "exactly one"):
                    AUDIT["inspect_boot_collection"](archive, helper, {}, len(image))
            with zipfile.ZipFile(path, "a") as archive:
                archive.writestr(AUDIT["BOOT_MEMBER"], image)
            report = {}
            with zipfile.ZipFile(path) as archive:
                AUDIT["inspect_boot_collection"](archive, helper, report, len(image))
            self.assertEqual(report["path"], AUDIT["BOOT_MEMBER"])
            self.assertEqual(report["fileset"]["status"], "accepted")
            self.assertEqual(report["sha256"], hashlib.sha256(image).hexdigest())
            self.assertEqual(report["sha1"], hashlib.sha1(image).hexdigest())
            self.assertEqual(report["xnu_release_candidates"], ["12377.161.15.701.48~2"])

    def test_verified_directory_distinguishes_collection_from_delta(self):
        payload_count = AUDIT["MAX_REPORT_ENTRIES"] + 3
        paths = {
            "AssetData/boot/BootKernelExtensions.kc": b"collection stand-in",
            "AssetData/delta/SystemKernelExtensions.kc.bxdiff": b"delta stand-in",
            "AssetData/payloadv2/patches/System/Library/KernelCollections/BootKernelExtensions.kc": b"BXDIFF50",
            "AssetData/payloadv2/ecc_data/System/Library/KernelCollections/BootKernelExtensions.kc": b"BXECC010",
            "AssetData/payloadv2/data_payload": b"opaque",
            "AssetData/payloadv2/firmlinks_payload": b"YAA1",
            "AssetData/post.bom": b"BootKernelExtensions.kc",
            "AssetData/payloadv2/patches/Library/Receipts/example.bom": b"BXDIFF50",
            "AssetData/payloadv2/ecc_data/Library/Receipts/example.bom": b"BXECC010",
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
        self.assertEqual(payload["count"], payload_count + 2)
        self.assertEqual(len(payload["entries"]), AUDIT["MAX_REPORT_ENTRIES"])
        payload_paths = {entry["path"] for entry in payload["entries"]}
        self.assertIn("AssetData/payloadv2/data_payload", payload_paths)
        self.assertIn("AssetData/payloadv2/firmlinks_payload", payload_paths)

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
