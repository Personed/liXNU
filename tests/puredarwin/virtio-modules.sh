# SPDX-License-Identifier: GPL-2.0-only
# Shared detection for VirtIO kernel modules that prepare-guest.sh can normalize.

virtio_module_path() {
  local modules_dir="$1"
  local driver="$2"
  find "$modules_dir" -type f \
    \( -name "$driver.ko" -o -name "$driver.ko.zst" -o -name "$driver.ko.xz" \) \
    -print -quit
}
