# Native x86_64 code-path audit

Status on 2026-11-27, after the Intel source build in #187 and the Recovery
audit in #217. This audit covers the experimental native x86_64 hosted-XNU
path for Tahoe 26.7.3.

## Current path and gates

| Layer | Current code | Evidence boundary |
| --- | --- | --- |
| XNU module selection | `xnu-ko/Makefile` links only `xnu_x86_64_stub.o` for `CONFIG_X86_64`; ARM64 still links `xnu_main.o`, `macho.o`, `adt.o`, and `world.o`. | Normal x86 `xnu.ko` cannot call ARM64 loader/world-switch code. |
| XNU module init | `xnu-ko/xnu_x86_64_stub.c` checks the documented CPU feature floor, then returns `-EOPNOTSUPP` even when the features exist. | The feature check does not enable execution. The x86 module load test remains a refusal test in a disposable VM. |
| FKI x86 backend | `fki-ko/fki_x86_64.c` supplies CPU ID, monotonic counter, alternate Linux stacks, and an FPU save boundary. `fki_caps()` returns zero. | There is no x86 implementation of `fki_enter/leave`, PAC swap, address-space mapping, user contexts, or exception delivery. The shared `fki.h` state/capability API still includes ARM concepts; it must not be reinterpreted as an x86 ABI. |
| Mach-O inspection | `xnu-ko/macho_parse_x86_64.c` and its fixtures inspect a bounded synthetic `MH_EXECUTE` subset. The x86 branch in `xnu-ko/macho.c` parses and then returns `-EOPNOTSUPP`. | The parser does not load, relocate, apply fixups, allocate executable pages, or enter XNU. Its object is compiled for validation but is not linked into the normal x86 `xnu.ko`. |
| ARM64 loader and handoff | `xnu-ko/xnu_main.c`, `world.S`, and `world.h` use `_arm_init`, `linux64_abi.h`, PAC, AArch64 system registers, and ARM page-table setup. | These are not an x86 boot path. `tests/x86_64/build-xnu-main.sh` is only a diagnostic compile attempt and is not in the required x86 workflow; its Kbuild invocation also needs an explicit architecture before it can be treated as x86 evidence. |
| Host boundary probe | Merged PR #97 strengthens the disposable Linux-to-Linux call/return probe for normal return and software abort. | It does not switch CR3/GS, route XNU syscalls or faults, validate XNU boot arguments, or enter a kernel image. The #87 contract keeps exception-ownership design and real-artifact ABI pinning as separate gates. |

## Build and collection evidence

