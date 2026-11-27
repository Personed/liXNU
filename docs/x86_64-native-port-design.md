# Native x86_64 hosted-XNU port design

## Project goal and current boundary

The active project goal is to port hosted-XNU to native Linux x86-64 PCs with Intel and AMD CPUs, targeting macOS Tahoe
26.7.3, while preserving the existing ARM64 hosted-XNU path and its macOS 27 target. Acceptance is separate for physical
Intel and AMD hosts; no universal PC support is claimed. QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin and the
QEMU/KVM macOS launcher are complementary development and validation paths, not evidence for the native x86-64
hosted-XNU port.

The x86-64 native port remains experimental and fail-closed: x86_64 `fki_caps() == 0`, and `xnu.ko` returns
`-EOPNOTSUPP`. This document describes required architecture and validation work; it does not authorize XNU entry. The
PR #56 pins the latest publicly published XNU source, 26.5 (`xnu-12377.121.6`); building that source does
not establish compatibility with the Tahoe 26.7.3 runtime target.

AMD is a project target, not a currently supported host profile. The existing x86_64 stub's CPU-profile check rejects
vendors other than `GenuineIntel`; that refusal is only a guard and does not establish Intel runtime support either.
AMD support requires a vendor-aware CPU path and separate physical acceptance after the native entry, host-integrity,
boot, and shutdown gates pass. Until then, AMD hosts remain unsupported by the native x86_64 runtime.

The pinned XNU source includes `osfmk/i386` and `mach-o/x86_64`, but this repository's current machine patches and
loader target ARM64. An x86_64 Tahoe kernel collection and matching entry ABI must be built and checked; the presence
of Intel source files alone does not prove compatibility with the target artifact.

### Parse-only x86_64 Mach-O slice

`xnu-ko/macho_parse_x86_64.c` provides an isolated metadata parser for little-endian x86_64 Mach-O. For `MH_EXECUTE`, it accepts only the pinned XNU subtypes `X86_64_ALL` and `X86_64_H`, validates load-command and segment/section bounds, and requires exactly one `LC_UNIXTHREAD` carrying `x86_THREAD_STATE64` (flavor 4, 42 32-bit words). The entry RIP must point into one file-backed executable segment. The parser exports a bounded plan of at most 24 segment descriptors in Mach-O command order, including VM/file ranges and protections. For `LC_DYLD_CHAINED_FIXUPS`, it validates the version-zero header, ordered starts/imports/symbols regions, import table bounds, and starts-in-image segment table. It accepts dyld's prefix page arrays when `page_count` does not exceed the segment's VM page count. Every simple x86_64 start must leave room for an eight-byte word inside both its page and the segment's file-backed and VM ranges. Starts records use 4K/16K pages and one common supported pointer format per image: `DYLD_CHAINED_PTR_64`, `_64_OFFSET`, `_64_KERNEL_CACHE`, or `_X86_64_KERNEL_CACHE`; nonzero `max_valid_pointer` values must agree across nonempty records. The segment offset is checked against the preferred `__TEXT` load address. For Tahoe dyld's post-link `__CTF` case, this parser accepts only one omitted zero-VM segment immediately before a final `__LINKEDIT`, with the corresponding starts slot empty; it follows the pinned `dyld-1285.19` check and does not require the segment name to be `__CTF`. A file-backed CTF segment is accepted only in the `ctf_insert` layout: zero VM size, `SG_NORELOC`, read-only protections, and one bounded `(__CTF,__ctf)` regular section. The parser validates its file range and section but does not count the section as mapped VM. x86_64 `MULTI/LAST` starts are rejected as unsupported. `symbols_format` must be uncompressed; nonzero imports require a nonempty symbols pool after the header, but individual import name offsets and string contents are not checked. The summary records pointer format, page size, and page count, but the parser never walks pointer chains or applies fixups. Dyld-info command extents are only bounds-checked and reported; other known linkedit data extents are also only bounds-checked. Unsupported required commands are rejected, and unknown optional commands are counted.

