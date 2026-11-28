#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -Wstrict-prototypes \
  -I"$REPO/fki-ko" "$REPO/tests/x86_64/test-fki-x86-context.c" -o "$TMP/test-fki-x86-context"
"$TMP/test-fki-x86-context"
