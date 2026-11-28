# x86-64 FKI context-state contract draft

This document is the proposed architecture-specific register and transaction contract for the native x86-64 FKI/XNU port. It is a design boundary for implementation work, not evidence that the boundary is implemented or safe.

The contract exists because the ARM64 `fki.h` interface cannot be copied mechanically to x86-64. ARM64 hosting relies on exception-level separation, TTBR state, PSTATE, PAC keys, and Linux's ARM64 exception model. The x86-64 port instead has to account for long-mode versus protected-mode startup, segmentation state, control registers, descriptor tables, PCIDs, XSAVE state, and the fact that the architectural XNU entry protocol begins in 32-bit protected mode. `docs/x86_64-entry-abi-source-pin.md` records that source-level entry protocol; this document defines how a host-owned CPU context must describe, validate, enter, stop, and restore such a foreign context without exposing an unimplemented capability.

No part of this document authorizes setting an x86-64 `FKI_CAP_*` bit or entering XNU. While exception/memory ownership remains unresolved, x86-64 `fki_caps()` must remain zero and XNU module loading must remain fail-closed.

## Design goals

The contract must be able to:

1. snapshot enough host state to prove that Linux survives entry, normal return, fault recovery, shutdown, and abort paths;
2. describe the exact architectural state that will be active when foreign x86-64 code starts or resumes;
3. reject invalid or unsupported state before any foreign instruction executes;
4. preserve enough foreign and host state at every stop to diagnose and resume or abort deterministically;
5. support Intel and AMD without pretending that their feature sets are interchangeable;
6. separate experiment-only synthetic entry from any future authentic XNU entry profile.

The contract must not:

1. treat a Mach-O `LC_UNIXTHREAD` register record as the XNU kernel entry protocol;
2. expose host kernel pointers or Linux-specific structures to the foreign module;
3. require userspace ABI layout decisions before the native kernel-entry boundary exists;
4. claim that XSAVE state can be ignored because early synthetic tests happen not to use AVX;
5. imply that QEMU/KVM or QEMU TCG results validate the native x86-64 module path.

## Execution profiles

There are three distinct profiles. They are not interchangeable and must never share acceptance evidence.

### `HOST_BOUND_SHIM`

The CPU remains in Linux long mode with Linux-owned paging, descriptor tables, interrupt state, and control registers. The shim may save host state, call a bounded synthetic function, and restore host state. This is the only profile supported by the current boundary-probe work.

This profile validates the host-preservation oracle. It is not authentic XNU execution and must never be used as native-port acceptance.

### `XNU_PREBOOT_LONG`

The host or loader performs the machine setup normally done by XNU's protected-mode bootstrap, constructs the long-mode state expected at a precisely identified later XNU entry point, and enters that anchor directly. This profile avoids a live long-mode to protected-mode transition, but it is only valid if the skipped bootstrap work is enumerated and reproduced exactly for the selected artifact/source configuration.

This profile still gives the foreign kernel architectural control over GDT/IDT, page tables, exceptions, syscalls, MSRs, and possibly PCIDs. It cannot be enabled until the exception-forwarding and memory-ownership design below is implemented and validated.

### `XNU_COLD_BOOT`

The host recreates the source-pinned protected-mode environment and transfers control to XNU's architectural `_start`/`pstart` entry. This is artifact-faithful but much more invasive: the CPU must leave the host's normal long-mode execution environment, and Linux cannot execute on that CPU while paging and host control state are unavailable.

This profile is reserved for research only. It requires CPU quiescence, exact control-register and descriptor-state ownership, an abort/re-entry path, physical-address layout validation, and a demonstrated way to recover the CPU on faults, NMI, machine check, and unexpected halt or shutdown. No production acceptance may use it until those mechanisms exist and are validated.

## Versioning and shape

The x86-64 extension must carry an explicit ABI version and fixed-size control block version independently of the architecture-neutral FKI version. A caller supplies the highest version it understands; the host rejects zero, unknown, truncated, or misaligned state. Fields added in later versions must be appended without changing the meaning of earlier fields, and the host must reject nonzero bytes in reserved space.

