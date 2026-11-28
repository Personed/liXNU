#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
cd "$(dirname "$0")"
cc -Wall -Wextra -Werror -O2 -o /tmp/test-fki-x86-address-space-coexistence test-fki-x86-address-space-coexistence.c
/tmp/test-fki-x86-address-space-coexistence
