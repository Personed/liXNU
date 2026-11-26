#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check QEMU launcher arguments on Linux, macOS, and Windows."""

import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/qemu-vm.py"
spec = importlib.util.spec_from_file_location("qemu_vm", SCRIPT)
qemu_vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qemu_vm)


class QemuLauncherTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.disk = root / "linux disk,1.qcow2"
        self.firmware_code = root / "firmware code.fd"
        self.firmware_vars = root / "firmware vars.fd"
        self.installer = root / "ubuntu installer.iso"
        for path in (self.disk, self.firmware_code, self.firmware_vars, self.installer):
            path.touch()
        self.args = qemu_vm.argparse.Namespace(
            qemu="qemu-system-aarch64",
            cpus=4,
            memory=16384,
            disk=self.disk,
            disk_format="qcow2",
            firmware_code=self.firmware_code,
            firmware_vars=self.firmware_vars,
            installer=self.installer,
            ssh_port=2222,
            graphical=True,
            display_backend=None,
        )

    def test_graphics_backend_matches_host(self):
        for host, backend in (("Linux", "gtk"), ("Darwin", "cocoa"), ("Windows", "sdl")):
            with self.subTest(host=host), \
                 mock.patch.object(qemu_vm.platform, "system", return_value=host), \
                 mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout=f"{backend}\n")) as probe:
                command = qemu_vm.build_command(self.args)
            self.assertEqual(command[command.index("-display") + 1], backend)
            self.assertEqual(probe.call_args.args[0], ["qemu-system-aarch64", "-display", "help"])

    def test_uses_sdl_when_preferred_backend_is_missing(self):
        with mock.patch.object(qemu_vm.platform, "system", return_value="Linux"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="sdl\n")):
            command = qemu_vm.build_command(self.args)
        self.assertEqual(command[command.index("-display") + 1], "sdl")

    def test_uses_explicit_display_backend_when_requested(self):
        self.args.display_backend = "sdl"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Linux"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\nsdl\n")):
            command = qemu_vm.build_command(self.args)
        self.assertEqual(command[command.index("-display") + 1], "sdl")

    def test_explicit_display_backend_must_be_available(self):
        self.args.display_backend = "sdl"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Linux"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\n")):
            with self.assertRaisesRegex(
                ValueError,
                "requested display backend 'sdl'.*available: gtk.*omit --display-backend",
            ):
                qemu_vm.build_command(self.args)

    def test_explicit_display_backend_without_any_graphics_backend_is_actionable(self):
        self.args.display_backend = "sdl"
        self.args.window_title = "Linux ARM64"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Linux"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="vnc\n")):
            with self.assertRaisesRegex(
                ValueError,
                "install a QEMU build with GTK or SDL support, or run headless after "
                "removing --graphical, --display-backend, and --window-title",
            ) as result:
                qemu_vm.build_command(self.args)
        self.assertNotIn("select an available backend", str(result.exception))

    def test_windows_without_sdl_gets_actionable_graphics_error(self):
        self.args.display_backend = "sdl"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Windows"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\n")):
            with self.assertRaisesRegex(
                ValueError,
                "Windows graphical mode requires SDL for its default; install QEMU "
                "with SDL support; "
                r"try an available backend with --display-backend \(gtk\).*run headless "
                r"after removing --graphical, --display-backend, and --window-title",
            ):
                qemu_vm.build_command(self.args)

    def test_windows_default_graphics_without_sdl_gets_actionable_error(self):
        self.args.display_backend = None
        with mock.patch.object(qemu_vm.platform, "system", return_value="Windows"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\n")):
            with self.assertRaisesRegex(
                ValueError,
                "Windows graphical mode requires SDL; install QEMU with SDL support "
                r"or select an available backend with --display-backend \(gtk\).*run headless "
                r"after removing --graphical, --display-backend, and --window-title",
            ):
                qemu_vm.build_command(self.args)

    def test_windows_missing_backend_without_sdl_does_not_suggest_host_default(self):
        self.args.display_backend = "cocoa"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Windows"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="vnc\n")):
            with self.assertRaisesRegex(
                ValueError,
                "Windows graphical mode requires SDL for its default; install QEMU with SDL "
                r"support; after installing SDL, retry with --display-backend sdl or "
                r"remove --display-backend to use the Windows default; run headless "
                r"after removing --graphical, --display-backend, "
                "and --window-title",
            ) as result:
                qemu_vm.build_command(self.args)
        self.assertNotIn("omit --display-backend", str(result.exception))

    def test_windows_missing_requested_backend_explains_sdl_retry(self):
        self.args.display_backend = "cocoa"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Windows"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\n")):
            with self.assertRaisesRegex(
                ValueError,
                r"try an available backend with --display-backend \(gtk\).*after "
                r"installing SDL, retry with --display-backend sdl or remove "
                r"--display-backend to use the Windows default",
            ) as result:
                qemu_vm.build_command(self.args)
        self.assertIn("requested display backend 'cocoa'", str(result.exception))

    def test_windows_can_select_an_available_non_sdl_backend(self):
        self.args.display_backend = "gtk"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Windows"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\n")):
            command = qemu_vm.build_command(self.args)
        self.assertEqual(command[command.index("-display") + 1], "gtk")

    def test_display_backend_requires_graphical_mode(self):
        argv = [
            "qemu-vm.py", str(self.disk),
            "--firmware-code", str(self.firmware_code),
            "--firmware-vars", str(self.firmware_vars),
            "--display-backend", "sdl",
        ]
        stderr = io.StringIO()
        with (
            mock.patch.object(qemu_vm.sys, "argv", argv),
            mock.patch.object(qemu_vm.sys, "stderr", stderr),
            self.assertRaises(SystemExit) as result,
        ):
            qemu_vm.main()
        self.assertEqual(result.exception.code, 2)
        self.assertIn("--display-backend requires --graphical", stderr.getvalue())

    def test_escapes_qemu_name_option_separators(self):
        self.args.window_title = "Linux, ARM64=Test"
        with mock.patch.object(qemu_vm.platform, "system", return_value="Linux"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="gtk\n")):
            command = qemu_vm.build_command(self.args)
        self.assertEqual(command[command.index("-name") + 1], "guest=Linux,, ARM64=Test")

    def test_reports_when_no_supported_graphics_backend_is_available(self):
        with mock.patch.object(qemu_vm.platform, "system", return_value="Linux"), \
             mock.patch.object(qemu_vm.subprocess, "run", return_value=mock.Mock(stdout="vnc\n")):
            with self.assertRaisesRegex(ValueError, "no graphical backend suitable"):
                qemu_vm.build_command(self.args)

    def test_headless_mode_and_escaped_paths(self):
        self.args.graphical = False
        command = qemu_vm.build_command(self.args)
        self.assertEqual(command[command.index("-display") + 1], "none")
        self.assertIn("file=" + str(self.disk).replace(",", ",,") + ",if=none,id=linux,format=qcow2",
                      command)
        self.assertIn("hostfwd=tcp:127.0.0.1:2222-:22", command[command.index("-netdev") + 1])

    def test_windows_launch_waits_for_qemu_exit(self):
        argv = [
            "qemu-vm.py", str(self.disk),
            "--firmware-code", str(self.firmware_code),
            "--firmware-vars", str(self.firmware_vars),
        ]
        completed = qemu_vm.subprocess.CompletedProcess([], 0)
        with (
            mock.patch.object(qemu_vm.sys, "argv", argv),
            mock.patch.object(qemu_vm.platform, "system", return_value="Windows"),
            mock.patch.object(qemu_vm.subprocess, "run", return_value=completed) as run,
        ):
            self.assertEqual(qemu_vm.main(), 0)
        self.assertEqual(run.call_count, 1)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("-display") + 1], "none")


if __name__ == "__main__":
    unittest.main()