All integer fields use fixed-width little-endian x86-64 layout. No Linux structure, host pointer, file descriptor, or architecture-neutral ARM64 state crosses the interface. Opaque host handles remain opaque integers or pointers meaningful only to the host implementation.

A state object is never implicitly valid because it was copied from a register dump. It becomes usable only after validation succeeds and the host has taken its own restoration snapshot.

## Register state

The initial fixed control block must represent the following state explicitly. Names are descriptive; exact C layout is implementation work after this contract is accepted.

| Group | Required state |
| --- | --- |
| transaction metadata | ABI version, state size, profile, entry/resume operation, flags, stop mask, feature bitmask, reserved fields |
| GPRs | RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, R8-R15, RIP |
| flags | RFLAGS, including fixed bit 1, interrupt flag policy, IOPL policy, and reserved-bit validation |
| selectors | CS, SS, DS, ES, FS, GS, TR, LDTR, with mode-appropriate validation |
| segment bases | FS base, GS base, KERNEL_GS base |
| control state | CR0, CR2 for stop reporting, CR3 including PCID low bits, CR4, EFER, XCR0 |
| descriptor state | GDTR base/limit, IDTR base/limit |
| XSAVE state | out-of-line standard-format XSAVE area pointer, byte length, feature mask, and explicit compaction/supervisor-state policy |
| boot payload | abstract boot-argument physical or linear address plus size/identity information when the profile uses the XNU boot protocol |
| stop/reporting | stop reason, vector, error code, CR2, instruction pointer, stack pointer, and machine-readable auxiliary data |

The exact representation of CR3 must distinguish the page-table root physical address from PCID low bits. The implementation must reject a nonzero PCID when PCIDE is disabled or unsupported, reserved control-register bits, non-canonical long-mode addresses, invalid fixed RFLAGS bits, malformed XSAVE metadata, and combinations inconsistent with the selected profile.

### Version-1 validation-only control block

`fki-ko/fki_x86_64_context.h` fixes the first binary control block at 512 bytes, with ABI version 1 and control-block version 1. Static assertions pin the offsets of the register, control-state, XSAVE, stop-reporting, and reserved groups. The accompanying userspace fixtures in `tests/x86_64/test-fki-x86-context.c` exercise acceptance and rejection without loading either kernel module.

Version 1 accepts only the `HOST_BOUND_SHIM` profile with `ENTER` or `RESUME`; `XNU_PREBOOT_LONG` and `XNU_COLD_BOOT` are rejected as unsupported. `RESUME` receives the same structural architectural/memory validation as `ENTER`, including zero `stop_*` input fields. The validator does not prove that a resumable continuation exists or is fresh, owned by the caller, in `RESUME_READY`, tied to the expected task/CPU/generation/request ID, or consumed exactly once; those are transaction-oracle and caller responsibilities outside structural control-block validation. Accepting `RESUME` here is not runtime enablement. The profile describes a four-level-paging Linux long-mode host boundary: LA57, LAM, memory-encryption CR3 bits, interrupts, trap flag, direction flag, IOPL, virtual-8086 mode, nested task state, alignment-check control, and virtual interrupt flags are outside this profile. XSAVE version 1 accepts only the standard format and the x87/SSE/AVX feature subset; later XCR0 features require an explicitly designed extension. Boot payload and stop-reporting fields are output/future state and must be zero in validation input. Undefined metadata flags, feature bits, stop-mask bits, and reserved bytes must also be zero.

For `HOST_BOUND_SHIM` version 1, `rsp` is normatively the 16-byte-aligned pre-call stack pointer consumed by the host entry trampoline. It is not the architectural RSP at the first byte of `rip`: after the trampoline's call-like transfer, the target observes `rsp - 8` and the usual callee-entry alignment. The validator's 16-byte alignment requirement is therefore a deliberate custom entry ABI rule, not a claim that x86 architectural RSP must always be 16-byte aligned. The transaction-private stack must include at least the return-address slot below the supplied pre-call `rsp`. A future profile that describes raw architectural state at the first byte of `rip` needs an explicit versioned contract rather than reinterpreting this field.

