#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Compile xnu_main.c for the host architecture using a caller-provided kernel build tree.
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR" >&2
  exit 2
fi

KDIR=$(cd "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

cp -a "$REPO/xnu-ko" "$BUILD/xnu-ko"
cp -a "$REPO/fki-ko" "$BUILD/fki-ko"
cp -a "$REPO/xnu-patches" "$BUILD/xnu-patches"
make -C "$KDIR" M="$BUILD/xnu-ko" xnu_main.o
test -s "$BUILD/xnu-ko/xnu_main.o"
