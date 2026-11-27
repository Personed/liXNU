#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

if [[ $# != 4 || ! -f $1/Makefile ]]; then
  echo "usage: $0 KDIR SOURCE_COMMIT DEBUG_SOURCE_PREFIX OUTPUT_DIR" >&2
  exit 2
fi

repo=$(cd "$(dirname "$0")/../.." && pwd)
kdir=$(cd "$1" && pwd)
commit=$(git -c safe.directory="$repo" -C "$repo" rev-parse --verify --end-of-options "$2^{commit}")
source_prefix=$3
output=$4

if [[ $source_prefix != /* || $source_prefix == *[[:space:]=]* || -e $output || -L $output ]]; then
  echo "expected an absolute source prefix without spaces or '=' and a new output directory" >&2
  exit 2
fi

work=$(mktemp -d /tmp/lixnu-debian-repro.XXXXXX)
other_work=
trap 'rm -rf -- "$work"; test -z "$other_work" || rm -rf -- "$other_work"' EXIT
other_work=$(mktemp -d /tmp/lixnu-debian-repro.XXXXXX)
git -c safe.directory="$repo" -C "$repo" archive "$commit" -- fki-ko xnu-ko > "$work/source.tar"

build_copy() {
  local tree=$1
  local flag="-ffile-prefix-map=$tree=$source_prefix"

  mkdir -p -- "$tree"
  tar -xf "$work/source.tar" -C "$tree"
  make -C "$kdir" ARCH=x86_64 M="$tree/fki-ko" KCFLAGS="$flag" KAFLAGS="$flag" \
    CONFIG_DEBUG_INFO_BTF_MODULES= modules
  make -C "$kdir" ARCH=x86_64 M="$tree/xnu-ko" KCFLAGS="$flag" KAFLAGS="$flag" \
    KBUILD_EXTRA_SYMBOLS="$tree/fki-ko/Module.symvers" modules
}

first="$work/checkout"
second="$other_work/nested/checkout"
build_copy "$first"
build_copy "$second"

for module in fki-ko/fki.ko xnu-ko/xnu.ko; do
  if ! cmp -s -- "$first/$module" "$second/$module"; then
    echo "${module##*/} changed across checkout paths" >&2
    sha256sum -- "$first/$module" "$second/$module" >&2
    exit 1
  fi
  echo "${module##*/}: byte-identical across checkout paths"
done

mkdir -- "$output"
cp -- "$first/fki-ko/fki.ko" "$first/xnu-ko/xnu.ko" "$output/"
