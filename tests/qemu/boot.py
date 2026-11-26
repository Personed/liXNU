#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot the ARM64 module and FKI carve-out smoke test with QEMU TCG."""

import argparse
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

SUCCESS_MARKER = "xnulinux QEMU smoke test passed"


def guest_reported_success(log):
    """Accept the unique success marker even if shutdown text shares its line."""
    return log.count(SUCCESS_MARKER) == 1 and any(
        line.startswith(SUCCESS_MARKER) for line in log.splitlines()
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path, help="directory with Image and initramfs from prepare.sh")
    args = parser.parse_args()
    kernel = args.images / "Image"
    initramfs = args.images / "initramfs"
    for path in (kernel, initramfs):
        if not path.is_file():
            parser.error(f"missing boot input: {path}")

    with TemporaryDirectory() as work:
        console = Path(work) / "console.log"
        command = [
            "qemu-system-aarch64",
            "-machine", "virt,virtualization=off",
            "-accel", "tcg",
            "-cpu", "max",
            "-smp", "2",
            "-m", "1024",
            "-nodefaults",
            "-display", "none",
            "-serial", f"file:{console}",
            "-monitor", "none",
            "-no-reboot",
            "-kernel", str(kernel),
            "-initrd", str(initramfs),
            "-append", "console=ttyAMA0 rdinit=/init panic=1 loglevel=4 mem=768M",
        ]
        try:
            result = subprocess.run(command, timeout=180)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"QEMU could not complete the smoke test: {error}", file=sys.stderr)
            return 1
        finally:
            log = console.read_text(encoding="utf-8", errors="replace") if console.exists() else ""
            print(log, end="")

    if result.returncode:
        return result.returncode
    if not guest_reported_success(log):
        print("Guest did not report a successful smoke test", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