Passing this validator means only that the fixed state object is internally well formed for the limited profile. It does not arm a transaction, prove equality with live host registers, establish ownership of the referenced GDT/IDT/XSAVE/stack/CR3 memory, or permit foreign execution. A future transaction implementation must perform those live-state, ownership, pinning, restoration, and abort checks before using the object.

### Validation-only memory ownership model

`fki-ko/fki_x86_64_memory.h` defines an abstract memory-reference validation model outside the fixed control-block ABI. Its userspace fixtures in `tests/x86_64/test-fki-x86-memory.c` classify the existing control-block references without changing the version-1 layout. An extent records optional linear start, mandatory contiguous physical backing, byte length, ownership class, use class, access class, and reserved fields. The model validates the CR3 root page, GDT/IDT memory, XSAVE memory, pre-call stack memory, and executable text memory referenced by a `HOST_BOUND_SHIM` control block.

Version 1 recognizes these abstract ownership classes: host-owned memory, transaction-private memory, foreign-owned memory, and explicitly shared memory. Foreign-owned and shared memory are rejected as unsupported in this version. Access policy is separate from ownership: CR3 page-table memory is host-owned and must be readable/writable because hardware page walks can update accessed/dirty bits even though foreign software must not write page-table entries directly; descriptor tables are host-owned and read-only; executable text is host-owned, readable/executable, and never writable; XSAVE and stack memory are transaction-private and writable. All linear extents must carry contiguous physical backing information, and the low 12 bits of linear and physical starts must match so the described backing can be the same byte-for-byte translation. Linear overlaps, physical backing overlaps, and aliases are rejected at page granularity even if the aliased byte ranges are distinct. The validator also requires every control reference to be contained in an extent of the correct use class.

This model does not pin pages, walk translations, reserve memory, or prove that the caller actually owns the extents. It is a contract and fixture layer only. Real kernel ownership, translation walking, pinning/refcounting, reclamation exclusion, and lifetime validation remain required before any transaction may be armed. The contiguous-backing rule is deliberately conservative; multi-page or noncontiguous objects must either be represented as separate extents or wait for a versioned extension.

### XSAVE policy

The XSAVE area is out-of-line and variable length. Version 1 accepts only the architecturally defined standard format; compacted and supervisor-only formats are rejected until their use is designed. The recorded `xstate_bv` must be a subset of XCR0, XCR0 must be supported by the host CPU, and unsupported extended features must not be enabled merely because a hypervisor or CPUID leaf exposes them.

The host must snapshot and restore enough floating-point/vector state even in `HOST_BOUND_SHIM`; otherwise a synthetic call that uses SSE/AVX can silently corrupt Linux or another task. The boundary probe's XCR0 and sentinel checks are the host-side oracle, not a substitute for transaction state validation.

Extended-XCR0 support is deliberately deferred rather than inferred from the disposable-VM fixture. The boundary probe's projection to x87/SSE/AVX is only a test-only way to keep the version-1 validator inside its declared envelope on hosts that also enable MPX, PKU, AVX512, or later features; it is not a production transaction design. Before any real transaction or XNU profile, the implementation must choose and validate either a versioned XSAVE extension that explicitly enumerates the additional components or a host-managed opaque full-xstate save/restore mechanism with defined ownership, sizing, alignment, and feature-matrix rules. Until that decision is implemented, extended-XCR0 hosts remain outside production validation even when the synthetic fixture passes.

## Transaction lifecycle

`fki-ko/fki_x86_64_transaction.h` defines a validation-only model of the lifecycle below. Its userspace fixtures use synthetic events for setup failure, normal return, resume, timeout/cancellation, and abort. The model does not execute an entry, arm CPU state, or implement the runtime transaction.

