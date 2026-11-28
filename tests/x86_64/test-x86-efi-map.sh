#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CC_BIN=${CC:-cc}

"$CC_BIN" -std=c11 -O2 -Wall -Wextra -Werror -Wformat=2 \
  -Wstrict-prototypes -I"$REPO/xnu-ko" \
  "$REPO/tests/x86_64/test-x86-efi-map.c" \
  "$REPO/xnu-ko/x86_64_boot_args.c" "$REPO/xnu-ko/x86_64_boot_memory_map.c" \
  -o "$TMP/test-x86-efi-map"
"$TMP/test-x86-efi-map"
