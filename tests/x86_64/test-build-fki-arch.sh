#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/bin" "$fixture/headers"
touch "$fixture/headers/Makefile"

# An ELF64 little-endian relocatable header with e_machine=EM_AARCH64.
python3 - "$fixture/arm.ko" <<'PY'
import struct
import sys
from pathlib import Path

header = struct.pack(
    "<16sHHIQQQIHHHHHH", b"\x7fELF\2\1\1", 1, 183, 1,
    0, 0, 0, 0, 64, 0, 0, 0, 0, 0,
)
Path(sys.argv[1]).write_bytes(header)
PY

cat > "$fixture/bin/make" <<'MAKE'
#!/bin/sh
set -eu
arch= output=
for arg; do
  case "$arg" in
    ARCH=*) arch=${arg#ARCH=} ;;
    M=*) output=${arg#M=} ;;
  esac
done
test "$arch" = x86_64
test -n "$output"
cp -- "$TEST_MODULE" "$output/fki.ko"
MAKE
chmod +x "$fixture/bin/make"

if PATH="$fixture/bin:$PATH" ARCH=arm64 TEST_MODULE="$fixture/arm.ko" \
    bash "$repo/tests/x86_64/build-fki.sh" "$fixture/headers" > "$fixture/log" 2>&1; then
  echo 'x86_64 FKI build helper accepted an AArch64 module' >&2
  exit 1
fi
if ! grep -Fq 'did not produce an x86_64 ELF module' "$fixture/log"; then
  cat "$fixture/log" >&2
  exit 1
fi