The disposable-VM boundary probe now carries a narrowly scoped integration fixture for the one-shot entry shape. For each existing synthetic normal-return or software-abort ioctl, the test module snapshots live host control state, constructs a `HOST_BOUND_SHIM`/`ENTER` control object backed by probe-owned XSAVE and alternate-stack memory, validates that object with the fixed-layout helper, and drives the synthetic transaction events around the already existing assembly call. A successful normal return must reach `COMPLETE`; a successful controlled software abort must reach `ABORTED`. The existing Linux host-state comparison and stack/XSAVE canaries remain the preservation oracle. Because control-block version 1 supports only x87/SSE/AVX, the fixture validates that XCR0 projection while the oracle still compares the complete live XCR0.

This fixture is still not a transaction implementation. Its RIP/RSP/XSAVE references describe probe-owned synthetic state, not proof of equality with every transient live register or a pinned production entry frame. It does not populate or restore a production XSAVE image, establish memory ownership, use `fki_caps`, enter XNU, own production exceptions, or cover resume, fault, or shutdown paths.

The transaction states are:

`INIT -> VALIDATED -> ARMED -> FOREIGN -> STOPPED -> RESTORED`

For a transaction that makes a blocking request, the restored Linux task may
wait and then resume the saved foreign continuation:

`RESTORED -> WAITING -> RESUME_READY -> ARMED -> FOREIGN`

`RESTORED -> COMPLETE` is the terminal success path when no request remains.
`ABORTED` is terminal and is reachable from `ARMED`, `FOREIGN`, `STOPPED`,
`RESTORED`, `WAITING`, or `RESUME_READY`.

1. `INIT`: the caller fills architectural state and metadata.
2. `VALIDATED`: the host checks version, shape, alignment, profile, feature support, control-state consistency, memory references, and stop-mask policy. No foreign state is loaded.
3. `ARMED`: the host snapshots its own complete restoration state and prepares the entry trampoline. The CPU is pinned and preemption/interrupt ownership is explicit.
4. `FOREIGN`: control is in the foreign or synthetic context. Linux does not run normally on that CPU in this state.
5. `STOPPED`: control returned to the host trampoline, foreign architectural state was captured, and the stop reason is stable.
6. `RESTORED`: host control, descriptor, paging, interrupt, FPU/vector, and task state were restored and compared against the host oracle. A blocking request may enter `WAITING` only after execution is back on the owning Linux task's normal stack and Linux can schedule or sleep safely.
7. `WAITING`: Linux is restored and the owner has published the wait state for one identified request before making that request visible to a responder. The owner may be about to sleep or already asleep; a response may arrive before the first sleep. No foreign state is loaded. The continuation slot remains reserved for this task and transaction generation; other tasks may run on the CPU but may not reuse that slot or its stack.
8. `RESUME_READY`: exactly one valid reply was accepted for the current transaction generation and request ID. The complete reply payload is published before this state becomes visible, and the owner is woken if it is asleep. After observing this state, the owner reads and validates the reply payload before re-entry. The host then rechecks task/CPU ownership, stack bounds, continuation validity, and captures a fresh Linux restoration snapshot.
9. `COMPLETE`: the transaction returned to Linux, the final host-state oracle passed, and no continuation or request remains live.
10. `ABORTED`: restoration failed, the stop cannot be represented, the request timed out or was cancelled, or ownership validation failed. The module must fail closed, invalidate the continuation, and prevent reuse.

The normal path is `INIT -> VALIDATED -> ARMED -> FOREIGN -> STOPPED -> RESTORED -> COMPLETE`. For a cooperative blocking stop, the owner restores Linux, records the task, generation, request ID, and reply-buffer identity, then publishes `WAITING` with the required synchronization before enqueueing or otherwise exposing the request to a responder. A responder may reply immediately after that publication. A valid reply permits `WAITING -> RESUME_READY -> ARMED -> FOREIGN`. Re-entry through `ARMED` is a new arm of the saved continuation, not a second consumption of the previous reply. A transaction may have only one outstanding request at a time; each request has a monotonically unique request ID within its transaction generation.

