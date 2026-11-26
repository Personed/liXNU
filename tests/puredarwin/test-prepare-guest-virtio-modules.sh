#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

here=$(cd "$(dirname -- "$0")" && pwd)
source "$here/virtio-modules.sh"

for tool in python3 gzip xz zstd; do
  command -v "$tool" >/dev/null 2>&1 || {
    printf 'Missing required test tool: %s\n' "$tool" >&2
    exit 2
  }
done

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
fakebin="$work/bin"
mkdir -p "$fakebin"

cat > "$fakebin/busybox" <<'BUSYBOX'
#!/bin/sh
if [ "${1-}" = --list ]; then
  echo modprobe
  exit 0
fi
exit 2
BUSYBOX
cat > "$fakebin/depmod" <<'DEPMOD'
#!/bin/sh
exit 0
DEPMOD
cat > "$fakebin/cpio" <<'CPIO'
#!/bin/sh
python3 -c 'import os, sys; open(os.environ["CPIO_FILE_LIST"], "wb").write(sys.stdin.buffer.read().replace(b"\0", b"\n"))'
CPIO
chmod +x "$fakebin/busybox" "$fakebin/depmod" "$fakebin/cpio"

kernel="$work/Image"
busybox="$fakebin/busybox"
fki="$work/fki.ko"
xnu="$work/xnu.ko"
kc="$work/kc-free.bin"
conf="$work/pd.conf"
printf 'kernel fixture\n' > "$kernel"
printf 'fki fixture\n' > "$fki"
printf 'xnu fixture\n' > "$xnu"
printf 'kc fixture\n' > "$kc"
printf 'ROOT_UUID=fixture\nBOOT_UUID=fixture\n' > "$conf"

release=7.0.0-44-generic
module_rel=kernel/drivers/virtio/virtio_mmio
for suffix in ko ko.zst ko.xz; do
  modules="$work/modules-$suffix"
  output="$work/out-$suffix"
  module="$modules/$release/$module_rel.$suffix"
  mkdir -p "$(dirname -- "$module")" "$output"
  case "$suffix" in
    ko) printf 'module fixture\n' > "$module" ;;
    ko.zst) printf 'module fixture\n' | zstd -q -c > "$module" ;;
    ko.xz) printf 'module fixture\n' | xz -c > "$module" ;;
  esac

  detected=$(virtio_module_path "$modules" virtio_mmio)
  test "$detected" = "$module" || {
    echo "virtio_module_path did not recognize $suffix" >&2
    exit 1
  }

  file_list="$work/cpio-$suffix.list"
  CPIO_FILE_LIST="$file_list" PATH="$fakebin:$PATH" \
    "$here/prepare-guest.sh" "$kernel" "$busybox" "$fki" "$xnu" "$kc" "$conf" "$modules/$release" "$output"
  grep -Fqx "./lib/modules/$release/$module_rel.ko" "$file_list" || {
    echo "prepare-guest did not stage $suffix as an uncompressed .ko" >&2
    exit 1
  }
  if grep -Eq '\.ko\.(zst|xz)$' "$file_list"; then
    echo "compressed $suffix file remained in the initramfs staging list" >&2
    exit 1
  fi
done

unsupported="$work/unsupported/$release/kernel/drivers/virtio/virtio_mmio.ko.gz"
mkdir -p "$(dirname -- "$unsupported")"
touch "$unsupported"
test -z "$(virtio_module_path "$work/unsupported/$release" virtio_mmio)" || {
  echo 'unsupported .ko.gz was incorrectly treated as loadable' >&2
  exit 1
}

# The guest init uses modprobe only when the normalized .ko exists.
grep -Fq 'find "/lib/modules/$(uname -r)" -type f -name "$module.ko"' "$here/init" || {
  echo 'guest init no longer looks for normalized .ko files' >&2
  exit 1
}

echo 'VirtIO .ko, .ko.zst, and .ko.xz formats all stage as loadable .ko files; .ko.gz is rejected.'
