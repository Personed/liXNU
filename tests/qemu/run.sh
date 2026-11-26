#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build and boot a temporary guest for the QEMU module smoke test.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
"$here/prepare.sh" "$@" "$work"
python3 "$here/boot.py" "$work"