Partial entry is possible. Every setup step must have reverse-order teardown, and a failure after any host state is changed must still pass through the same restoration oracle. Timeout, cancellation, invalid ownership, or failed restoration transitions to `ABORTED`; a caller must not resume an `ABORTED` transaction. Only `WAITING` may accept a reply. The first reply that atomically matches the owner, generation, request ID, and live state claims the reply slot, finishes the payload, and publishes `RESUME_READY`; timeout/cancellation and reply acceptance must be serialized so exactly one wins. Publish `RESUME_READY` only after the payload is complete, with synchronization that lets the owner read the payload after acquiring that state. Duplicate and late replies must be rejected without waking the task or touching the continuation or reply buffer.

## Stop reasons

The machine-readable stop taxonomy must distinguish at least:

- normal cooperative return or host call;
- timer/yield/request to let Linux run;
- user or foreign syscall boundary event;
- page fault or other translation fault;
- synchronous architectural exception;
- external interrupt, NMI, or machine check;
- shutdown, halt, triple-fault equivalent, or unrecoverable foreign state;
- host invariant violation or restoration failure.

A page-fault stop records the vector, error code, CR2, foreign RIP/RSP/RFLAGS, active CR3/PCID, and the translation-ownership class if known. A syscall stop must not be conflated with a Linux syscall merely because the same architectural instruction is used; XNU syscall semantics are a separate contract.

### Version-1 synthetic stop-report shape

`fki-ko/fki_x86_64_context.h` defines a small validation-only stop taxonomy for the fields already present in the fixed v1 control block. The helper `fki_x86_validate_stop_report_v1()` validates terminal output records only. It does not validate an entry input, prove that the captured foreign state is safe or resumable, implement fault/shutdown handling, or change the fixed layout. `HOST_BOUND_SHIM` input validation continues to require every `stop_*` field to be zero for both `ENTER` and `RESUME`. A terminal report may carry `operation = ENTER` or `operation = RESUME` when the same reason taxonomy applies; the operation value records which structural entry operation produced the report and does not authorize or prove a continuation.

The architectural register/control fields of the control block are the in/out foreign-state area: a production stop capture may overwrite them with terminal foreign state such as RFLAGS and active CR3/PCID. Because those values are foreign output rather than host input, they are not rechecked by the entry validator or by the metadata-only stop-report helper. `stop_rip` and `stop_rsp` preserve the instruction/stack pointer of the stop event itself so a later fault-resolution design can distinguish the original stop location from any adjusted continuation state. This normative interpretation does not implement that capture.

`SOFTWARE_ABORT` means a controlled abort after foreign execution has conceptually started, so the foreign stop location must still be captured. Setup or validation failures before entry are not stop reports at all; they remain input/transaction errors and must not be encoded as software-abort foreign state. Likewise, these x86 kernel-boundary values are not the legacy user-context `FKI_STOP_*` values and must not be reinterpreted through that separate interface without an explicit mapping contract.

The defined v1 report reasons and field rules are:

