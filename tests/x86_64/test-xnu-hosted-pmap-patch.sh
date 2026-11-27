#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/osfmk/i386"
PIN=ac9718fb1af618d5ce8678d0dc6e8a58f252216f
SOURCE="$STAGE/osfmk/i386/i386_init.c"
PATCH="$REPO/scripts/x86_64/experimental-owned-pmap.patch"

if [ "$#" -eq 0 ]; then
  curl --fail --silent --show-error --location --max-time 60 \
    "https://raw.githubusercontent.com/apple-oss-distributions/xnu/$PIN/osfmk/i386/i386_init.c" \
    -o "$SOURCE"
elif [ "$#" -eq 1 ]; then
  cp -- "$1" "$SOURCE"
else
  echo "usage: $0 [PINNED_I386_INIT_C]" >&2
  exit 2
fi

check_source() {
  printf '%s  %s\n' '37df8205df9ae1e594bba4642a5f106a44eba3e9f9fc65ffc6c0c725b66ad492' \
    "$SOURCE" | sha256sum -c -
}

check_source
git -C "$STAGE" init -q
git -C "$STAGE" apply --whitespace=error "$PATCH"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -Wstrict-prototypes \
  -I"$STAGE/osfmk" "$REPO/tests/x86_64/test-xnu-hosted-boot-memory.c" \
  -o "$STAGE/test-boot-memory"
"$STAGE/test-boot-memory"
git -C "$STAGE" apply --whitespace=error --reverse "$PATCH"
check_source
test ! -e "$STAGE/osfmk/i386/linux_hosted_boot_memory.h"