The [Intel build in #187](https://github.com/Personed/liXNU/actions/runs/46073430260)
used the verified KDK `26.5/25F71` to build public XNU `xnu-12377.121.6`.
Its [artifact](https://github.com/Personed/liXNU/actions/runs/46073430260/artifacts/15235905935)
contains a 24,730,344 byte `kernel.development` with SHA-256
`77c60cb9be541ce818fa1ece614667df39423117da0cf86cf5428d525de05b9d`.
The matching manifest records 404,791 dependency entries across 1,123 files,
with zero missing. The Apple Silicon job skipped the cross-build without a
matching KDK. The source artifact is evidence for the 26.5 build.

The [Recovery audit in #217](https://github.com/Personed/liXNU/actions/runs/46077603347)
verified the Apple chunklist and inspected the downloaded volume's Intel
`BootKernelExtensions.kc`. Its fileset report has 204 entries. The Recovery
catalog's `latest` selector returned 26.6.2 (`25G83`). The standalone kernel
audit in #185 identified the 26.7.3 (`25G342`) kernel in the KDK. The installed
boot collection for `25G342` still needs inspection.

The Recovery audit uses the pinned OpenCore
[Tahoe recipe](https://github.com/acidanthera/OpenCorePkg/blob/ebea824880706ac9faa8ee45604245de62be658b/Utilities/macrecovery/recovery_urls.txt#L50-L51).
`macrecovery.py` accepts `default` and `latest` as catalog selectors. Neither
pins a build. The separate Intel OTA inventory checks the size and SHA-256
of the Apple CDN ZIP indexed for `25G76 → 25G342` by
[AppleDB](https://github.com/littlebyteorg/appledb/blob/2d59226769714041389efda35daf4b9a938ba8a9/osFiles/macOS/25x%20-%2026.x/25G342.json).
On a machine with a verified local copy, run
`python3 scripts/x86_64/audit-tahoe-ota.py --archive /path/to/intel-ota.zip`.
The JSON lists collection files, binary deltas, BOMs, and payload containers
visible in the ZIP directory. A delta requires the matching base. Files
inside payload containers and the collection selected by an installed System
volume require separate inspection.

The [full installer catalog recorded by #187](https://github.com/Personed/liXNU/actions/runs/46073430260/job/140755502992)
lists `25G342` at 17,957,880 KiB (about 17.1 GiB). That exceeds the 14 GB of
SSD documented for the [standard Intel runner](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
Use a compatible Intel Mac with enough free storage and an installation made
from the [Apple full installer](https://support.apple.com/en-us/102662).

## Safe slice in this change

This change adds a source-pinned x86 `boot_args` v2/revision-0 layout and a
core-field fixture builder based on
`pexpert/pexpert/i386/boot.h` in the pinned XNU commit. It asserts the 4096-byte
structure and key field offsets, validates EFI memory-map descriptor extents,
requires 64-bit EFI mode, and applies the source's range semantics per field.
The memory-map and device-tree bases and lengths are widened before their data
is traversed (`ml_static_ptovirt()` in
`osfmk/i386/AT386/model_dep.c` and `pexpert/i386/pe_init.c`), so their
end-exclusive address may equal 4 GiB: the interval `[0xfffff000, 0x100000000)`
contains only representable 32-bit physical addresses. The kernel interval is
stricter: `osfmk/i386/i386_init.c` first evaluates
`uint32_t lphysfree = kaddr + ksize`, and `osfmk/i386/pmap_common.c` also adds
those fields before widening. XNU then rounds the initial `physfree` up to a
4 KiB page and later uses `ID_MAP_VTOP()` to keep the low 32 bits of the
identity-mapped address. The validator accepts a kernel end at most
`0xfffff000`. A higher end rounds to 4 GiB and loses the address. This bound
does not reserve space for bootstrap allocations.

Synthetic tests cover the final 4 KiB page for the memory map and device
tree, the kernel endpoint at `0xfffff000` and one byte above it, malformed
lengths, unsupported versions/modes, unterminated command lines, and intervals
extending past the 32-bit physical-address space.

The builder initializes only a core subset. EFI system-table, runtime-service,
video, security, and target-specific entry fields remain unset. The helper is
compiled as a validation-only object and tested on the host; it is not included
in the x86 `xnu.ko` object list or called by the ARM64 module. A passing fixture
means only that this source-pinned data layout and range checker behave as
specified. It does not produce bootable arguments or prove the real artifact's
handoff ABI.

## Next step

For a prepared Intel Tahoe installation, mount its System volume read-only.
Record the installer source and snapshot or volume identity locally. From the
liXNU checkout with Python 3.9 or later and a C compiler, pass the mounted
path to the audit. It reads the version plist and the collection from the same
System volume, requires `26.7.3/25G342`, and inspects a private snapshot of the
collection. The temporary directory needs space for that snapshot.

```sh
python3 scripts/x86_64/audit-tahoe-system-volume.py \
  --system-root '/Volumes/Tahoe System' > /tmp/lixnu-tahoe-collection.json
```

Record the liXNU commit and the JSON report with both hashes. The audit exits
zero only if the inspector reports `status=accepted`, `parser_result=0`, and a
nonempty nested kernel segment list. The plist establishes the version reported
by the supplied volume. Record Apple installer or sealed volume provenance in
the acceptance record. Keep the paired files on the operator's machine and
share only the reviewed metadata and hashes. Use this evidence to compare the
25G342 layout against the loader and handoff contract. The normal x86 module retains its
refusal until the loader, context transaction, exception and syscall ownership,
and return paths have their own reviewed evidence.
