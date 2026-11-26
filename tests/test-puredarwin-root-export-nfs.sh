#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
if [ "$EUID" -eq 0 ]; then
  echo 'Run this NFS integration test as an unprivileged user.' >&2
  exit 1
fi
for tool in sudo systemctl exportfs mount mountpoint sfdisk mkfs.ext4 tar gzip stat link; do
  command -v "$tool" >/dev/null || { echo "Missing NFS test dependency: $tool" >&2; exit 1; }
done

tmp=$(mktemp -d)
share=$tmp/export
mount_dir=$tmp/client
mount_dirs=$tmp/mount-directories
link_inodes=$tmp/link-inodes
rpcbind_started=0
nfs_started=0
export_registered=0
if ! sudo systemctl is-active --quiet rpcbind; then rpcbind_started=1; fi
if ! sudo systemctl is-active --quiet nfs-kernel-server; then nfs_started=1; fi

cleanup() {
  local status=$?
  trap - EXIT
  set +e
  if mountpoint -q -- "$mount_dir"; then sudo umount -- "$mount_dir"; fi
  if [ "$export_registered" -eq 1 ]; then
    sudo exportfs -u "127.0.0.1:$share"
  fi
  if [ "$nfs_started" -eq 1 ]; then sudo systemctl stop nfs-kernel-server; fi
  if [ "$rpcbind_started" -eq 1 ]; then sudo systemctl stop rpcbind; fi
  rm -rf -- "$tmp"
  exit "$status"
}
trap cleanup EXIT

mkdir -m 0755 -- "$share"
mkdir -m 0700 -- "$share/tmp" "$share/output" "$mount_dir"
mkdir -m 0700 -- "$tmp/root"
printf 'root-squashed export smoke test\n' >"$tmp/root/hello.txt"

partition=$tmp/root.ext4
root_sectors=65536
root_start=2048
truncate -s "$((root_sectors * 512))" "$partition"
mkfs.ext4 -q -F -d "$tmp/root" "$partition"
image=$tmp/puredarwin.img
truncate -s "$(((root_start + root_sectors + 4096) * 512))" "$image"
sfdisk --quiet "$image" <<PARTITIONS
label: gpt
unit: sectors

start=$root_start, size=$root_sectors, type=linux, name="Darwin ext4 Root"
PARTITIONS
dd if="$partition" of="$image" bs=512 seek="$root_start" conv=notrunc status=none

sudo systemctl start rpcbind
sudo systemctl start nfs-kernel-server
sudo exportfs -i -o rw,sync,root_squash,no_subtree_check "127.0.0.1:$share"
export_registered=1
sudo mount -t nfs -o vers=3,proto=tcp,nolock "127.0.0.1:$share" "$mount_dir"

# The mounted NFS directory is writable by this user, while the server maps
# client root to nobody. This proves the export enforces root_squash.
: >"$mount_dir/tmp/user-write-probe"
rm -- "$mount_dir/tmp/user-write-probe"
if sudo test -w "$mount_dir/tmp"; then
  echo 'The local NFS export allowed root to write; root_squash is not active.' >&2
  exit 1
fi

bin=$tmp/bin
mkdir -- "$bin"
real_mkdir=$(command -v mkdir)
real_link=$(command -v link)
real_stat=$(command -v stat)
cat >"$bin/mkdir" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
last=''
for arg do last=$arg; done
case "$last" in
  */.puredarwin-root-mount.*) printf '%s\n' "$last" >>"@NFS_MOUNT_DIRS@" ;;
esac
exec "@REAL_MKDIR@" "$@"
WRAPPER
cat >"$bin/link" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
source=$1
destination=$2
"@REAL_LINK@" "$@"
source_identity=$("@REAL_STAT@" -c '%d:%i' -- "$source")
destination_identity=$("@REAL_STAT@" -c '%d:%i' -- "$destination")
printf '%s\n%s\n' "$source_identity" "$destination_identity" >"@NFS_LINK_INODES@"
WRAPPER
sed -i "s|@REAL_MKDIR@|$real_mkdir|g; s|@NFS_MOUNT_DIRS@|$mount_dirs|g" "$bin/mkdir"
sed -i "s|@REAL_LINK@|$real_link|g; s|@REAL_STAT@|$real_stat|g; s|@NFS_LINK_INODES@|$link_inodes|g" "$bin/link"
chmod +x "$bin/mkdir" "$bin/link"
nfs_output=$mount_dir/output/root.tar.gz
PATH="$bin:$PATH" TMPDIR="$mount_dir/tmp" "$script" "$image" "$nfs_output"

mapfile -t observed_mount_dirs <"$mount_dirs"
if [ "${#observed_mount_dirs[@]}" -lt 2 ]; then
  echo 'Exporter did not try both the root-squashed TMPDIR and local /tmp.' >&2
  exit 1
fi
case "${observed_mount_dirs[0]}" in
  "$mount_dir"/tmp/.puredarwin-root-mount.*) ;;
  *) echo "Exporter did not try NFS TMPDIR first: ${observed_mount_dirs[0]}" >&2; exit 1 ;;
esac
fallback_mount_dir=${observed_mount_dirs[${#observed_mount_dirs[@]}-1]}
case "$fallback_mount_dir" in
  /tmp/.puredarwin-root-mount.*) ;;
  *) echo "Exporter did not fall back to local /tmp: $fallback_mount_dir" >&2; exit 1 ;;
esac
[ ! -e "$fallback_mount_dir" ]
[ "$(stat -c '%u' -- "$nfs_output")" = "$EUID" ]
[ "$(stat -c '%a' -- "$nfs_output")" = 644 ]
mapfile -t published_inodes <"$link_inodes"
[ "${#published_inodes[@]}" -eq 2 ]
[ "${published_inodes[0]}" = "${published_inodes[1]}" ]
gzip -t "$nfs_output"
tar -tzf "$nfs_output" | grep -Fx './hello.txt' >/dev/null
[ "$(tar -xOzf "$nfs_output" ./hello.txt)" = 'root-squashed export smoke test' ]

echo 'PureDarwin root exporter root-squashed NFS integration test passed.'
