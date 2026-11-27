#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that the free collection builder exports a canonical DVM path."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/free-kc/build.sh"


class FreeKernelCollectionPathTest(unittest.TestCase):
    def test_relative_and_space_paths_reach_final_kernel_collection_copy(self):
        source = SCRIPT.read_text(encoding="utf-8")
        pd_revision = re.search(r"\bPD_REV=([0-9a-f]+)", source).group(1)
        mbedtls_revision = re.search(
            r"^MBEDTLS_REV=(.+)$",
            (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
            re.MULTILINE,
        ).group(1)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            launch = root / "launch with spaces"
            vm = root / "vm with spaces"
            cdpath = root / "CDPATH alternatives"
            fake_bin = root / "bin"
            poc_target = root / "repo with spaces"
            poc_link = root / "poc with spaces"
            work = launch / "relative work"
            poc_checkout = launch / "poc"
            launch.mkdir()
            (cdpath / "vm with spaces").mkdir(parents=True)
            (cdpath / "relative work").mkdir(parents=True)
            (cdpath / "poc").mkdir(parents=True)
            fake_bin.mkdir()
            for relative in (
                "build/netinc",
                "kexts/free/corecrypto",
                "kexts/free/compat",
                "kexts/XnuCPU",
                "kexts/XnuDisk",
                "kexts/XnuDrm",
                "scripts/free-kc",
                "scripts/kclink",
            ):
                (poc_target / relative).mkdir(parents=True, exist_ok=True)
            (poc_target / "kexts/free/corecrypto/mbedtls.rev").write_text(
                (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            for relative in (
                "scripts/free-kc/kcc.py",
                "scripts/free-kc/genspec.py",
                "scripts/free-kc/mk-template.py",
                "scripts/free-kc/layout.json",
                "scripts/kclink/kclink.py",
            ):
                (poc_target / relative).touch()
            poc_link.symlink_to(poc_target, target_is_directory=True)
            poc_checkout.symlink_to(poc_link, target_is_directory=True)
            kernel_framework = (
                vm
                / "out/x12377/build/xnu/k27h-dst/System/Library/Frameworks/Kernel.framework/Versions/A"
            )
            (kernel_framework / "PrivateHeaders").mkdir(parents=True)
            (vm / "out/k27/xnu").mkdir(parents=True)
            (launch / "vm with spaces").symlink_to(vm, target_is_directory=True)

            extensions = work / "src/pd/src/Kernel/Extensions"
            for relative in (
                "amfi",
                "pthread",
                "IOStorageFamily",
                "HFSEncodings",
                "hfs/core",
                "hfs/shim",
                "hfs/hfs_encodings",
            ):
                (extensions / relative).mkdir(parents=True)
            (extensions / "hfs/core/kext-config.h").write_text("/* fixture */\n", encoding="utf-8")

            git = fake_bin / "git"
            git.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = \"-C\" ] && [ \"$3\" = \"rev-parse\" ]; then\n"
                "  case \"$2\" in\n"
                "    */pd) printf '%s\\n' \"$FAKE_PD_REV\"; exit 0 ;;\n"
                "    */mbedtls) printf '%s\\n' \"$FAKE_MBEDTLS_REV\"; exit 0 ;;\n"
                "  esac\n"
                "fi\n"
                "exit 88\n",
                encoding="utf-8",
            )
            git.chmod(0o755)

            python = fake_bin / "python3"
            python.write_text(
                "#!/bin/sh\n"
                "printf 'CALL\\0' >> \"$PYTHON_CAPTURE\"\n"
                "printf '%s\\0' \"$@\" >> \"$PYTHON_CAPTURE\"\n"
                "case \"$1\" in\n"
                "  */kclink.py)\n"
                "    pwd -P > \"$KCLINK_CWD_CAPTURE\"\n"
                "    mkdir -p -- \"$(dirname -- \"$4\")\"\n"
                "    printf 'fake kernel collection\\n' > \"$4\"\n"
                "    printf 'wrote %s\\n' \"$4\"\n"
                "    ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            python.chmod(0o755)

            strings = fake_bin / "strings"
            strings.write_text(
                "#!/bin/sh\n"
                "printf '%s\\0' \"$@\" > \"$STRINGS_CAPTURE\"\n"
                "printf 'Darwin Kernel Version 27.0.0: fixture\\n'\n",
                encoding="utf-8",
            )
            strings.chmod(0o755)

            sudo = fake_bin / "sudo"
            sudo.write_text("#!/bin/sh\nexec \"$@\"\n", encoding="utf-8")
            sudo.chmod(0o755)

            python_capture = root / "python-call-log.bin"
            strings_capture = root / "strings-args.bin"
            output = root / "kc free.bin"
            env = os.environ.copy()
            env.update(
                {
                    "CDPATH": str(cdpath),
                    "DVM": "vm with spaces",
                    "KCLINK_CWD_CAPTURE": str(root / "kclink-cwd.txt"),
                    "PYTHON_CAPTURE": str(python_capture),
                    "STRINGS_CAPTURE": str(strings_capture),
                    "FAKE_PD_REV": pd_revision,
                    "FAKE_MBEDTLS_REV": mbedtls_revision,
                    "FREE_KC_WORK": "relative work",
                    "OUT": str(output),
                    "POC": "poc",
                    "PATH": str(fake_bin) + os.pathsep + env["PATH"],
                }
            )
            result = subprocess.run(
                ["bash", str(SCRIPT)],
                cwd=launch,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout)
            calls = [
                [value.decode() for value in record.split(b"\0") if value]
                for record in python_capture.read_bytes().split(b"CALL\0")[1:]
            ]
            kcc_calls = [arguments for arguments in calls if arguments[0].endswith("/kcc.py")]
            self.assertGreater(len(kcc_calls), 1)

            corecrypto = kcc_calls[0]
            self.assertEqual(corecrypto[0], str(poc_checkout / "scripts/free-kc/kcc.py"))
            self.assertNotIn("--macos", corecrypto)
            self.assertEqual(corecrypto[1], str(work.resolve() / "obj/corecrypto"))
            self.assertEqual(corecrypto[2:4], ["-I", str(poc_checkout / "kexts/free/corecrypto")])
            self.assertEqual(corecrypto[4:6], ["-I", str(work.resolve() / "src/mbedtls/include")])
            source_list = re.search(
                r"^MBEDTLS_SOURCES=(.+)$",
                (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
                re.MULTILINE,
            ).group(1)
            for relative in source_list.split():
                self.assertIn(str(work.resolve() / "src/mbedtls/library" / relative), corecrypto)
            self.assertTrue(all("--macos" in arguments for arguments in kcc_calls[1:]))

            strings_arguments = [
                value.decode() for value in strings_capture.read_bytes().split(b"\0") if value
            ]
            self.assertEqual(strings_arguments, [str(vm.resolve() / "out/k27/kernel-k27.macho")])
            kclink = next(arguments for arguments in calls if arguments[0].endswith("/kclink.py"))
            self.assertEqual((root / "kclink-cwd.txt").read_text().strip(), str(vm.resolve()))
            self.assertEqual(kclink[1], str(work.resolve() / "kc-template.bin"))
            self.assertEqual(kclink[3], str(work.resolve() / "kc-free.bin"))
            self.assertEqual(kclink[4], "--native")
            self.assertIn(str(poc_checkout / "kexts/XnuCPU/XnuCPU.json"), kclink)
            self.assertEqual(output.read_text(encoding="utf-8"), "fake kernel collection\n")

    def test_default_poc_ignores_competing_cdpath_checkout(self):
        source = SCRIPT.read_text(encoding="utf-8")
        pd_revision = re.search(r"\bPD_REV=([0-9a-f]+)", source).group(1)
        mbedtls_revision = re.search(
            r"^MBEDTLS_REV=(.+)$",
            (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
            re.MULTILINE,
        ).group(1)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            launch = root / "dev"
            stable = root / "stable"
            vm = root / "vm"
            fake_bin = root / "bin"
            for checkout in (launch, stable):
                (checkout / "scripts/free-kc").mkdir(parents=True)
                (checkout / "kexts/free/corecrypto").mkdir(parents=True)
                (checkout / "kexts/free/corecrypto/mbedtls.rev").write_text(
                    (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
                    encoding="utf-8",
                )
                (checkout / "scripts/free-kc/kcc.py").touch()
            (launch / "scripts/free-kc/build.sh").write_text(source, encoding="utf-8")
            (vm / "out/x12377/build/xnu/k27h-dst/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders").mkdir(parents=True)
            fake_bin.mkdir()

            git = fake_bin / "git"
            git.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = \"-C\" ] && [ \"$3\" = \"rev-parse\" ]; then\n"
                "  case \"$2\" in\n"
                "    */pd) printf '%s\\n' \"$FAKE_PD_REV\"; exit 0 ;;\n"
                "    */mbedtls) printf '%s\\n' \"$FAKE_MBEDTLS_REV\"; exit 0 ;;\n"
                "  esac\n"
                "fi\n"
                "exit 88\n",
                encoding="utf-8",
            )
            git.chmod(0o755)

            python = fake_bin / "python3"
            python.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n%s\\n' \"$DVM\" \"$1\" > \"$DVM_CAPTURE\"\n"
                "exit 73\n",
                encoding="utf-8",
            )
            python.chmod(0o755)

            capture = root / "paths.txt"
            env = os.environ.copy()
            env.update(
                {
                    "CDPATH": str(stable),
                    "DVM": str(vm),
                    "DVM_CAPTURE": str(capture),
                    "FAKE_PD_REV": pd_revision,
                    "FAKE_MBEDTLS_REV": mbedtls_revision,
                    "FREE_KC_WORK": str(root / "free-kc-work"),
                    "OUT": str(root / "kc-free.bin"),
                    "PATH": str(fake_bin) + os.pathsep + env["PATH"],
                }
            )
            env.pop("POC", None)
            result = subprocess.run(
                ["bash", "scripts/free-kc/build.sh"],
                cwd=launch,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 73, result.stdout)
            self.assertEqual(
                capture.read_text(encoding="utf-8").splitlines(),
                [str(vm.resolve()), str(launch / "scripts/free-kc/kcc.py")],
            )

    def test_xnu_rebuild_uses_its_checkout_and_cdpath_dvm(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dev = root / "dev"
            stable = root / "stable"
            stable_dev = stable / "dev"
            vm = root / "vm-real"
            alternate_vm = root / "vm-alternate"
            fake_bin = root / "bin"
            fake_bin.mkdir()
            for checkout, marker in ((dev, "dev.c"), (stable_dev, "stable.c")):
                (checkout / "scripts").mkdir(parents=True)
                (checkout / "xnu-patches/include").mkdir(parents=True)
                (checkout / "xnu-patches/corecrypto/include/corecrypto").mkdir(parents=True)
                (checkout / "xnu-patches/corecrypto/kernel").mkdir(parents=True)
                (checkout / "xnu-patches/include/test.h").write_text("/* fixture */\n", encoding="utf-8")
                (checkout / "xnu-patches/0001-source.patch").write_text(
                    f"+++ b/{marker}\n",
                    encoding="utf-8",
                )
            (dev / "scripts/vm-xnu-rebuild.sh").write_text(
                (ROOT / "scripts/vm-xnu-rebuild.sh").read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            (dev / "vm-link").symlink_to(vm, target_is_directory=True)
            (stable / "vm-link").symlink_to(alternate_vm, target_is_directory=True)
            (alternate_vm / "out/k27/xnu").mkdir(parents=True)

            builder = alternate_vm / "k27/build-kernel27.sh"
            builder.parent.mkdir(parents=True)
            builder.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n%s\\n' \"$PWD\" \"$DVM\" > \"$DVM_CAPTURE\"\n"
                "cat out/k27/xnu/.linux64-patched >> \"$DVM_CAPTURE\"\n"
                "exit 73\n",
                encoding="utf-8",
            )
            builder.chmod(0o755)

            git = fake_bin / "git"
            git.write_text(
                "#!/bin/bash\n"
                "case \"$3\" in\n"
                "  checkout) exit 1 ;;\n"
                "  apply) exit 0 ;;\n"
                "  *) exit 88 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            git.chmod(0o755)

            capture = root / "vm-paths.txt"
            env = os.environ.copy()
            env.update(
                {
                    "CDPATH": "stable",
                    "DVM": "vm-link",
                    "DVM_CAPTURE": str(capture),
                    "PATH": str(fake_bin) + os.pathsep + env["PATH"],
                }
            )
            env.pop("POC", None)
            result = subprocess.run(
                ["bash", "dev/scripts/vm-xnu-rebuild.sh"],
                cwd=root,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 73, result.stdout)
            self.assertEqual(
                capture.read_text(encoding="utf-8").splitlines(),
                [str(alternate_vm.resolve()), str(alternate_vm.resolve()), "dev.c"],
            )

    def test_xnu_rebuild_builds_required_puredarwin_kexts_without_kernelcache(self):
        def run_rebuild(fail_kext=None, fail_layout_check=False):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                dvm = root / "Droid VM workspace"
                poc = root / "xnulinux checkout"
                fake_bin = root / "bin"
                shutil.copytree(ROOT / "xnu-patches", poc / "xnu-patches")
                for name in ("XnuCPU", "XnuDisk", "XnuDrm"):
                    shutil.copytree(ROOT / "kexts" / name, poc / "kexts" / name)
                (dvm / "out/k27/xnu").mkdir(parents=True)
                (dvm / "k27").mkdir()
                (dvm / "toolchains/sdks").mkdir(parents=True)
                fake_bin.mkdir()

                kernel_builder = dvm / "k27/build-kernel27.sh"
                kernel_builder.write_text(
                    "#!/bin/sh\n"
                    "mkdir -p out/k27\n"
                    "printf 'build\\n' >> \"$EVENT_CAPTURE\"\n"
                    "printf 'fixture kernel\\n' > out/k27/kernel-k27.macho\n",
                    encoding="utf-8",
                )
                kernel_builder.chmod(0o755)

                git = fake_bin / "git"
                git.write_text(
                    "#!/bin/sh\n"
                    "case \"$3\" in\n"
                    "  checkout) exit 1 ;;\n"
                    "  apply) exit 0 ;;\n"
                    "  *) exit 88 ;;\n"
                    "esac\n",
                    encoding="utf-8",
                )
                git.chmod(0o755)

                python = fake_bin / "python3"
                python.write_text(
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"$POC/scripts/free-kc/fit_kernel_segments.py\" ]; then\n"
                    "  if [ \"${2:-}\" = --check ]; then\n"
                    "    printf 'check %s %s %s\\n' \"$2\" \"$3\" \"$4\" >> \"$EVENT_CAPTURE\"\n"
                    "    [ \"${FAIL_LAYOUT_CHECK:-0}\" != 1 ] || exit 42\n"
                    "    exit 0\n"
                    "  fi\n"
                    "  printf 'fit %s %s\\n' \"$2\" \"$3\" >> \"$EVENT_CAPTURE\"\n"
                    "  echo 'relink kernel: fixture layout changed'\n"
                    "  exit 0\n"
                    "fi\n"
                    "[ \"$1\" = scripts/build-native-kext.py ] || exit 88\n"
                    "printf 'CALL\\0' >> \"$KEXT_CAPTURE\"\n"
                    "printf '%s\\0' \"$@\" >> \"$KEXT_CAPTURE\"\n"
                    "[ \"${FAIL_KEXT:-}\" != \"$2\" ] || exit 42\n",
                    encoding="utf-8",
                )
                python.chmod(0o755)

                sudo = fake_bin / "sudo"
                sudo.write_text(
                    "#!/bin/sh\n"
                    "case \"$1\" in install|cp) exit 0 ;; esac\n"
                    "exec \"$@\"\n",
                    encoding="utf-8",
                )
                sudo.chmod(0o755)

                capture = root / "kext-builds.txt"
                capture.touch()
                event_capture = root / "rebuild-events.txt"
                event_capture.touch()
                env = os.environ.copy()
                env.update(
                    {
                        "DVM": str(dvm),
                        "EVENT_CAPTURE": str(event_capture),
                        "KEXT_CAPTURE": str(capture),
                        "KEXT_OBJ": "objects with spaces",
                        "PATH": str(fake_bin) + os.pathsep + env["PATH"],
                        "POC": str(poc),
                    }
                )
                env.pop("BUILD_NATIVE_KEXTS", None)
                if fail_layout_check:
                    env["FAIL_LAYOUT_CHECK"] = "1"
                else:
                    env.pop("FAIL_LAYOUT_CHECK", None)
                if fail_kext is None:
                    env.pop("FAIL_KEXT", None)
                else:
                    env["FAIL_KEXT"] = fail_kext

                result = subprocess.run(
                    ["bash", str(ROOT / "scripts/vm-xnu-rebuild.sh")],
                    cwd=ROOT,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    check=False,
                )
                calls = [
                    [value.decode() for value in call.split(b"\0") if value]
                    for call in capture.read_bytes().split(b"CALL\0")[1:]
                ]
                events = event_capture.read_text(encoding="utf-8").splitlines()
                return result, calls, poc, events

        def expected_kext_args(poc, name):
            return [
                "scripts/build-native-kext.py",
                name,
                str(poc / "kexts" / name / f"{name}.cpp"),
                "-I",
                str(poc / "build/netinc"),
                "--obj",
                "objects with spaces",
            ]

        result, calls, poc, events = run_rebuild()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(
            events,
            [
                "build",
                "fit out/k27/kernel-k27.macho k27/segaddrs.sh",
                "build",
                "check --check out/k27/kernel-k27.macho k27/segaddrs.sh",
            ],
        )
        self.assertEqual(
            calls,
            [expected_kext_args(poc, name) for name in ("XnuCPU", "XnuDisk", "XnuDrm")],
        )

        result, calls, poc, events = run_rebuild(fail_kext="XnuDisk")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(
            events,
            [
                "build",
                "fit out/k27/kernel-k27.macho k27/segaddrs.sh",
                "build",
                "check --check out/k27/kernel-k27.macho k27/segaddrs.sh",
            ],
        )
        self.assertEqual(
            calls,
            [expected_kext_args(poc, name) for name in ("XnuCPU", "XnuDisk")],
        )
        self.assertIn("kext XnuDisk is required for the PureDarwin collection", result.stdout)

        result, calls, _poc, events = run_rebuild(fail_layout_check=True)
        self.assertEqual(result.returncode, 42, result.stdout)
        self.assertEqual(
            events,
            [
                "build",
                "fit out/k27/kernel-k27.macho k27/segaddrs.sh",
                "build",
                "check --check out/k27/kernel-k27.macho k27/segaddrs.sh",
            ],
        )
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
