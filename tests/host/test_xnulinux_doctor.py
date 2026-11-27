# SPDX-License-Identifier: GPL-2.0-only
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
