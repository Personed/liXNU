#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -Wstrict-prototypes \
  -I"$REPO/xnu-ko" "$REPO/tests/x86_64/test-x86-page-table-audit.c" \
  "$REPO/xnu-ko/x86_64_page_table_audit.c" "$REPO/xnu-ko/x86_64_boot_ptd.c" \
  -o "$TMP/test-x86-page-table-audit"
"$TMP/test-x86-page-table-audit"
