#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Usage: run.sh ARM64_KERNEL STATIC_BUSYBOX FKI_KO XNU_KO FKI_CARVE_PROBE_KO
# Build and boot a temporary guest for the QEMU module and carve-out smoke test.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
"$here/prepare.sh" "$@" "$work"
python3 "$here/boot.py" "$work"
