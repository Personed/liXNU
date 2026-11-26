#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Inside the VM: apply xnulinux's XNU patches to the k27 tree, rebuild the kernel incrementally, and (when the macOS 27.2
# kernelcache is there) relink the kernel collection with Apple's kexts. The kernel's segment slots inside the collection
# (k27/segaddrs.sh) follow the kernel's size: when kc27-layout.py moves them, the kernel is linked again.
set -e -o pipefail	# a failed kernel build stops here: never repackage the previous kernel
POC=${POC:-$(dirname "$0")/..}	# this repository: its patches, kexts and linker
POC=$(CDPATH= cd -- "$POC" && pwd -P)
DVM=${DVM:-$HOME/w/dvm}
DVM=$(CDPATH= cd -- "$DVM" && pwd -P)
cd "$DVM"
export PATH=$PATH:/usr/lib/llvm-21/bin
T=out/k27/xnu
# back to the tree's own sources: every file the patches touched last time (.linux64-patched) or touch now, then the
# patches again. A file that comes out as it was keeps its old time, so the build recompiles only what changed (assert.h
# and cpu_data.h are included everywhere: touching them rebuilds the whole kernel)
P="$POC/xnu-patches"
FILES=$(cat $T/.linux64-patched 2>/dev/null; grep -h '^+++ b/' "$P"/*.patch | sed 's|^+++ b/||')
SNAP=$(mktemp -d)
for f in $FILES; do [ -e $T/$f ] && mkdir -p $SNAP/$(dirname $f) && cp -p $T/$f $SNAP/$f; done
for f in $FILES; do git -C $T checkout -q -- $f 2>/dev/null || rm -f $T/$f; done
for p in "$P"/*.patch; do git -C $T apply "$p"; echo "applied $(basename "$p")"; done
for f in $FILES; do [ -e $SNAP/$f ] && cmp -s $SNAP/$f $T/$f && touch -r $SNAP/$f $T/$f; done
rm -rf $SNAP
grep -h '^+++ b/' "$P"/*.patch | sed 's|^+++ b/||' > $T/.linux64-patched
# the machine layer's headers (shared with xnu.ko), where every component's sources find them
mkdir -p $T/EXTERNAL_HEADERS/linux64
for h in "$P"/include/*.h; do cmp -s "$h" $T/EXTERNAL_HEADERS/linux64/$(basename "$h") || cp "$h" $T/EXTERNAL_HEADERS/linux64/; done
# the corecrypto interface: xnulinux's own headers and kernel side (xnu-patches/corecrypto) in place of the tree's
# corecrypto, whose files are under Apple's corecrypto internal-use license (0008 drops them from the build). Files that
# come out the same keep their times.
mirror() {	# mirror SRC DST: DST holds exactly SRC's files
  mkdir -p "$2"
  for f in $(ls -A "$2"); do [ -e "$1/$f" ] || rm -rf "$2/$f"; done
  for f in $(ls -A "$1"); do cmp -s "$1/$f" "$2/$f" || cp "$1/$f" "$2/$f"; done
}
rm -rf $T/osfmk/corecrypto $T/osfmk/arm64/corecrypto
mirror "$P"/corecrypto/include/corecrypto $T/EXTERNAL_HEADERS/corecrypto
mirror "$P"/corecrypto/kernel $T/osfmk/xnulinux_crypto
t=$SECONDS; JOBS=12 bash k27/build-kernel27.sh 2>&1 | tail -15; echo "kernel: $((SECONDS - t)) s"
APPLE_KC=out/kc/kc27.bin
have_apple_kc=false
if [ -e "$APPLE_KC" ]; then
  have_apple_kc=true
  cp k27/segaddrs.sh /tmp/segaddrs.before
  python3 k27/kc27-layout.py "$APPLE_KC" out/k27/kernel-k27.macho --write k27/segaddrs.sh | tail -2
  if ! cmp -s /tmp/segaddrs.before k27/segaddrs.sh; then
    echo "kernel slots moved: linking the kernel again"
    JOBS=12 bash k27/build-kernel27.sh 2>&1 | tail -3
  fi
fi

# Both collections need native kexts. Build the PureDarwin set even with no Apple kernelcache.
NATIVE=()
NETINC="$POC/build/netinc"
mkdir -p "$NETINC/IOKit"
if "$have_apple_kc"; then
  KEXTS=("$POC"/kexts/*/*.json)
  # XnuEthernet needs the private SPI declarations from the open-source IONetworkingFamily.
  NETSRC="$POC/build/IONetworkingFamily-188"
  [ -e "$NETSRC/IOEthernetController.h" ] || git clone -q --depth 1 --branch IONetworkingFamily-188 https://github.com/apple-oss-distributions/IONetworkingFamily.git "$NETSRC"
  ln -sfn "$NETSRC" "$NETINC/IOKit/network"
