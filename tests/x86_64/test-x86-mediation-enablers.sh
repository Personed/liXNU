#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
cc -O2 -Wall -Wextra -Werror -o "$tmp/x86-mediation-enablers-test" test-x86-mediation-enablers.c
"$tmp/x86-mediation-enablers-test"
