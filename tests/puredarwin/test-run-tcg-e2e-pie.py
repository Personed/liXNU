#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise strict PIE evidence and QEMU CPU selection with a fake QEMU process."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


RUNNER = Path(__file__).with_name("run-tcg-e2e.py")


def run_with_serial(
    fake_qemu: Path,
    root: Path,
    serial: str,
    *,
    qemu_cpu: str | None = None,
    require_pie: bool = True,
) -> subprocess.CompletedProcess[str]:
    kernel = root / "Image"
    initramfs = root / "initramfs.cpio.gz"
    disk = root / "pd.img"
    serial_log = root / "serial.log"
    for path in (kernel, initramfs, disk):
        path.write_bytes(b"fixture")
    env = os.environ.copy()
    env["FAKE_QEMU_SERIAL"] = serial
    env["FAKE_QEMU_EXPECTED_CPU"] = qemu_cpu or "max"
    command = [sys.executable, str(RUNNER), "--qemu", str(fake_qemu)]
    if qemu_cpu is not None:
        command.extend(("--qemu-cpu", qemu_cpu))
    if require_pie:
        command.append("--require-pie")
    command.extend(
        [
            "--qemu-memory", "6G",
            "--linux-memory", "2G",
            "--fki-carve-base", "0xc0000000",
            "--fki-carve-mb", "3072",
            "--xnu-ram-mb", "2816",
            str(kernel), str(initramfs), str(disk), str(serial_log),
        ]
    )
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="xnulinux-pie-runner-") as tmp:
        root = Path(tmp)
        fake_qemu = root / "qemu-system-aarch64"
        fake_qemu.write_text(
            "#!/usr/bin/env python3\n"
            "import os, sys\n"
            "from pathlib import Path\n"
            "if '--version' in sys.argv:\n"
            "    print('QEMU emulator version 11.1.0')\n"
            "    raise SystemExit(0)\n"
            "cpu = sys.argv[sys.argv.index('-cpu') + 1]\n"
            "assert cpu == os.environ['FAKE_QEMU_EXPECTED_CPU'], (cpu, os.environ['FAKE_QEMU_EXPECTED_CPU'])\n"
            "serial = sys.argv[sys.argv.index('-serial') + 1]\n"
            "assert serial.startswith('file:')\n"
            "Path(serial[5:]).write_text(os.environ['FAKE_QEMU_SERIAL'])\n"
        )
        fake_qemu.chmod(0o755)

        common = (
            "fki: 3072 MiB at 0xc0000000 for the foreign kernel's DRAM\n"
            "xnu: contiguous DRAM at phys 0xc0000000\n"
            "xnu: bootstrap window write probe passed\n"
            "xnu: bootstrap window execute probe passed\n"
            "xnu: probe port_crumb (rw) 0x26\n"
            "xnu: probe exec strlen through XNU tables 8\n"
            "xnu: probes done\n"
            "fki: darwin console: Darwin\n"
            "fki: darwin console: XNULINUX_USERLAND_OK\n"
            "xnu: the hosted system halted\n"
            "XNULINUX_E2E_PUREDARWIN_USERLAND_OK\n"
            "XNULINUX_E2E_GUEST_HALT_OK\n"
            "XNULINUX_E2E_XNU_UNLOADED\n"
            "reboot: Power down\n"
        )
        cases = (
            (
                "default max CPU with PIE=1 reaches userland",
                common + "xnu: XNU PIE mapping mode 0x1\nxnu: host TCR2_EL1 0x2\nxnu: host TCR2_EL1.PIE 0x1\n",
                0,
                "",
                None,
                True,
            ),
            (
                "explicit max CPU with PIE=1 reaches userland",
                common + "xnu: XNU PIE mapping mode 0x1\nxnu: host TCR2_EL1 0x2\nxnu: host TCR2_EL1.PIE 0x1\n",
                0,
                "",
                "max",
                True,
            ),
            (
                "PIE=0 is rejected",
                common + "xnu: XNU PIE mapping mode 0x0\nxnu: host TCR2_EL1 0x0\nxnu: host TCR2_EL1.PIE 0x0\n",
                1,
                "requires TCR2_EL1.PIE=1",
                None,
                True,
            ),
            (
                "unsupported TCR2 is rejected",
                common + "xnu: XNU PIE mapping mode 0x0\nxnu: host TCR2_EL1 unavailable; using direct permissions\n",
                1,
                "requires TCR2_EL1.PIE=1",
                None,
                True,
            ),
            (
                "PIE=1 with XNU direct mode is rejected",
                common + "xnu: XNU PIE mapping mode 0x0\nxnu: host TCR2_EL1 0x2\nxnu: host TCR2_EL1.PIE 0x1\n",
                1,
                "requires XNU PIE mapping mode=1",
                None,
                True,
            ),
            (
                "PIE=1 without the XNU writable-table probe is rejected",
                common.replace("xnu: probe port_crumb (rw) 0x26\n", "")
                + "xnu: XNU PIE mapping mode 0x1\nxnu: host TCR2_EL1 0x2\nxnu: host TCR2_EL1.PIE 0x1\n",
                1,
                "xnu: probe port_crumb (rw)",
                None,
                True,
            ),
        )
        for name, serial, want_status, expected_error, qemu_cpu, require_pie in cases:
            result = run_with_serial(fake_qemu, root, serial, qemu_cpu=qemu_cpu, require_pie=require_pie)
            output = result.stdout + result.stderr
            if result.returncode != want_status or expected_error not in output:
                print(f"FAIL: {name}: exit={result.returncode}, expected={want_status}", file=sys.stderr)
                print(output, file=sys.stderr)
                return 1
            if want_status == 0 and "PureDarwin ARM64 console boot and guest-requested halt passed" not in output:
                print(f"FAIL: {name}: success marker missing", file=sys.stderr)
                return 1
            if want_status == 0:
                for evidence in (
                    "xnu: host TCR2_EL1.PIE 0x1",
                    "xnu: XNU PIE mapping mode 0x1",
                    "xnu: probe port_crumb (rw)",
                    "xnu: probe exec strlen through XNU tables",
                ):
                    if evidence not in output:
                        print(f"FAIL: {name}: evidence not printed: {evidence}", file=sys.stderr)
                        return 1
            print(f"PASS: {name}")

        direct_serial = (
            common
            + "xnu: XNU PIE mapping mode 0x0\n"
            + "xnu: host TCR2_EL1 0x0\n"
            + "xnu: host TCR2_EL1.PIE 0x0\n"
        )
        direct_result = run_with_serial(
            fake_qemu, root, direct_serial, qemu_cpu="cortex-a710", require_pie=False
        )
        if direct_result.returncode != 0 or "cortex-a710 CPU" not in direct_result.stdout:
            print("FAIL: explicit cortex-a710 direct profile did not reach clean halt", file=sys.stderr)
            print(direct_result.stdout + direct_result.stderr, file=sys.stderr)
            return 1
        print("PASS: explicit cortex-a710 direct profile is forwarded to QEMU")
    return 0


if __name__ == "__main__":
    sys.exit(main())
