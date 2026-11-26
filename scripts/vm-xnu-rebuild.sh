#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Inside the VM: apply xnulinux's XNU patches to the k27 tree, rebuild the kernel incrementally, and (when the macOS 27.2
# kernelcache is there) relink the kernel collection with Apple's kexts. The kernel's segment slots inside the collection
# (k27/segaddrs.sh) follow the kernel's size: when kc27-layout.py moves them, the kernel is linked again.
set -e -o pipefail	# a failed kernel build stops here: never repackage the previous kernel
POC=${POC:-$(cd "$(dirname "$0")/.." && pwd)}	# this repository: its patches, kexts and linker
DVM=${DVM:-$HOME/w/dvm}
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
# The pinned XNU tree's x86-specific sanitizer denylists name two source files
# under corecrypto/. They are removed above because Apple's corecrypto sources
# are excluded from xnulinux's build, but XNU validates every exact denylist
# path even when the target is ARM64. Remove only those stale entries; the
# wildcard exclusions remain harmless and the ARM64 denylists are unchanged.
for denylist in "$T/san/memory/kasan-denylist-x86_64" "$T/san/coverage/kcov-denylist-x86_64"; do
  [ -f "$denylist" ] || continue
  sed -i \
    -e '/^src:\.\/osfmk\/corecrypto\/cc_clear\.c$/d' \
    -e '/^src:\.\/osfmk\/corecrypto\/ccdrbg_nisthmac\.c$/d' \
    "$denylist"
done
mirror "$P"/corecrypto/include/corecrypto $T/EXTERNAL_HEADERS/corecrypto
mirror "$P"/corecrypto/kernel $T/osfmk/xnulinux_crypto
mkdir -p out/k27/obj
t=$SECONDS
set +e
JOBS="${JOBS:-12}" bash k27/build-kernel27.sh 2>&1 | tee out/k27/xnulinux-rebuild.log | tail -15
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
  echo "kernel rebuild failed ($rc); showing the bounded build logs that explain missing compiler recipes"
  for log in out/k27/build-bootstrap.log out/k27/build-raw.log; do
    if [ -f "$log" ]; then
      echo "===== $log (last 120 lines) ====="
      tail -120 "$log"
    fi
  done
  exit "$rc"
fi
echo "kernel: $((SECONDS - t)) s"
sudo install -d -m 0755 /lib/firmware/xnu
if [ -e out/kc/kc27.bin ]; then
  cp k27/segaddrs.sh /tmp/segaddrs.before
  python3 k27/kc27-layout.py out/kc/kc27.bin out/k27/kernel-k27.macho --write k27/segaddrs.sh | tail -2
  if ! cmp -s /tmp/segaddrs.before k27/segaddrs.sh; then
    echo "kernel slots moved: linking the kernel again"
    JOBS="${JOBS:-12}" bash k27/build-kernel27.sh 2>&1 | tail -3
  fi
fi
if [ -e out/kc/kc27.bin ] || [ "${BUILD_NATIVE_KEXTS:-0}" = 1 ]; then
  # xnulinux's own kexts (kexts/*/NAME.json + NAME.cpp), built against the k27 kernel's objects and linked into the collection
  NATIVE=()
  # IONetworkingFamily's kernel headers (IOKit/network/*.h) for XnuEthernet: Apple's open source IONetworkingFamily-188 (APSL,
  # fetched here, not kept in the repository), whose __PRIVATE_SPI__ declarations name the reserved slots 2-5 the shipped family
  # uses (the SDK's headers leave slot 5 a reserved stub the collection does not export)
  NETINC="$POC/build/netinc"; NETSRC="$POC/build/IONetworkingFamily-188"; mkdir -p "$NETINC/IOKit"
  [ -e "$NETSRC/IOEthernetController.h" ] || git clone -q --depth 1 --branch IONetworkingFamily-188 https://github.com/apple-oss-distributions/IONetworkingFamily.git "$NETSRC"
  ln -sfn "$NETSRC" "$NETINC/IOKit/network"
  # IOHIDFamily's kernel headers (IOKit/hid/*.h) for XnuInput and IOStorageFamily's (IOKit/storage/*.h) for XnuDisk: the SDK's
  # Kernel.framework has them
  for h in toolchains/sdks/*.sdk/System/Library/Frameworks/Kernel.framework/Headers/IOKit/hid; do
    [ -e $h/IOHIDDevice.h ] && { ln -sfn $PWD/$h "$NETINC/IOKit/hid"; ln -sfn $PWD/$h/../hidsystem "$NETINC/IOKit/hidsystem"; ln -sfn $PWD/$h/../graphics "$NETINC/IOKit/graphics"; ln -sfn $PWD/$h/../storage "$NETINC/IOKit/storage"
      ln -sfn $PWD/${h%%/System/*}/usr/include/TargetConditionals.h "$NETINC/TargetConditionals.h"
      ln -sfn $PWD/$h/../../HIDDriverKit "$NETINC/HIDDriverKit"; break; }	# IOHIDDevice.h includes HIDDriverKit/IOHIDDevice.h
  done
  for j in "$POC"/kexts/*/*.json; do
    [ -e "$j" ] || continue
    n=$(basename "$j" .json)
    # a kext that does not build is left out of the collection (reported), not the whole rebuild
    python3 scripts/build-native-kext.py $n "$(dirname "$j")/$n.cpp" -I "$NETINC" --obj ${KEXT_OBJ:-k27-obj} || { echo "kext $n failed to build: left out"; continue; }
    NATIVE+=("$j")
  done
  if [ -e out/kc/kc27.bin ]; then
    printf '%s\n' "${NATIVE[@]}" > out/k27/natives.txt	# the kexts of ours in this collection, for scripts/apple-kc/package.sh
  fi
  if [ -e out/kc/kc27.bin ]; then
    # the collection (scripts/kclink): the kernel, xnulinux's kexts, and the Apple kexts of scripts/apple-kc/keep.txt from
    # macOS 27.2's kernelcache
    t=$SECONDS
    python3 "$POC"/scripts/kclink/kclink.py out/kc/kc27.bin out/k27/kernel-k27.macho out/k27/kc-k27.bin \
      --keep $(grep -v '^#' "$POC"/scripts/apple-kc/keep.txt) --native "${NATIVE[@]}" \
      --rebind $(grep -v '^#' "$POC"/scripts/apple-kc/rebind.txt) > out/k27/kclink.log 2>&1 ||
      { tail -5 out/k27/kclink.log; echo "kclink failed: out/k27/kclink.log"; exit 1; }
    grep -E '^wrote|vtable check' out/k27/kclink.log; echo "collection: $((SECONDS - t)) s"
    # AMFI of the collection's macOS expects the code-signature platform version of that build; the system disk's binaries
    # carry their own (macOS 27.0.1: 26 where 27.2's AMFI expects 27)
    python3 "$POC"/scripts/kc-amfi-platform.py out/k27/kc-k27.bin ${AMFI_PLATFORM:-26}
    sudo cp out/k27/kc-k27.bin /lib/firmware/xnu/kc.bin
  fi
fi
# natives.txt describes kexts to package with an Apple kernelcache. The free
# collection consumes the generated object manifests directly instead.
if [ ! -e out/kc/kc27.bin ]; then rm -f out/k27/natives.txt; fi
ls -la out/k27/kernel-k27.macho
sudo cp out/k27/kernel-k27.macho /lib/firmware/xnu/kernel.macho
sync	# the VM may be stopped from the host right after (tart/krunkit stop): unsynced pages of the collection would be lost
