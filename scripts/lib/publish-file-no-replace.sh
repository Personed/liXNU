# SPDX-License-Identifier: GPL-2.0-only
# Publish a same-filesystem staging file without replacing any destination.
publish_file_no_replace() {
  local source=$1 destination=$2
  if [ -e "$destination" ] || [ -L "$destination" ]; then
    echo "Refusing to replace existing output path: $destination" >&2
    return 1
  fi
  # `link` invokes link(2) with the destination as a path, so a directory that
  # appears after the check fails with EEXIST instead of receiving the image.
  if ! link "$source" "$destination"; then
    echo "Cannot publish $destination; it may have appeared during generation, or the output filesystem may not support hard links." >&2
    return 1
  fi
  rm "$source"
}