else
  KEXTS=("$POC/kexts/XnuCPU/XnuCPU.json" "$POC/kexts/XnuDisk/XnuDisk.json" "$POC/kexts/XnuDrm/XnuDrm.json")
fi

# XnuDisk needs the SDK's IOStorageFamily headers. The macOS collection also uses its HID headers.
for sdk in "$PWD"/toolchains/sdks/*.sdk; do
  headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
  [ -f "$headers/IOKit/storage/IOBlockStorageDevice.h" ] || continue
  if "$have_apple_kc" && [ ! -f "$headers/IOKit/hid/IOHIDDevice.h" ]; then
    continue
  fi
  for family in storage hid hidsystem graphics; do
    ln -sfn "$headers/IOKit/$family" "$NETINC/IOKit/$family"
  done
  ln -sfn "$sdk/usr/include/TargetConditionals.h" "$NETINC/TargetConditionals.h"
  ln -sfn "$headers/HIDDriverKit" "$NETINC/HIDDriverKit"
  break
done
for j in "${KEXTS[@]}"; do
  n=$(basename "$j" .json)
  if ! python3 scripts/build-native-kext.py "$n" "$(dirname "$j")/$n.cpp" -I "$NETINC" --obj "${KEXT_OBJ:-k27-obj}"; then
    if ! "$have_apple_kc"; then
      echo "kext $n is required for the PureDarwin collection" >&2
      exit 1
    fi
    echo "kext $n failed to build: left out"
    continue
  fi
  NATIVE+=("$j")
done

sudo install -d /lib/firmware/xnu
if "$have_apple_kc"; then
  printf '%s\n' "${NATIVE[@]}" > out/k27/natives.txt
  # the collection (scripts/kclink): the kernel, xnulinux's kexts, and the Apple kexts of scripts/apple-kc/keep.txt from
  # macOS 27.2's kernelcache
  t=$SECONDS
  python3 "$POC"/scripts/kclink/kclink.py "$APPLE_KC" out/k27/kernel-k27.macho out/k27/kc-k27.bin \
    --keep $(grep -v '^#' "$POC"/scripts/apple-kc/keep.txt) --native "${NATIVE[@]}" \
    --rebind $(grep -v '^#' "$POC"/scripts/apple-kc/rebind.txt) > out/k27/kclink.log 2>&1 ||
    { tail -5 out/k27/kclink.log; echo "kclink failed: out/k27/kclink.log"; exit 1; }
  grep -E '^wrote|vtable check' out/k27/kclink.log; echo "collection: $((SECONDS - t)) s"
  # AMFI of the collection's macOS expects the code-signature platform version of that build; the system disk's binaries
  # carry their own (macOS 27.0.1: 26 where 27.2's AMFI expects 27)
  python3 "$POC"/scripts/kc-amfi-platform.py out/k27/kc-k27.bin ${AMFI_PLATFORM:-26}
  sudo cp out/k27/kc-k27.bin /lib/firmware/xnu/kc.bin
fi
ls -la out/k27/kernel-k27.macho
sudo cp out/k27/kernel-k27.macho /lib/firmware/xnu/kernel.macho
sync	# the VM may be stopped from the host right after (tart/krunkit stop): unsynced pages of the collection would be lost
