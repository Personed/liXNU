#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the unified xnulinux CLI and install.sh."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CLI = ROOT / "scripts" / "xnulinux"
INSTALLER = ROOT / "scripts" / "install.sh"


class TestCliAndInstaller(unittest.TestCase):
    def simulated_user_env(self, tmp, *, without_python=False, sudo_log=None):
        shim = pathlib.Path(tmp) / "shim"
        shim.mkdir(exist_ok=True)
        fake_id = shim / "id"
        fake_id.write_text("#!/bin/sh\nprintf '1000\\n'\n", encoding="utf-8")
        fake_id.chmod(0o755)
        if sudo_log is not None:
            fake_sudo = shim / "sudo"
            fake_sudo.write_text(
                '#!/bin/sh\nprintf "called\\n" >> "$SUDO_LOG"\nexit 97\n', encoding="utf-8"
            )
            fake_sudo.chmod(0o755)
        if without_python:
            fake_python = shim / "python3"
            fake_python.write_text("#!/bin/sh\nexit 127\n", encoding="utf-8")
            fake_python.chmod(0o755)
        env = {**os.environ, "PATH": f"{shim}{os.pathsep}{os.environ.get('PATH', '')}"}
        if sudo_log is not None:
            env["SUDO_LOG"] = str(sudo_log)
        return env

    def make_install_fixture(self, tmp):
        checkout = pathlib.Path(tmp) / "repo space 'single' \"double\" $dollar"
        scripts = checkout / "scripts"
        scripts.mkdir(parents=True)
        fixture_installer = scripts / "install.sh"
        shutil.copy2(INSTALLER, fixture_installer)
        fixture_cli = scripts / "xnulinux"
        fixture_cli.write_text(
            '#!/bin/sh\nprintf \'root=%s\\narg=%s\\n\' "$XNULINUX_HOME" "${1:-}"\n',
            encoding="utf-8",
        )
        fixture_cli.chmod(0o755)
        return checkout.resolve(), fixture_installer

    def test_help_doctor_status(self):
        for cmd in ("help", "doctor", "status"):
            res = subprocess.run([str(CLI), cmd], capture_output=True, text=True, check=True)
            self.assertTrue(res.stdout.strip())

    def test_screenshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            fb = os.path.join(tmp, "fb.raw")
            out = os.path.join(tmp, "out.png")
            with open(fb, "wb") as f:
                f.write(b"\x00\x80\xff\xff" * 4)
            env = {**os.environ, "FB_DUMP": fb}
            subprocess.run([str(CLI), "screenshot", out, "2", "2"], env=env, check=True, capture_output=True)
            with open(out, "rb") as f:
                self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")

    def test_missing_optional_scripts_report_cleanly(self):
        with tempfile.TemporaryDirectory() as tmp:
            # Isolated minimal tree without optional PR #3 / PR #9 scripts
            scripts_dir = pathlib.Path(tmp) / "scripts"
            scripts_dir.mkdir()
            shutil.copy2(CLI, scripts_dir / "xnulinux")
            (scripts_dir / "xnu-console.sh").write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
            env = {**os.environ, "XNULINUX_HOME": tmp}

            res_vnc = subprocess.run([str(scripts_dir / "xnulinux"), "vnc"], env=env, capture_output=True, text=True)
            self.assertNotEqual(res_vnc.returncode, 0)
            self.assertIn("xnu-vnc.py", res_vnc.stderr)

            res_vm = subprocess.run([str(scripts_dir / "xnulinux"), "vm"], env=env, capture_output=True, text=True)
            self.assertNotEqual(res_vm.returncode, 0)
            self.assertIn("qemu-vm.py", res_vm.stderr)

            res_con = subprocess.run(
                [str(scripts_dir / "xnulinux"), "console", "--local"], env=env, capture_output=True, text=True
            )
            self.assertNotEqual(res_con.returncode, 0)
            self.assertIn("--local", res_con.stderr)

    def test_non_arm64_guards_boot_and_setup_mem(self):
        if os.uname().machine == "aarch64":
            self.skipTest("host is aarch64")
        res_boot = subprocess.run([str(CLI), "boot"], capture_output=True, text=True)
        self.assertNotEqual(res_boot.returncode, 0)
        self.assertIn("ARM64", res_boot.stderr)

        res_mem = subprocess.run([str(CLI), "setup-mem", "--dry-run"], capture_output=True, text=True)
        self.assertNotEqual(res_mem.returncode, 0)
        self.assertIn("ARM64", res_mem.stderr)

    def test_install_relative_prefix_and_uninstall(self):
        with tempfile.TemporaryDirectory() as tmp:
            # Install using a relative --prefix, then run from another working directory
            env = self.simulated_user_env(tmp)
            subprocess.run(
                [str(INSTALLER), "--prefix", "relprefix"], cwd=tmp, env=env, check=True, capture_output=True, text=True
            )
            installed = pathlib.Path(tmp) / "relprefix" / "bin" / "xnulinux"
            self.assertTrue(installed.is_file())
            res = subprocess.run([str(installed), "doctor"], cwd="/", check=True, capture_output=True, text=True)
            self.assertIn("=== xnulinux PC check ===", res.stdout)
            subprocess.run(
                [str(INSTALLER), "--prefix", "relprefix", "--uninstall"],
                cwd=tmp,
                env=env,
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertFalse(installed.exists())

    def test_install_and_uninstall_absolute_prefix_without_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = self.simulated_user_env(tmp, without_python=True)
            prefix = pathlib.Path(tmp) / "absolute-prefix"
            installed = prefix / "bin" / "xnulinux"
            subprocess.run(
                [str(INSTALLER), "--prefix", str(prefix)], env=env, check=True, capture_output=True, text=True
            )
            self.assertTrue(installed.is_file())
            subprocess.run(
                [str(INSTALLER), "--prefix", str(prefix), "--uninstall"],
                env=env,
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertFalse(installed.exists())

    def test_user_install_wrapper_preserves_shell_special_checkout_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            sudo_log = pathlib.Path(tmp) / "sudo.log"
            env = self.simulated_user_env(tmp, sudo_log=sudo_log)
            checkout, fixture_installer = self.make_install_fixture(tmp)
            prefix = pathlib.Path(tmp) / "prefix space 'single' \"double\" $dollar"
            installed = prefix / "bin" / "xnulinux"

            result = subprocess.run(
                [str(fixture_installer), "--prefix", str(prefix)],
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(sudo_log.exists(), "user install must not call sudo")
            launched = subprocess.run(
                [str(installed), "probe"], cwd="/", env=env, capture_output=True, text=True
            )
            self.assertEqual(launched.returncode, 0, launched.stderr)
            self.assertEqual(launched.stdout, f"root={checkout}\narg=probe\n")

    def test_system_install_wrapper_preserves_shell_special_checkout_path(self):
        if not hasattr(os, "geteuid") or os.geteuid() != 0:
            self.skipTest("requires root to exercise the system-wide install branch")
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            public_root = pathlib.Path(tmp)
            public_root.chmod(0o755)
            checkout, fixture_installer = self.make_install_fixture(tmp)
            checkout.chmod(0o755)
            (checkout / "scripts").chmod(0o755)
            fixture_installer.chmod(0o755)

            shim = public_root / "shim"
            shim.mkdir()
            sudo_log = public_root / "sudo.log"
            fake_sudo = shim / "sudo"
            fake_sudo.write_text(
                '#!/bin/sh\nprintf "called\\n" >> "$SUDO_LOG"\nexit 97\n', encoding="utf-8"
            )
            fake_sudo.chmod(0o755)
            env = {
                **os.environ,
                "PATH": f"{shim}{os.pathsep}{os.environ.get('PATH', '')}",
                "SUDO_LOG": str(sudo_log),
            }
            prefix = public_root / "system prefix 'single' \"double\" $dollar"
            installed = prefix / "bin" / "xnulinux"

            result = subprocess.run(
                [str(fixture_installer), "--prefix", str(prefix)],
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(sudo_log.exists(), "root install must not call sudo")
            self.assertTrue(installed.is_file())
            launched = subprocess.run(
                [str(installed), "probe"], cwd="/", env=env, capture_output=True, text=True
            )
            self.assertEqual(launched.returncode, 0, launched.stderr)
            self.assertEqual(launched.stdout, f"root={checkout}\narg=probe\n")

    def test_system_install_rejects_private_checkout_parent(self):
        if not hasattr(os, "geteuid") or os.geteuid() != 0:
            self.skipTest("requires root to exercise system-wide install checks")
        with tempfile.TemporaryDirectory() as tmp:
            private = pathlib.Path(tmp) / "private"
            private.mkdir(mode=0o700)
            checkout = private / "xnulinux"
            scripts = checkout / "scripts"
            scripts.mkdir(parents=True)
            fixture_installer = scripts / "install.sh"
            shutil.copy2(INSTALLER, fixture_installer)
            fixture_cli = scripts / "xnulinux"
            fixture_cli.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fixture_cli.chmod(0o755)

            prefix = pathlib.Path(tmp) / "system-prefix"
            result = subprocess.run(
                [str(fixture_installer), "--prefix", str(prefix)], capture_output=True, text=True
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing system-wide install", result.stderr)
            self.assertFalse((prefix / "bin" / "xnulinux").exists())

    def test_system_install_succeeds_from_public_root_owned_checkout(self):
        if not hasattr(os, "geteuid") or os.geteuid() != 0:
            self.skipTest("requires root to exercise system-wide install checks")
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            public_root = pathlib.Path(tmp)
            public_root.chmod(0o755)
            checkout = public_root / "public-checkout"
            scripts = checkout / "scripts"
            scripts.mkdir(parents=True, mode=0o755)
            checkout.chmod(0o755)
            scripts.chmod(0o755)
            fixture_installer = scripts / "install.sh"
            shutil.copy2(INSTALLER, fixture_installer)
            fixture_cli = scripts / "xnulinux"
            fixture_cli.write_text("#!/bin/sh\nprintf 'fixture-doctor-ok\\n'\n", encoding="utf-8")
            fixture_cli.chmod(0o755)

            prefix = public_root / "system-prefix"
            installed = prefix / "bin" / "xnulinux"
            result = subprocess.run(
                [str(fixture_installer), "--prefix", str(prefix)], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(installed.is_file())
            self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
            self.assertIn(str(fixture_cli.resolve()), installed.read_text(encoding="utf-8"))
            launched = subprocess.run(
                [str(installed), "doctor"], cwd=public_root, capture_output=True, text=True
            )
            self.assertEqual(launched.returncode, 0, launched.stderr)
            self.assertEqual(launched.stdout, "fixture-doctor-ok\n")


if __name__ == "__main__":
    unittest.main()
