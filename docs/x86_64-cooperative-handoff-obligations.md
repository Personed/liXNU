# x86_64 cooperative handoff proof obligations

## Status and scope

This is a validation-only proof-obligation table for the cooperative patched handoff category compared in `docs/x86_64-entry-architecture-decision.md`. It answers only which x86 architectural differences prevent direct transfer of the ARM64 hosted-XNU analogy and which obligations would have to be satisfied before any executable design could even be proposed.

It does not choose the cooperative route, prove viability or production safety, authorize an XNU patch, define an ABI/handoff contract, or authorize entry. `XNU_PREBOOT_LONG` remains rejected/unmodeled in ABI v1, and `_start+0x5c` remains only a structural anchor rather than an authorized entry point.

The helper in `fki-ko/fki_x86_64_cooperative_handoff_obligations.h` exposes the same table to userspace fixtures. No item is accepted or resolved.

Class meanings are deliberately negative: structural mismatch means the ARM64 mechanism cannot be transferred because the x86 resource model differs; unresolved stronger protocol means no complete production-safe protocol is known; required but undesigned means a future design must satisfy the obligation before implementation; forbidden as current claim means the corresponding simultaneous-ownership claim is invalid for current unmodified components.

## Evidence boundary

The ARM64 implementation is useful analogy but architecturally limited evidence. [`xnu-ko/world.S`](../xnu-ko/world.S) switches the ARM64 user half (`TTBR0`/TCR) while Linux retains its exception entry context: `TPIDR_EL1` remains Linux-owned, XNU runs on the Linux task stack, Linux vectors can receive faults, and the host upcall path restores Linux keys/preemption/IRQ state. x86 has no direct equivalent of the ARM64 split between user translation root (`TTBR0`) and kernel translation root (`TTBR1`).

Relevant x86 evidence is structural, not proof of a design:

- Linux v6.8 `Documentation/arch/x86/x86_64/mm.rst` documents kernel-space virtual memory as shared between all processes and gives one combined kernel/user address-space model per CR3. With KPTI, `arch/x86/entry/entry_64.S` enters syscall code with `swapgs`, stores the user stack in TSS scratch state, switches to the kernel CR3, and then loads the Linux per-CPU top of stack.
- Pinned XNU source `xnu-12377.121.6` programs syscall MSRs in `osfmk/i386/mp_desc.c`: SYSENTER MSRs, EFER.SCE, `MSR_IA32_LSTAR`, `MSR_IA32_STAR`, and `MSR_IA32_FMASK` (the syscall flags mask, also called SFMASK in Linux/AMD-oriented naming).
- Pinned XNU `osfmk/x86_64/idt64.s` enters `idt64_syscall` with `swapgs`, uses `%gs`-relative per-CPU shadow fields, switches to an XNU per-CPU exception stack, and later dispatches Mach/UNIX/machdep/diagnostic syscall classes. Its common dispatch and return paths use `%gs:CPU_KERNEL_CR3`, `%gs:CPU_TASK_CR3`, PCIDs, `invpcid`, debug registers, interrupt stacks, and CR3 reloads.
- The same pinned XNU source has LAPIC TPR access in `osfmk/i386/lapic_native.c` and XCR0/xsetbv plus AVX/AVX512 state flavors in `osfmk/i386/fpu.h`, `proc_reg.h`, and `pcb.c`.

These observations are not claims about a patched implementation. They show why treating the ARM64 world switch as evidence that x86 cooperative execution is straightforward would be unsound.

## Proof-obligation table

