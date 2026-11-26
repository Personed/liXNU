#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the FKI module against a caller-provided Linux kernel build tree.
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR" >&2
  exit 2
fi

KDIR=$(cd "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

cp -a "$REPO/fki-ko" "$BUILD/fki-ko"
make -C "$KDIR" M="$BUILD/fki-ko" CONFIG_DEBUG_INFO_BTF_MODULES= modules
test -s "$BUILD/fki-ko/fki.ko"