The parser also recognizes a bounded `MH_FILESET` metadata subset. It validates the outer header and load-command framing, every `LC_FILESET_ENTRY` command size, and the `entry_id` `lc_str` offset relative to the command start. The offset must be at or after the 32-byte entry structure and strictly inside `cmdsize`; its NUL-terminated string is scanned only to the command boundary. Any gap before the string and the zero padding after it must be zero, with the command ending on its required 8-byte boundary. The parser checks a zero reserved word and that each entry's `fileoff` leaves room for a 64-bit header. It requires exactly one ID equal to `com.apple.kernel`. That entry's `fileoff` must identify an in-file x86_64 `MH_EXECUTE` header and a complete, exactly consumed load-command region; its subtype must be `X86_64_ALL` or `X86_64_H`. The result records outer and nested header metadata, the entry's file offset and unslid `vmaddr`, and command counts/lengths. `vmaddr` is reported as an opaque address: the parser does not infer a relation between it and the file offset.

The fileset fields are grounded in the pinned public XNU [`loader.h`](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/EXTERNAL_HEADERS/mach-o/loader.h). Its pinned [`bootstrap.cpp`](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/libsa/bootstrap.cpp) lists `com.apple.kernel` as a kernel component. XNU's pinned [`kt-dump.cpp`](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/tools/kt-dump/kt-dump.cpp) uses a fileset entry's `fileoff` to inspect the embedded image, while [`dyld_kernel_fixups.h`](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/osfmk/mach/dyld_kernel_fixups.h) uses `vmaddr` as its in-memory header address. Pinned dyld's [`UnsafeHeader.cpp`](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/mach_o/UnsafeHeader.cpp) bounds-checks the string at `command + entry_id.offset`, while its [`MachOFileAbstraction.hpp`](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/common/MachOFileAbstraction.hpp) setter emits the canonical offset `sizeof(fileset_entry_command)` (32). Although `loader.h` describes lc_str strings as stored immediately after the structure, the parser accepts a larger offset only when it is inside `cmdsize` and all intervening/tail padding is zero. This broader value is exposed only as bounded metadata; no target Tahoe artifact proves that such an encoding occurs there. It checks that the nested command table stays within the containing file, but the public entry structure does not provide an independent member length. The nested segment table reports container-relative file ranges and VM intervals with checked bounds. Each segment command must contain exactly its declared section records. Section payloads, ownership within the collection, kernel entry state, fixups, and loadability remain outside this check. The Recovery audit reports parser rejection as a structural observation. Confirm segment extraction with an accepted result and a nonempty kernel segment list in the report.