| Obligation | Classification | Why the ARM64 analogy is insufficient |
| --- | --- | --- |
| CR3/KPTI/PCID address-space split | structural mismatch | ARM64 can switch the user half while the kernel half remains separately rooted. x86 CR3 covers user and kernel mappings together; KPTI/PCID add Linux-specific trampoline and full-kernel CR3 rules. An XNU-controlled pmap cannot be presumed to preserve Linux entry mappings, PCID semantics, or kernel text/direct-map availability. |
| GS/GS_BASE/KERNEL_GS_BASE/`swapgs` versus XNU cpu_data/percpu | structural mismatch | Both kernels use GS as a core CPU-local mechanism, but with incompatible layouts. Linux syscall and exception entries depend on `swapgs` and Linux percpu state; pinned XNU uses `%gs` cpu_data/shadow fields for stack, thread, CR3 and PCID state. A fault taken while XNU's GS is active cannot be presumed safe for Linux entry. |
| STAR/LSTAR/SFMASK syscall MSRs | forbidden as current claim | The MSRs name single architectural entry targets. Linux already owns them for Linux syscall entry, while pinned XNU programs them for XNU entry. Claiming current simultaneous unmodified ownership is invalid; mediation or a future patch/contract would be required. |
| CR0/CR4/CR8/EFER control-state mediation | unresolved stronger protocol | XNU and Linux depend on overlapping control-state bits and interrupt/syscall behavior. CR0/CR4/EFER cannot be freely divergent on one logical CPU, and CR8/TPR priority behavior cannot be presumed independent of Linux interrupt ownership. No production-safe mediation protocol exists. |
| LAPIC/TPR/IPI coexistence | required but undesigned | XNU contains LAPIC/TPR logic; Linux owns LAPIC interrupt delivery, IPIs, and IRQ routing in the cooperative category. A future design would have to virtualize, mediate, or remove XNU's direct assumptions without confusing Linux interrupt state. |
| TSS/IST/exception/syscall stacks | structural mismatch | Linux syscall entry uses TSS scratch and per-CPU stack state; pinned XNU uses its own per-CPU exception and kernel stacks, including IST-like/double-fault handling. One active TSS/IST environment cannot transparently encode both kernels' assumptions. |
| XSAVE/AVX/FPU state | required but undesigned | XNU has AVX/AVX512 state flavors and writes XCR0 through xsetbv paths; Linux owns XSAVE feature policy, context switching, signal frames, and task state. A future contract would need complete ownership, format, feature-mask, and context-switch mediation. |
| Physical memory reservation | required but undesigned | XNU expects physical memory/page ownership semantics; Linux owns the allocator and memory map. The cooperative category needs a bounded reservation and coexistence model rather than XNU ownership of host memory. |
| pmap/address-space integration | unresolved stronger protocol | XNU pmap code manages CR3/PCID/task address spaces, while Linux process CR3s are required for safe Linux entry under KPTI. There is no known production-safe composition of these two pmap models. |
| Exception entry forwarding | required but undesigned | ARM64 forwards selected faults from Linux vectors into XNU context. x86 cannot copy that design while GS, CR3, stack, IDT, IST, debug state, and KPTI entry invariants conflict. A new forwarding/abort contract would be required. |
| Interrupts and preemption coexistence | unresolved stronger protocol | Linux ownership of IRQs, preemption, scheduler state and LAPIC delivery must be preserved while foreign code runs. XNU interrupt/priority assumptions remain unmediated. NMI, IRQ and machine-check remain unsupported invariants in existing models. |
| Rollback and cleanup | required but undesigned | Any future handoff would need reverse-order exactly-once cleanup of memory, control state, MSRs, GS, stacks, interrupt state, FPU state and task/CPU identity. No such x86 protocol exists. |

## Consequences

The table supports only these narrow conclusions:

1. The ARM64 cooperative implementation is analogical evidence for a category, not transferable x86 design evidence.
2. The largest immediate x86 structural blockers are the combined CR3/KPTI/PCID model and the GS/`swapgs` cpu-local-state conflict.
3. Syscall MSRs cannot presently be claimed as shared or concurrently owned by Linux and unmodified XNU.
4. Even if the execution-injection gap is avoided, cooperative execution remains unresolved across memory, control state, exceptions, interrupts, preemption, syscalls, FPU state and rollback.

This table references, but does not alter or integrate with, the transaction machine, exception/syscall ownership models, control/memory matrix, CPU-quiescence record, logical-offline obligations, or entry-architecture decision matrix. It does not authorize runtime, XNU execution, a loader, patch, entry, hotplug/CPU operation, real MSR/IDT access, userland execution, ABI/layout changes, transaction changes, `fki_caps`, VM harness changes, real ownership, or `#216` work.
