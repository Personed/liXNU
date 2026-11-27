# x86_64 controlled exception recovery design

This is a disposable-VM design for the next test-only boundary gate. It does not implement XNU entry, transfer Linux interrupt ownership to XNU, or change the production FKI/XNU ABI. `fki_caps()` and the normal x86_64 XNU probe must remain fail-closed until this mechanism is implemented and validated.

## Requirement

Before foreign kernel execution, a selected vCPU must survive one controlled page fault (`#PF`) and one controlled general-protection fault (`#GP`) raised inside the test-owned shim, one at a time. For each case the test must record the vector, faulting RIP, error code, CR2 for `#PF`, CPU identity, and recovery count; resume exactly once at the documented recovery point; then pass the existing host-state, syscall, unload, and shutdown checks. The userspace verifier must stay pinned to that same vCPU from before the ioctl through its post-recovery syscall check; a syscall on a different vCPU does not satisfy this gate. A nested or unclassified `#PF`/`#GP`, or any other synchronous exception covered by a terminal gate, is a failed test even if the disposable VM remains alive. NMI is explicitly outside this test's acceptance claim.

NMI is intentionally not claimed by this design. NMI can arrive outside the controlled instruction window, uses different completion semantics, and interacts with Linux's NMI-safe entry code and watchdogs. It remains a separate blocker.

## Why the exception table alone is not enough

Linux already has a recoverable-fault mechanism through the exception table. In the kernels relevant to this tree, both the kernel-mode page-fault path and `#GP` path first call `fixup_exception()` for faults in kernel text. That mechanism can redirect execution to a fixup target and is appropriate for ordinary Linux helper code.

It is not the full contract oracle: the fixup target is not given a structured copy of the vector, error code, faulting RIP, or CR2. Separate faulting instructions can identify the intended vector only indirectly. That may be useful as a preliminary implementation reference, but it must not be presented as exact exception-metadata validation.

## Test-owned shadow IDT design

The full test mechanism is deliberately confined to one pinned online vCPU in a disposable VM. Before entering the per-CPU path, allocate and validate the per-vCPU control/capture state, shadow IDT storage, a fault stack, and a separate recovery stack with canaries. No allocation is allowed after arming.

1. The probe enters from the existing `work_on_cpu` path with preemption disabled and local IRQs disabled. It verifies the CPU identity and saves the current IDTR plus the host-state snapshot already required by the boundary probe. In assembly, capture the original envelope RSP and callee-saved GPRs in the per-vCPU control block while still on the Linux task stack.
2. Before loading the shadow IDT, switch RSP to the aligned top of the preallocated test-owned fault stack and validate its bounds and canaries. Keep this fault-stack RSP distinct from the saved envelope RSP.
3. Copy the complete current IDT into test-owned memory. Install recovery gates for `#PF` and `#GP` with IST=0 so their frames land on the fault stack. Install no-return terminal gates for every synchronous exception vector 0–31 except `#NMI` (vector 2), `#PF`, and `#GP`. Make those gates present ring-0 interrupt gates with Linux's kernel code selector and preserve each original IST index; in particular, require and retain the original nonzero `#DF` IST index for its emergency stack. Leave the NMI and external-interrupt descriptors byte-identical to Linux's table. Maskable interrupts remain disabled.
4. The recovery handlers are small assembly stubs in the test module. They never call Linux C code before state is captured, never allocate memory, never acquire locks, and never re-enable interrupts. Terminal stubs identify their vector, write a failure code to the disposable VM's `isa-debug-exit` port, then enter a `cli`/`hlt` loop without returning.
5. Load the shadow IDT only on the selected vCPU, execute one injection, and restore the original IDTR before any C-level result processing. Serialize setup/restore so partial setup cannot persist after a handled fault. Only one injection mode is armed at a time; `#PF`/`#GP` handlers require an exact armed vector and RIP, and any mismatch is terminal.

Other CPUs keep Linux's original IDT. This is still not a production mechanism: replacing even a per-CPU copy of the IDT is a disposable-VM test operation and must never be used by the normal x86_64 module path.

## Handler and recovery contract

In 64-bit mode the architectural exception frame includes SS, RSP, RFLAGS, CS and RIP, with the `#PF`/`#GP` error code pushed after RIP; this is also true for a same-CPL0 exception. The recovery gates use IST=0, and the setup has explicitly switched RSP to the test-owned fault stack before installing the shadow IDT. The raw recovery-handler entry RSP points at the error-code slot; the error code is not part of the frame consumed by `iretq`. Each recovery handler must:

- save the general-purpose registers it needs in per-vCPU test storage before touching them;
- record the vector selected by the independent handler entry point;
- record the error code and hardware frame fields exactly as delivered;
- read CR2 for `#PF` before any instruction that can take another fault, while preserving the fault-time R11 before using it as a CR2 temporary and restoring it before `iretq`;
- restore every general-purpose register it used before `iretq`, so the recovery path starts with the fault-time register state, and compare the captured full GPR image against the exact expected per-injection image before reporting success (including registers used to form injection operands);
- rewrite the frame exactly once: set frame RIP to the test recovery stub, set frame RSP to the test-owned recovery-stack top, keep CS/RFLAGS/SS unchanged, and account for the error-code slot before executing `iretq`; and
- increment no counter and perform no C call in the raw handler beyond the capture writes.

The stack switch is therefore explicit: `iretq` first consumes the five-slot frame left by the exception on the fault stack, then loads the rewritten frame-RSP value. Recovery begins on the test-owned recovery stack with fault-time GPRs restored. The exact recovery point must execute once; a second caught `#PF` or `#GP` for the same armed case is a nested-fault failure and takes the terminal path. Other synchronous vectors use the no-return terminal gates described below.

