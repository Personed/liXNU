# SPDX-License-Identifier: GPL-2.0-only
import gzip
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("xnulinux_doctor", ROOT / "scripts" / "xnulinux-doctor.py")
doctor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(doctor)
GIB = 1024 ** 3


def facts(system="Linux", machine="x86_64", qemu=None):
    return {
        "system": system,
        "release": "test",
        "machine": machine,
        "processor": "test",
        "logical_cpus": 4,
        "memory_total_bytes": 16 * GIB,
        "memory_available_bytes": 12 * GIB,
        "free_disk_bytes": 40 * GIB,
        "qemu": qemu if qemu is not None else {
            "executable": "qemu-system-aarch64",
            "path": "/usr/bin/qemu-system-aarch64",
            "version": "8.2.2",
            "version_parts": (8, 2, 2),
            "error": None,
        },
        "linux_cpu_flags": list(doctor.X86_64H_FEATURES),
    }


class DoctorTest(unittest.TestCase):
    def test_meminfo_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "meminfo"
            path.write_text("MemTotal:       16367500 kB\nMemAvailable:   14965924 kB\n", encoding="utf-8")
            values = doctor.read_meminfo(str(path))
        self.assertEqual(values["MemTotal"], 16367500 * 1024)
        self.assertEqual(values["MemAvailable"], 14965924 * 1024)

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux")
    def test_linux_cpu_facts_use_one_cpu_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cpuinfo"
            path.write_text(
                "processor : 0\nvendor_id : AuthenticAMD\nmodel name : Example AMD CPU\n"
                "flags : fma avx2\n\nprocessor : 1\nflags : sse4_2\n",
                encoding="utf-8",
            )
            cpu_info = doctor.linux_cpu_info(path)
        with patch.object(doctor, "linux_cpu_info", return_value=cpu_info):
            # Avoid running QEMU while testing independent Linux host facts.
            with patch.object(doctor, "qemu_facts", return_value={}):
                host = doctor.collect_facts()

        self.assertEqual(host["linux_cpu_vendor"], "AuthenticAMD")
        self.assertEqual(host["linux_cpu_model"], "Example AMD CPU")
        self.assertEqual(host["linux_cpu_flags"], ["avx2", "fma"])
        self.assertEqual(host["linux_page_size_bytes"], os.sysconf("SC_PAGE_SIZE"))

    def test_kernel_config_prefers_proc_gzip_and_hashes_decompressed_bytes(self):
        config = b"CONFIG_64BIT=y\nCONFIG_EXAMPLE=m\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            proc_config = root / "proc-config.gz"
            proc_config.write_bytes(gzip.compress(config, mtime=0))
            boot_dir = root / "boot"
            boot_dir.mkdir()
            (boot_dir / "config-6.8.0-test").write_bytes(b"fallback config\n")

            facts = doctor.linux_kernel_config_facts(
                "6.8.0-test",
                proc_config_path=str(proc_config),
                boot_config_dir=str(boot_dir),
            )

        self.assertEqual(facts["status"], "available")
        self.assertEqual(facts["source"], str(proc_config))
        self.assertEqual(facts["sha256"], hashlib.sha256(config).hexdigest())

    def test_kernel_config_falls_back_to_matching_boot_config(self):
        config = b"CONFIG_64BIT=y\nCONFIG_EXAMPLE=y\n"
        with tempfile.TemporaryDirectory() as tmp:
            boot_dir = Path(tmp)
            proc_config = Path(tmp) / "proc-config.gz"
            proc_config.write_bytes(b"not a gzip stream")
            boot_config = boot_dir / "config-6.8.0-test"
            boot_config.write_bytes(config)

            facts = doctor.linux_kernel_config_facts(
                "6.8.0-test",
                proc_config_path=str(proc_config),
                boot_config_dir=str(boot_dir),
            )

        self.assertEqual(facts["status"], "available")
        self.assertEqual(facts["source"], str(boot_config))
        self.assertEqual(facts["sha256"], hashlib.sha256(config).hexdigest())

    def test_kernel_config_unavailable_has_null_source_and_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            facts = doctor.linux_kernel_config_facts(
                "6.8.0-test",
                proc_config_path=str(Path(tmp) / "missing-config.gz"),
                boot_config_dir=str(Path(tmp) / "missing-boot"),
            )

        self.assertEqual(
            facts,
            {"status": "unavailable", "source": None, "sha256": None},
        )

    def test_json_snapshot_reports_kernel_config_status_and_digest(self):
        expected_config = {
            "status": "available",
            "source": "/proc/config.gz",
            "sha256": "abc123",
        }
        with (
            patch.object(doctor.platform, "system", return_value="Linux"),
            patch.object(doctor.platform, "machine", return_value="x86_64"),
            patch.object(doctor.platform, "release", return_value="6.8.0-test"),
            patch.object(doctor, "memory_facts", return_value=(16 * GIB, 12 * GIB)),
            patch.object(doctor, "qemu_facts", return_value={}),
            patch.object(doctor, "linux_cpu_info", return_value={}),
            patch.object(
                doctor,
                "linux_kernel_config_facts",
                return_value=expected_config,
            ) as config_facts,
        ):
            host = doctor.collect_facts()

        self.assertEqual(host["linux_kernel_config_status"], "available")
        self.assertEqual(host["linux_kernel_config_source"], "/proc/config.gz")
        self.assertEqual(host["linux_kernel_config_sha256"], "abc123")
        config_facts.assert_called_once_with("6.8.0-test")

    def test_non_linux_json_snapshot_marks_kernel_config_not_applicable(self):
        with (
            patch.object(doctor.platform, "system", return_value="Darwin"),
            patch.object(doctor.platform, "machine", return_value="x86_64"),
            patch.object(doctor.platform, "release", return_value="27.0"),
            patch.object(doctor, "memory_facts", return_value=(16 * GIB, 12 * GIB)),
            patch.object(doctor, "qemu_facts", return_value={}),
            patch.object(doctor, "linux_kernel_config_facts") as config_facts,
        ):
            host = doctor.collect_facts()

        self.assertEqual(host["linux_kernel_config_status"], "not_applicable")
        self.assertIsNone(host["linux_kernel_config_source"])
        self.assertIsNone(host["linux_kernel_config_sha256"])
        config_facts.assert_not_called()

    def test_linux_x86_with_reference_qemu_passes(self):
        checks, overall = doctor.evaluate(facts())
        self.assertEqual(overall, "pass")
        self.assertIn(("host-profile", "pass"), [(item["name"], item["status"]) for item in checks])
        self.assertIn(("qemu", "pass"), [(item["name"], item["status"]) for item in checks])

    def test_missing_qemu_fails(self):
        data = facts()
        data["qemu"]["path"] = None
        checks, overall = doctor.evaluate(data)
        self.assertEqual(overall, "fail")
        qemu = next(item for item in checks if item["name"] == "qemu")
        self.assertEqual(qemu["status"], "fail")

    def test_arm64_direct_host_does_not_require_qemu(self):
        data = facts(system="Linux", machine="aarch64")
        data["qemu"]["path"] = None
        checks, overall = doctor.evaluate(data)
        self.assertEqual(overall, "pass")
        qemu = next(item for item in checks if item["name"] == "qemu")
        self.assertEqual(qemu["status"], "info")

    def test_windows_x86_is_candidate_warning(self):
        checks, overall = doctor.evaluate(facts(system="Windows"))
        self.assertEqual(overall, "warning")
        host = next(item for item in checks if item["name"] == "host-profile")
        self.assertEqual(host["status"], "warning")

    def test_old_qemu_is_warning_not_hard_failure(self):
        data = facts()
        data["qemu"].update(version="6.2.0", version_parts=(6, 2, 0))
        checks, overall = doctor.evaluate(data)
        self.assertEqual(overall, "warning")
        qemu = next(item for item in checks if item["name"] == "qemu")
        self.assertEqual(qemu["status"], "warning")

    def test_native_feature_floor_is_informational(self):
        data = facts()
        data["linux_cpu_flags"] = []
        checks, overall = doctor.evaluate(data)
        feature_check = next(item for item in checks if item["name"] == "native-x86-feature-floor")
        self.assertEqual(feature_check["status"], "info")
        self.assertIn("avx2", feature_check["details"]["missing_features"])
        self.assertEqual(overall, "pass")

    def test_json_report_shape(self):
        facts_data = facts()
        checks, overall = doctor.evaluate(facts_data)
        report = {"schema": 1, "facts": facts_data, "checks": checks, "overall": overall}
        encoded = json.dumps(report)
        self.assertEqual(json.loads(encoded)["schema"], 1)


if __name__ == "__main__":
    unittest.main()
