#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/bin" "$fixture/headers"
touch "$fixture/headers/Makefile"

# The two headers isolate the machine and class checks.
python3 - "$fixture" <<'PY'
import struct
import sys
from pathlib import Path

directory = Path(sys.argv[1])
for name, elf_class, machine, fmt in (
    ("arm64.ko", 2, 183, "<16sHHIQQQIHHHHHH"),
    ("elf32-x86_64.ko", 1, 62, "<16sHHIIIIIHHHHHH"),
):
    ident = b"\x7fELF" + bytes((elf_class, 1, 1))
    header = struct.pack(fmt, ident, 1, machine, 1, 0, 0, 0, 0,
                         struct.calcsize(fmt), 0, 0, 0, 0, 0)
    (directory / name).write_bytes(header)
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

for module in arm64.ko elf32-x86_64.ko; do
  if PATH="$fixture/bin:$PATH" ARCH=arm64 TEST_MODULE="$fixture/$module" \
      bash "$repo/tests/x86_64/build-fki.sh" "$fixture/headers" > "$fixture/log" 2>&1; then
    echo "x86_64 FKI build helper accepted $module" >&2
    exit 1
  fi
  if ! grep -Fq 'did not produce an x86_64 ELF module' "$fixture/log"; then
    cat "$fixture/log" >&2
    exit 1
  fi
done
