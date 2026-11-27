#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build and verify the x86_64 FKI module against a caller-provided kernel build tree.
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR" >&2
  exit 2
fi

KDIR=$(cd -- "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf -- "$BUILD"' EXIT

cp -a "$REPO/fki-ko" "$BUILD/fki-ko"
make -C "$KDIR" ARCH=x86_64 M="$BUILD/fki-ko" CONFIG_DEBUG_INFO_BTF_MODULES= modules
module="$BUILD/fki-ko/fki.ko"
if [ ! -s "$module" ]; then
  echo "Kbuild did not produce fki.ko with the headers at $KDIR" >&2
  exit 1
fi
header=$(LC_ALL=C readelf -h "$module")
if ! grep -Eq '^[[:space:]]*Class:[[:space:]]+ELF64$' <<< "$header" ||
   ! grep -Eq '^[[:space:]]*Machine:[[:space:]]+Advanced Micro Devices X86-64$' <<< "$header"; then
  echo "Kbuild did not produce an x86_64 ELF module with the headers at $KDIR" >&2
  exit 1
fi
