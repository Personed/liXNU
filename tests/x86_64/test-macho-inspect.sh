#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf -- "$BUILD"' EXIT

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  "$REPO/tools/dev/x86_64/xnu-macho-inspect.c" \
  "$REPO/xnu-ko/macho_parse_x86_64.c" \
  -o "$BUILD/xnu-macho-inspect"
python3 "$REPO/tests/x86_64/test-macho-inspect.py" "$BUILD/xnu-macho-inspect"
LC_ALL=C PYTHONUTF8=0 python3 "$REPO/tests/x86_64/test-macho-inspect.py" \
  "$BUILD/xnu-macho-inspect"