| `stop_reason` | Required report fields | Prohibited fields | Vector/error/CR2 rules | `aux` rules |
| --- | --- | --- | --- | --- |
| `FKI_X86_STOP_NORMAL_RETURN` | nonzero canonical `stop_rip`/`stop_rsp` | `stop_vector`, `stop_error_code`, `stop_cr2`, `stop_aux0`, `stop_aux1` | no architectural vector, error code, or CR2 | both zero |
| `FKI_X86_STOP_SOFTWARE_ABORT` | nonzero canonical `stop_rip`/`stop_rsp` | same as normal return | no architectural vector, error code, or CR2 | both zero |
| `FKI_X86_STOP_PAGE_FAULT` | vector 14, canonical CR2, nonzero canonical `stop_rip`/`stop_rsp` | `stop_aux0`, `stop_aux1` | error code may contain only architectural PF bits P/W/U/R/I/PK/SS plus the SGX bit; CR2 is validated for 48-bit canonicity and may be zero | both zero; translation ownership remains undocumented and must not be invented in v1 |
| `FKI_X86_STOP_GENERAL_PROTECTION` | vector 13, architectural 16-bit selector-form error code, nonzero canonical `stop_rip`/`stop_rsp` | `stop_cr2`, `stop_aux0`, `stop_aux1` | error-code bits above bit 15 are prohibited; CR2 is not meaningful and must be zero | both zero |
| `FKI_X86_STOP_SHUTDOWN` | `stop_aux0` subtype | `stop_vector`, `stop_error_code`, `stop_cr2` | no architectural vector, error code, or CR2 | `stop_aux0` is exactly one of `FKI_X86_STOP_SHUTDOWN_HALT`, `FKI_X86_STOP_SHUTDOWN_TRIPLE_FAULT`, or `FKI_X86_STOP_SHUTDOWN_UNRECOVERABLE`; for controlled halt only, `stop_aux1` may be zero or exactly `FKI_X86_STOP_SHUTDOWN_RESTART_REQUESTED`; all other `stop_aux1` bits are prohibited and `stop_aux1` is zero for triple-fault/unrecoverable reports |

For `NORMAL_RETURN`, `SOFTWARE_ABORT`, `PAGE_FAULT`, and `GENERAL_PROTECTION`, `stop_rip` and `stop_rsp` are mandatory captured values and must be nonzero and canonical in the v1 kernel profile. That is representation validation only; it does not prove that the captured foreign state is safe or resumable. For `SHUTDOWN`, the pair is optional because a halt, triple-fault equivalent, or unrecoverable foreign state may not have a trustworthy capture. If either shutdown field is present, both must be nonzero and canonical; both zero means omitted.

Controlled shutdown is a cooperative terminal exit of the current run. In the validation-only transaction oracle it follows the controlled-stop/restored/finish sequence and ends in `COMPLETE`: validation, snapshot, entry, `COOPERATIVE_STOP`, successful restoration, then finish. `COMPLETE` here means only that the boundary/run terminated in a controlled way; it does not claim physical power-off, physical reboot, logical boot success, or availability of a later run. `FKI_X86_STOP_SHUTDOWN_RESTART_REQUESTED` records the foreign restart request already exposed by the hosted model; it does not implement restart. Triple-fault and unrecoverable shutdown reports are terminal failures. In the validation-only oracle they use the terminal fault-stop shape, successful host restoration, then abort, ending in `ABORTED`; this use of `FAULT_STOP` is a synthetic terminal-failure shape, not recoverable #PF/#GP handling and not a new transaction event. No shutdown report has a continuation and none may be resumed. This taxonomy and mapping do not implement real shutdown, power management, reboot, halt interception, or foreign execution.

All other stop causes in the broader taxonomy—syscall boundaries, yield/host calls, external interrupts, NMI/machine check, generic synchronous exceptions other than #GP, host invariant violations, and detailed translation-fault ownership—remain unspecified in v1 and must not be encoded by repurposing these values or `aux` fields. `stop_mask` remains zero in v1 input. Extending the taxonomy requires an explicit versioned contract decision, not silent use of reserved bits.

## Linux calls that can sleep

The x86_64 branch of `fki_arch_linux_runs()` can switch to a single alternate stack for the current CPU. The common FKI calls include `fki_yield()` and `fki_idle_until()`, which enable preemption, and file and service calls that can block. A per-CPU stack can still hold a suspended callback when another task runs on that CPU. Pinning the task to that CPU does not prevent the second task from running. That stack also lies outside the task stack Linux uses for normal unwinding.

