#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -Wstrict-prototypes \
  -I"$REPO/xnu-ko" "$REPO/tests/x86_64/test-x86-boot-preflight.c" \
  "$REPO/xnu-ko/x86_64_boot_args.c" "$REPO/xnu-ko/x86_64_boot_layout.c" \
  "$REPO/xnu-ko/x86_64_boot_memory_map.c" "$REPO/xnu-ko/x86_64_boot_ptd.c" \
  "$REPO/xnu-ko/x86_64_page_table_audit.c" -o "$TMP/test-x86-boot-preflight"
"$TMP/test-x86-boot-preflight"
