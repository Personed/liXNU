# SPDX-License-Identifier: GPL-2.0-only
#!/bin/sh
set -eu
cd "$(dirname "$0")"
cc -Wall -Wextra -Werror -O2 -o /tmp/test-fki-x86-exception-ownership test-fki-x86-exception-ownership.c
/tmp/test-fki-x86-exception-ownership