Treat a request that can run Linux code as a cooperative stop. Save the foreign continuation in host-owned transaction storage and restore the Linux execution context, including the saved task stack pointer and its return frame, before running the operation in C. At that point Linux must own GS, paging, exception and syscall entry, and floating-point state. Run a blocking operation only when the restored Linux context can sleep. Balance the stop path's interrupt and preemption masking and use a Linux task frame that the configured kernel unwinder understands. Keep the request data and reply buffer in memory accessible under Linux page tables for the lifetime of the operation.

Tie each pending request to one task, transaction generation, and request ID. A second task on the same CPU uses its own task stack. Reserve any per-CPU foreign context slot and continuation storage that the pending request depends on until the continuation is consumed or invalidated. The reservation must allow Linux scheduling and must survive a blocking call without holding a spinlock. The first implementation should bind the owner task to an online CPU and prevent CPU offline for the transaction.

The request and reply buffers must be host-owned, accessible with Linux's restored page tables, and valid for the full wait and reply lifetime. They must not reside on the per-CPU alternate stack or in storage that another task can overwrite. A timeout or cancellation marks the request and continuation invalid, but does not free buffers still visible to an in-flight responder; release them only after completion or cancellation has been acknowledged. Reject late or duplicate replies by state, generation, and request ID before they can dereference those buffers.

When a reply wakes the owner, verify task/CPU ownership and current stack bounds, capture fresh Linux restoration state, then arm the saved continuation for one resume. Consume that continuation atomically once. Any timeout, cancellation, stale generation, wrong task/CPU, or restoration failure aborts the transaction and leaves the foreign slot unusable until outstanding operations have quiesced.

The first test can use `HOST_BOUND_SHIM` in a disposable Linux VM. Have the shim return an identified request to its Linux task, restore Linux to that task's normal stack, and publish `WAITING` before making the request visible. Include an immediate responder that replies after request publication but before the owner makes its first sleep attempt. Require the owner to observe the completed reply and avoid sleeping, with no lost wakeup; verify the exact expected payload, generation, and request ID. Also include a delayed responder: block the owner with a bounded timeout, deliver the reply while it sleeps, then verify that after waking the owner reads the exact expected payload before re-arming the continuation.

While the original continuation is `WAITING`, run another task on the same CPU and have it attempt to enter the same shim. Require an observed context switch and prove that the second task does not reuse the reserved per-CPU stack or continuation: a single-slot implementation must reject the second entry before touching the reserved storage, while a multi-slot implementation must use a distinct slot and stack. Check the reserved canaries and original continuation before and after the attempt.

Have a same-CPU helper deliver the expected reply to the owner task. Check that the reply handler runs on the requesting task's stack, the request buffers remain valid until the reply is acknowledged, and the corresponding synthetic continuation resumes exactly once. Deliver duplicate and late replies and require rejection without a second wakeup or resume. Separately exercise timeout and cancellation; each must invalidate the continuation, retain buffers until cancellation is acknowledged, and leave stale replies unusable. Apply the existing host-state, syscall, unload, and guest power-off checks to every result.

The disposable-VM task-continuation harness now exercises this fixture shape with the validation-only models linked together. Before the first entry and before the exactly-once resume it builds a HOST_BOUND_SHIM v1 control block from a fresh Linux snapshot, validates the transaction transition, and validates an abstract memory map for the executable test byte, live GDT/IDT references, probe-private XSAVE and stack allocations, and active CR3 root page. The first entry uses `ENTER`; only the rearmed continuation uses `RESUME`, while identity, freshness, ownership, and exactly-once consumption remain enforced by the transaction oracle rather than the structural validator. The probe-private XSAVE and stack backing uses physically contiguous test allocations; host text/table backing is introspected from the active disposable-VM kernel translations only. The harness covers immediate and delayed replies, timeout, cancellation, wrong generation/request IDs, duplicate and late replies, same-CPU contention, host-state restoration, and stack canaries, and it requires the transaction oracle to reach `COMPLETE` exactly through two entries or `ABORTED` after one entry as appropriate.

