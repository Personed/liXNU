#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that the free collection builder exports a canonical DVM path."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/free-kc/build.sh"


class FreeKernelCollectionPathTest(unittest.TestCase):
    def test_relative_dvm_is_canonicalized_before_build_commands(self):
        source = SCRIPT.read_text(encoding="utf-8")
        pd_revision = re.search(r"\bPD_REV=([0-9a-f]+)", source).group(1)
        mbedtls_revision = re.search(
            r"^MBEDTLS_REV=(.+)$",
            (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
            re.MULTILINE,
        ).group(1)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            launch = root / "launch"
            vm = root / "vm"
            fake_bin = root / "bin"
            poc_target = root / "repo with spaces"
            poc_link = root / "poc"
            launch.mkdir()
            fake_bin.mkdir()
            (poc_target / "kexts/free/corecrypto").mkdir(parents=True)
            (poc_target / "scripts/free-kc").mkdir(parents=True)
            (poc_target / "kexts/free/corecrypto/mbedtls.rev").write_text(
                (ROOT / "kexts/free/corecrypto/mbedtls.rev").read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            (poc_target / "scripts/free-kc/kcc.py").touch()
            poc_link.symlink_to(poc_target, target_is_directory=True)
            kernel_framework = (
                vm
                / "out/x12377/build/xnu/k27h-dst/System/Library/Frameworks/Kernel.framework/Versions/A"
            )
            (kernel_framework / "PrivateHeaders").mkdir(parents=True)

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
                "printf '%s\\n%s\\n' \"$DVM\" \"$POC\" > \"$DVM_CAPTURE\"\n"
                "exit 73\n",
                encoding="utf-8",
            )
            python.chmod(0o755)

            capture = root / "dvm-value.txt"
            env = os.environ.copy()
            env.update(
                {
                    "DVM": "../vm",
                    "DVM_CAPTURE": str(capture),
                    "FAKE_PD_REV": pd_revision,
                    "FAKE_MBEDTLS_REV": mbedtls_revision,
                    "FREE_KC_WORK": str(root / "free-kc-work"),
                    "OUT": str(root / "kc-free.bin"),
                    "POC": str(poc_link),
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

            self.assertEqual(result.returncode, 73, result.stdout)
            self.assertEqual(
                capture.read_text(encoding="utf-8").splitlines(),
                [str(vm.resolve()), str(poc_link.absolute())],
            )

    def test_xnu_rebuild_resolves_symlink_with_cdpath_set(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            launch = root / "launch"
            vm = root / "vm-real"
            alternate_vm = root / "vm-alternate"
            fake_bin = root / "bin"
            launch.mkdir()
            fake_bin.mkdir()
            (launch / "vm-link").symlink_to(vm, target_is_directory=True)
            (root / "vm-link").symlink_to(alternate_vm, target_is_directory=True)
            (alternate_vm / "out/k27/xnu").mkdir(parents=True)

            builder = alternate_vm / "k27/build-kernel27.sh"
            builder.parent.mkdir(parents=True)
            builder.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n%s\\n' \"$PWD\" \"$DVM\" > \"$DVM_CAPTURE\"\n"
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
                    "CDPATH": "..",
                    "DVM": "vm-link",
                    "DVM_CAPTURE": str(capture),
                    "PATH": str(fake_bin) + os.pathsep + env["PATH"],
                }
            )
            result = subprocess.run(
                ["bash", str(ROOT / "scripts/vm-xnu-rebuild.sh")],
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
                [str(alternate_vm.resolve())] * 2,
            )


if __name__ == "__main__":
    unittest.main()
