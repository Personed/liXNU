#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -Wformat=2 \
  -Wstrict-prototypes -I"$REPO/xnu-ko" \
  "$REPO/tests/x86_64/test-x86-boot-layout.c" \
  "$REPO/xnu-ko/x86_64_boot_layout.c" "$REPO/xnu-ko/x86_64_boot_args.c" \
  -o "$TMP/test-x86-boot-layout"
"$TMP/test-x86-boot-layout"
