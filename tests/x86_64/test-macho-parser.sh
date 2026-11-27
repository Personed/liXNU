#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  "$REPO/tests/x86_64/macho_parser_test.c" \
  "$REPO/xnu-ko/macho_parse_x86_64.c" \
  -o "$BUILD/macho-parser-test"
"$BUILD/macho-parser-test"
