#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
export LC_ALL=C

if [[ $# != 1 ]]; then
    echo "Usage: $0 REPORT.json" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/tahoe-kdk.XXXXXXXX")
image="$work/KDK.dmg"
mount="$work/mount"
mounted=false
cleanup() {
    local status=$?
    if "$mounted" && ! /usr/bin/hdiutil detach "$mount" &&
        ! /usr/bin/hdiutil detach -force "$mount"; then
        echo "Could not detach the temporary KDK image" >&2
        exit 1
    fi
    rm -rf -- "$work" || exit 1
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

/usr/bin/curl --fail --silent --show-error --location --retry 3 \
    --proto '=https' --proto-redir '=https' --max-filesize 1181028172 \
    'https://github.com/dortania/KdkSupportPkg/releases/download/25G342/Kernel_Debug_Kit_26.7.3_build_25G342.dmg' \
    -o "$image"
test "$(/usr/bin/stat -f %z "$image")" -eq 1181028172
printf '%s  %s\n' '219f27c7e039b6ab3426496e0d5852d55a31d00b3d3984d855cba04377c5dba0' "$image" |
    /usr/bin/shasum -a 256 -c -

mkdir "$mount"
/usr/bin/hdiutil attach -readonly -nobrowse -mountpoint "$mount" "$image"
mounted=true
pkg="$mount/KernelDebugKit.pkg"
if [[ ! -f "$pkg" ]]; then
    pkg="$mount/Kernel Debug Kit/KernelDebugKit.pkg"
fi
test -f "$pkg"
test ! -L "$pkg"
signature=$(/usr/sbin/pkgutil --check-signature "$pkg")
printf '%s\n' "$signature"
grep -Eq '^[[:space:]]*Status: signed Apple Software[[:space:]]*$' <<<"$signature"
grep -Eq '^[[:space:]]*1\.[[:space:]]+Software Update[[:space:]]*$' <<<"$signature"
grep -Eq '^[[:space:]]*2\.[[:space:]]+Apple Software Update Certification Authority[[:space:]]*$' <<<"$signature"
grep -Eq '^[[:space:]]*3\.[[:space:]]+Apple Root CA[[:space:]]*$' <<<"$signature"

/usr/sbin/pkgutil --expand-full "$pkg" "$work/expanded"
python3 "$script_dir/audit-tahoe-kdk.py" "$work/expanded/KDK.pkg" "$work/report.json"
/usr/bin/hdiutil detach "$mount"
mounted=false
mv -- "$work/report.json" "$1"
