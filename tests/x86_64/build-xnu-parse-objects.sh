#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
	printf 'usage: %s KERNEL_BUILD_DIR\n' "$0" >&2
	exit 2
fi
case "$(uname -m)" in
x86_64) ;;
*) printf 'this compile-only check must run on x86_64; got %s\n' "$(uname -m)" >&2; exit 1 ;;
esac

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
make -C "$1" M="$repo_root/xnu-ko" macho.o adt.o

for object in macho.o adt.o; do
	object_path=$repo_root/xnu-ko/$object
	test -f "$object_path"
	header=$(LC_ALL=C readelf -h "$object_path")
	printf '%s\n' "$header" | grep -Fq 'Class:                             ELF64'
	printf '%s\n' "$header" | grep -Fq 'Machine:                           Advanced Micro Devices X86-64'
done