Before arming, the test control block records the armed vector/mode, exact injection RIP, post-injection recovery target, original envelope RSP and callee-saved GPRs, the expected full GPR image for this injection, fault-window RSP, original IDTR image, and both stack canaries. Capture the envelope RSP while still on the Linux task stack; then switch to the fault stack, validate it, and record the fault-window RSP immediately before the labeled injection. No push, pop, call, or other explicit RSP-changing instruction is allowed in the straight-line injection window. If the injection instruction returns normally, assembly must immediately restore and verify the original IDTR, restore the envelope RSP/GPRs, and return a distinct not-run configuration failure to C; it must never reach a pass marker. After entry on the recovery stack, the recovery stub remains assembly and follows this exact sequence:

1. store any scratch GPRs it needs in a dedicated recovery save area before using them;
2. validate the arm token, vector, faulting RIP, one-shot recovery count, and recovery-stack canaries; any mismatch takes the terminal path below;
3. copy the already captured architectural record into the ordinary result buffer and disarm the injection with a single test-state write;
4. restore the original IDTR from the saved image, read it back with `sidt`, compare base and limit, and record the proof bits for exactly-one recovery and byte-identical IDTR restoration;
5. restore the scratch GPRs and the saved callee-saved GPRs, load the original envelope RSP, verify the restored RSP/GPR values, and jump to the normal post-injection continuation on the Linux task stack.

Only then does the shim use the existing normal/software-abort envelope. Because the original envelope RSP and GPRs are restored before that jump, the shim's ordinary return path remains defined. No C result processing, logging, locking, allocation, or Linux helper runs between the recovery-stack switch and that handoff. The first C-level work after the shim returns is the existing after-snapshot/host-state comparison; the proof bits recorded in assembly are inputs to that comparison, not a substitute for it.

An unexpected `#PF`/`#GP` at a recovery gate outside the exact expected instruction is unclassified. A nested/mismatched `#PF`/`#GP`, failed IDTR readback, corrupted stack canary, second recovery entry, or a fault in any other synchronous exception vector except NMI takes a no-return terminal path. For the other vectors, the shadow IDT's terminal gate records its vector and writes a failure code to the disposable VM's explicit QEMU `isa-debug-exit` port, then enters a `cli`/`hlt` loop. Preserve the Linux IST index for `#DF` and other IST-backed terminal gates; require a nonzero `#DF` IST before installing the shadow IDT. Terminal handlers do not parse or return through an unknown frame and never call Linux C. A fault inside a terminal handler may cause QEMU to reset or triple-fault; the harness must treat any unexpected QEMU exit, panic, missing completion marker, or timeout as a failed run. The VM harness must include the debug-exit device, and no terminal failure may be converted into a pass or retried transparently.

The NMI descriptor remains byte-identical to Linux's gate and NMI is not injected or claimed by this test; NMI handling is still a separate blocker before production XNU entry. External interrupts also retain Linux's descriptors and stay masked for the controlled window.

## Controlled injections

The first `#PF` case should use a deliberately selected unmapped canonical kernel address, preferably a fixed low address such as zero in the disposable kernel image. The test verifies that CR2 equals the selected address and that the error code is architecturally consistent with the attempted access. It must not depend on Linux keeping address zero unmapped in every configuration; the implementation probes the environment and reports a not-run configuration error rather than weakening the check.

The `#GP` case must use `mov %ax, %ds` with selector `0xfff8` (TI=0, RPL=0), whose GDT byte offset is `0xfff8`. On the selected CPU, execute `sgdt` before arming and fail closed unless `GDTR.limit < 0xfff8`; this proves the selector index is outside the GDT without depending on LDT contents. The expected selector error code is exactly `0xfff8` (EXT=IDT=TI=0; its low three bits are zero because the selector RPL is zero). The faulting RIP is the exact `mov` instruction. Since the instruction faults before loading DS, DS must equal its pre-injection selector after recovery; if the instruction unexpectedly completes, the assembly not-run path must restore the saved DS selector before returning to C and the case must fail.

Both injection instructions must be out-of-line, individually labeled, and reachable only in the armed test mode. Compiler-generated memory accesses in the injection window are not accepted as controlled injections.

## Rollback and validation gates

The original IDTR is restored before preemption or interrupts are re-enabled. The after snapshot must match the existing boundary baseline for CPU/task identity, stack bounds, RSP, relevant RFLAGS, CR0/CR3/CR4, EFER, FS/GS/KERNEL_GS, XCR0, GDTR/IDTR, LDTR/TR, DS selector, and preemption/IRQ state. In particular, IDTR base and limit after the test must equal the values captured before shadow-IDT installation.

A pass requires all of the following for both `#PF` and `#GP`:

- exact vector, RIP, error code, CPU, and recovery count metadata (`#GP` error code `0xfff8`);
- CR2 equality and access-type consistency for `#PF`;
- exact restoration of the expected full GPR image, including fault-time R11 across CR2 capture, and unchanged DS selector;
- exactly one recovery entry and no nested/unclassified exception;
- normal Linux syscalls after return on the same vCPU as the recovered injection;
- byte-for-byte restoration of IDTR state before cleanup;
- successful module unload and guest shutdown in the disposable VM.

Failure of any element blocks XNU entry work. Passing these tests is still only host-boundary evidence: it does not assign Linux's production exception ownership to XNU, validate XNU page tables, or prove that foreign kernel exceptions can be serviced safely.
