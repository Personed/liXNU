#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Host fixtures for the source-pinned XNU x86 CPU preflight.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
  "$REPO/xnu-ko/x86_64_cpu_compat.c" \
  "$REPO/tests/x86_64/test-x86-cpu-compat.c" \
  -o "$BUILD/test-x86-cpu-compat"
"$BUILD/test-x86-cpu-compat"
echo 'xnu x86 CPU eligibility fixtures passed'
