#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Compile the Mach-O/ADT validation objects for the host architecture without linking them into xnu.ko.
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
make -C "$KDIR" M="$BUILD/xnu-ko" macho.o adt.o
test -s "$BUILD/xnu-ko/macho.o"
test -s "$BUILD/xnu-ko/adt.o"
