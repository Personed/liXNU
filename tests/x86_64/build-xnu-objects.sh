#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Compile the Mach-O/ADT/x86 parser and boot_args validation objects for the
# host architecture without linking them into xnu.ko.
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR" >&2
  exit 2
fi
if [ "$(uname -m)" != x86_64 ]; then
  echo "this compile-only check must run on x86_64; got $(uname -m)" >&2
  exit 1
fi

KDIR=$(cd "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

cp -a "$REPO/xnu-ko" "$BUILD/xnu-ko"
cp -a "$REPO/fki-ko" "$BUILD/fki-ko"
make -C "$KDIR" ARCH=x86_64 M="$BUILD/xnu-ko" \
  macho.o macho_parse_x86_64.o adt.o x86_64_boot_args.o x86_64_boot_layout.o
for object in macho.o macho_parse_x86_64.o adt.o x86_64_boot_args.o x86_64_boot_layout.o; do
  object_path="$BUILD/xnu-ko/$object"
  test -s "$object_path"
  header=$(LC_ALL=C readelf -h "$object_path")
  printf '%s\n' "$header" | grep -Fq 'Class:                             ELF64'
  printf '%s\n' "$header" | grep -Fq 'Machine:                           Advanced Micro Devices X86-64'
done