This integration is still strictly a test oracle. Translation introspection does not pin pages, reserve physical memory, transfer lifetime ownership, exclude reclamation, or establish a production memory-ownership policy. The synthetic continuation never enters XNU or foreign production state, does not alter the v1 control-block layout, and does not cover fault, shutdown, ABI v2, or extended-XCR0 transactions.

## Exception and interrupt ownership

This is the central unresolved x86-64 problem. Linux and XNU both expect architectural control of IDT entries, exceptions, syscalls, interrupt delivery, and low-level CPU state. A contract that only saves and restores registers is insufficient.

Before `XNU_PREBOOT_LONG` or `XNU_COLD_BOOT` may expose any capability, the implementation must define and validate one explicit ownership model:

- which descriptor tables are active while foreign code runs;
- how synchronous exceptions are captured without corrupting Linux;
- how NMI and machine check are handled;
- how external interrupts are held, redirected, or delivered;
- how the CPU returns to a host trampoline before Linux state is needed;
- how nested faults and faults in the trampoline itself are contained;
- how other CPUs are prevented from depending on the borrowed CPU.

If the answer requires XNU cooperation or patches, those patches are part of the trusted boundary and must be tied to the exact source revision/configuration. If safe ownership cannot be implemented without virtualization or a stronger hardware mechanism, the port must say so rather than entering foreign code anyway.

## Memory and paging ownership

Memory ownership is outside the register structure but mandatory for validation. Every mapping referenced by foreign state must be classified before entry:

- host-owned Linux memory that foreign code must not write;
- host-managed backing memory assigned to the foreign kernel;
- foreign-owned page-table memory and the physical address space it describes;
- shared user memory, if a future user-execution design supports it;
- memory-mapped I/O or firmware regions, which version 1 rejects unless explicitly designed.

CR3 validation requires knowing who owns the referenced page tables, whether Linux can reclaim them, whether the foreign kernel may write them, and how TLB invalidation is performed. PCID use requires an invalidation and migration policy. A CPU must not migrate while using foreign CR3/PCID state unless the transaction explicitly supports migration.

The validation-only model in `fki-ko/fki_x86_64_memory.h` is the first fixture layer for this requirement: it checks coherent abstract ownership classes and rejects overlap/aliasing for the existing version-1 references. It is not the real ownership mechanism. Before arming any transaction, kernel code still has to establish actual ownership, pinning or lifetime exclusion, translation accessibility, MMIO/firmware exclusion, and the CR3/PCID invalidation policy for the selected profile.

## Intel and AMD separation

CPUID feature bits are necessary but not sufficient. Validation records vendor, family/model/stepping, required baseline features, PCID/INVPCID support, XSAVE/XCR0 support, NX, paging width, speculative-control MSRs when relevant, and any vendor-specific errata or mitigation constraints. Intel evidence must not be presented as AMD evidence or vice versa.

AMD use additionally requires matching user-provided kernel patches and a separate artifact/configuration audit. The contract does not encode those patches and does not authorize them.

## Validation gates

The implementation must add automated fixtures before exposing a capability:

1. fixed-layout and reserved-byte tests for every versioned structure;
2. invalid-state rejection fixtures for control registers, descriptor state, RFLAGS, XSAVE metadata, PCID, memory-reference ownership/overlap, and inconsistent mode combinations;
3. transaction-state machine tests covering setup failure, normal return, resume, abort, and synthetic terminal stop-report shape;
4. disposable-VM host-preservation tests extending the boundary probe to entry, faults, shutdown, and abort;
5. Intel/AMD feature-matrix tests with explicit unsupported-feature failures;
6. artifact-specific source/configuration binding before any authentic XNU entry profile is enabled.

Passing those tests proves only the tested boundary mechanics. Native hosted-XNU support additionally requires an architecture-matched Tahoe 26 artifact, memory/exception/syscall ownership, complete boot evidence, shutdown evidence, and separate physical Intel and AMD validation.