The parser continues to reject `LC_MAIN`, additional thread-state flavors, unknown x86 subtypes, more than 24 segments, unsupported chained pointer formats/page sizes, `MULTI/LAST`, malformed ranges, and other unsupported `MH_EXECUTE` variants. A successful parse is structural inspection only; callers must not infer that the image is loadable or that its fixups are supported. Synthetic fixtures exercise the production parser API directly, including malformed starts and imports, valid page-prefix metadata, a lower-than-`__TEXT` segment, uniform-format enforcement, the narrow zero-VM segment omission, and malformed/valid fileset metadata with an offset-40 ID. CI also compiles the same parser and `macho.c` as x86_64 ELF objects in a temporary copy. The parser does not allocate or map executable memory, rebase pointers, apply fixups, or enter XNU. `macho_load()` still returns `-EOPNOTSUPP` after a successful fileset parse. [Recovery audit #12](https://github.com/Personed/liXNU/actions/runs/46001678966) accepted the fileset metadata of the Intel Recovery image that reported Tahoe 26.6.2 (25G83) and XNU `12377.161.14~5`. The Apple chunklist verified the image bytes; the HTTP catalog did not authenticate selection for the requested board or the latest version. The Tahoe 26.7.3 (25G342) runtime collection still needs verification, and `xnu.ko` remains a refusal stub.

The fileset JSON reports `chained_fixups` separately for the outer collection
and the embedded kernel. When present, the command must fit within its
header's unique `__LINKEDIT` file and VM ranges. The inspect-only report
includes collection-relative offsets and the offset within `__LINKEDIT`.
The payload bytes, chain targets, slides, and rebases are outside this check.
The pinned XNU source uses the outer header for collection fixups after EFI
has rebased the kernel image ([i386_init.c](https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/osfmk/i386/i386_init.c#L620-L673)).

## Default and validation artifacts

The boundary probe and the prerequisites for the next synthetic one-CPU entry/return test are specified in
[`docs/x86_64-entry-return-contract.md`](x86_64-entry-return-contract.md). That contract is test planning only: it does
not enable XNU execution or change the FKI ABI.

- The normal x86_64 build keeps `fki.ko` at zero execution capabilities and `xnu.ko` fail-closed with `-EOPNOTSUPP`.
- After the context, memory, and fault prerequisites pass, a separate opt-in build target may emit `fki-x86-test.ko` and
  `xnu-x86-test.ko`, compiled with a test-only flag such as `XNULINUX_X86_TEST=1`. These artifacts are not installed or
  autoloaded by the normal package, service, or boot script; the normal `fki.ko` and `xnu.ko` outputs remain unchanged.
- CI loads only the test artifacts into a disposable x86_64 Linux VM with a disposable guest disk. Intel and AMD desktop
  acceptance uses the same test artifact loaded manually on a dedicated test installation with the normal service and
  autoload disabled. These ring-0 modules can panic the host, so the physical test system must be recoverable and contain
  no production workload. Remove the test artifacts after the run; their result is evidence for the tested profile only.
- Enabling native x86_64 execution in the normal artifacts requires a later change after the XNU boot, desktop, and host-
  integrity gates pass. Do not make a test-only flag or module parameter silently turn the normal artifact on.

## Concrete architecture dependencies

| Area | Current ARM64 dependency | Work needed for x86_64 |
| --- | --- | --- |
| XNU loader and ABI | `xnu_main.c` looks up `_arm_init`, `_linux64_hook`, and ARM64 LINUX64 symbols; `linux64_abi.h` uses AArch64 registers and is copied into `osfmk/arm64`. | Define the x86_64 XNU entry point, boot data, and host-call ABI. Use the x86_64 SysV/kernel calling conventions intentionally, with an ABI version and symbol checks. |
| Mach-O and kernel-collection validation | The current loader and linker are built around the ARM64 kernel and collection formats. | Validate expected CPU type, subtype, ABI, and entry symbols before mapping executable data. Add fixtures for truncated headers, wrong-CPU images, malformed load commands, thread states, relocations, and fixups; verify rejection happens before executable allocation. |
| World switch | `world.S` uses AArch64 GPRs, DAIF, TPIDR, TTBR0/TCR, and ELR/SPSR/ERET; `struct xnu_world` has those register layouts. | Add a distinct x86_64 per-CPU save area and entry/return assembly. Specify and test stack, GPR, RFLAGS, FS/GS base, CR3/PCID, FPU/XSAVE, preemption, and interrupt state. Preserve Linux's current task and per-CPU state on every exit path. |
| Privileged host state | `fki_enter/leave` switches ARM pointer-authentication keys and IRQ/preemption state; `xnu_init` currently requires `FKI_CAP_PAC`. | Split generic execution/context capabilities from the optional ARM PAC capability. Do not report PAC on x86 or add a no-op PAC backend. The x86 XNU path must establish its own supported security and context contract before enabling execution. |
| Memory and translation | The ARM64 path allocates a physically contiguous region, shares TTBR1, switches TTBR0/TCR, installs ARM table entries, and flushes the ARM TLB. | Specify the x86_64 kernel/user page-table model, Linux mappings needed by host callbacks, isolation of XNU mappings, physical memory accounting, TLB/PCID invalidation, and kernel/user boundary. |
| Exceptions and faults | FKI probes ARM64 Linux fault handlers and converts FAR/ESR into an XNU fault trampoline that restores registers and returns with ERET. | Design x86 #PF and other exception delivery, fault-address/error-code/register mapping, recoverable XNU faults, fatal return to Linux, and nested-fault behavior. Linux IDT/IRQ ownership must remain valid; no global vector replacement without a proved per-CPU protocol. |
| Boot and CPUs | `boot_args_v2`, device tree, MPIDR slots, ARM CPU entry, and LINUX64 AP hooks are ARM64-specific. | Build x86_64 boot arguments and memory map from the target XNU/i386 interfaces; integrate ACPI/topology, AP startup, APIC/IPIs, timers, and stable Linux-to-XNU CPU mapping. |
| User execution | XNU's ARM64 LINUX64 layer defines EL0, PAC operations, ARM syscall/fault state, and ARM FPU structures. | Port x86_64 XNU user entry/return and syscall/exception handling, per-thread FS/GS state, and the enabled XSAVE state. Do not reuse ARM UTCB register or PAC layouts. Test that hosted-user syscalls reach XNU while Linux processes retain Linux syscall behavior, and verify state preservation on return. |
| XNU source patches | All current `xnu-patches/*.patch` machine-layer changes target `osfmk/arm64`; the build flow prepares an ARM64 kernel and ARM64 collection. | Add a separate x86_64/i386 machine layer and patch set for Tahoe's x86_64 kernel; keep the ARM64 patch application and build unchanged. |

## Implementation order and acceptance gates

The x86 memory stage includes a [Linux bootstrap reservation plan](x86_64-bootstrap-reservation.md) for the pinned entry layout.

1. **Pin the x86 build input.** Select the exact Tahoe-compatible XNU source/configuration, toolchain, and symbol-bearing x86_64 kernel collection. Record the architecture and source revision in CI artifacts. A successful ARM64 build or generic Mach-O parse is not sufficient.
2. **Keep architecture selection explicit.** Build separate ARM64 and x86_64 module object lists and machine-layer patches.
   Make FKI capabilities describe generic context execution separately from PAC and other optional features. Default and
   distributed x86 builds stay fail-closed. A separate test target such as `fki-x86-test.ko`/`xnu-x86-test.ko` uses an
   explicit compile-time test flag, is not packaged or autoloaded, and exposes only capabilities whose prerequisites
   passed. Never make the normal artifacts executable through a test-only change.
3. **Prove host-state entry/return in isolation.** Add a disposable x86_64 test for the assembly boundary before entering XNU. Exercise normal return and abort paths, stack alignment, saved registers/flags, Linux FS/GS and page-table state, IRQ/preemption state, and enabled XSAVE state. Verify the Linux task resumes correctly. Test both Intel and AMD CPU profiles; QEMU/KVM can provide early repeatable checks but not final hardware acceptance.
4. **Prove memory and exception routing.** Add a bounded XNU address-space test and verify page-table isolation, TLB/PCID behavior, Linux host mappings, #PF recovery, and fatal-fault return. Test SMP and CPU hotplug only after the one-CPU case is stable.
5. **Boot XNU with one CPU.** After the isolated context, memory, and exception tests pass, use the separate test artifacts
   to exercise native XNU entry in the disposable CI VM and on dedicated physical test hosts. Supply x86 boot arguments
   and host calls, reach the firmware/console boundary, then a PureDarwin shell and clean halt. Keep default/distributed
   builds fail-closed until the acceptance gates pass. Record each stage separately; reaching the XNU entry is not a
   PureDarwin boot.
6. **Validate the Tahoe desktop on hardware.** Boot a user-provided x86_64 Tahoe disk through the native module path on physical Intel and AMD Linux PCs separately. The first desktop milestone may use software rendering; verify disk I/O, keyboard/pointer input, network, visible display, and clean shutdown. Record CPU model/features, Linux kernel/config, RAM, source and disk hashes, and Linux host-integrity checks. Keep the ARM64 macOS 27 build and runtime smoke as a regression requirement.

Use the [physical acceptance record](x86_64-physical-acceptance.md) to collect evidence for each tested Intel or AMD profile.

A gate passes only with its named evidence. Keep default/distributed x86_64 builds at `fki_caps() == 0` until native XNU
boot, desktop, and host-integrity gates pass. Test artifacts must be explicitly named, built only through the test target,
and must expose only capabilities whose prerequisites have been verified. x86_64 remains in development until all
acceptance gates pass. No Apple macOS image, firmware, or locally supplied SMC key belongs in this repository or its logs.
