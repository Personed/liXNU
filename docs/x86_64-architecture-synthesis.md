# x86_64 architecture synthesis decision record

## Status

This is a validation-only architectural synthesis of the evidence recorded in `docs/x86_64-cpu-quiescence-decision.md`, `docs/x86_64-logical-offline-proof-obligations.md`, `docs/x86_64-entry-architecture-decision.md`, `docs/x86_64-cooperative-handoff-obligations.md`, and `docs/x86_64-address-space-coexistence.md`. It is an evidence-based assessment under current evidence, not an absolute truth, an irrevocable project decision, or an implementation authorization.

No route is accepted or resolved. This document authorizes no executable design, XNU patch, ABI, entry, runtime, ownership, CPU/hotplug operation, real MSR/IDT access, userland execution, transaction change, VM harness change, or `#216` work.

## Decision under current evidence

Under current evidence, **stock-kernel module plus patched cooperative execution does not present a production-safe plausible path**. More narrowly: patching XNU alone is not sufficient.

This does not mean cooperative execution is logically impossible. It means the evidence does not show a production-safe design that can be implemented as only a stock-kernel module plus patches confined to XNU.

## Why the cooperative category remains blocked

The entry-architecture matrix records that cooperative patched execution avoids the physical-CPU execution-injection gap because Linux can retain CPU ownership. That removes one blocker only. The proof-obligation and address-space tables show that the hardware and architectural control path still enters Linux first:

- `STAR`/`LSTAR`/`SFMASK` name the architectural syscall entry used on the CPU. Linux owns those MSRs for Linux syscall entry; XNU also expects to program them. An XNU patch cannot create a second architectural `syscall` entry target or a module-safe interception point in front of Linux.
- Exceptions and interrupts enter through the Linux-owned IDT path. KPTI, CR3 transitions, entry stacks, TSS/IST, and `%gs`/`swapgs` state must already be valid for Linux entry before any module-level C handler or forwarding logic can run.
- x86 CR3 covers user and kernel mappings together, unlike the ARM64 `TTBR0`/`TTBR1` user/kernel split. The address-space evidence shows unresolved layout conflicts and no automatic composition of Linux and XNU page-table ownership.
- Linux and XNU both rely on GS for incompatible CPU-local state. A fault or syscall taken while the wrong GS/CR3 state is live can fail before any cooperative recovery logic gets control.

Patching XNU can remove or rewrite some internal expectations—for example, direct use of GS, CR3, LAPIC, or syscall MSRs—but it cannot by itself create production-safe module-visible interposition for syscall, exception, and context mediation. Those events are architecturally delivered to the Linux-owned entry path first.

## Enabler mapping, without selecting one

| Blocker family | Possible enabler categories |
| --- | --- |
| Syscall entry ownership and routing | kernel cooperation/module-safe hooks; deep XNU/user ABI paravirtualization; hypervisor/virtualization architecture |
| Exception, IDT, KPTI, CR3 and entry-context mediation | kernel cooperation/module-safe hooks; hypervisor/virtualization architecture |
| GS/CPU-local state, LAPIC/TPR/IPI, preemption and interrupt assumptions | deep XNU paravirtualization; hypervisor/virtualization architecture |
| Address-space layout, pmap, physical-memory and PCID/TLB composition | deep XNU paravirtualization plus a future memory/CR3 contract; hypervisor/virtualization architecture |
| Rollback, exactly-once cleanup and host preservation | any enabler still requires a dedicated production-safe design |

The smallest theoretical enabler for continued research is **not implementation**: first obtain or define evidence for a module-safe mediation boundary covering syscall entry, synchronous exception delivery, and enough CPU/context state for Linux entry to remain valid. If no such boundary exists through kernel cooperation, then the remaining research directions are deeper XNU/ABI paravirtualization or a hypervisor/virtualization architecture rather than a stock-kernel-only cooperative module.

This ordering is a research filter, not a recommendation. It does not select kernel cooperation, XNU paravirtualization, or hypervisor/virtualization.

## Consequence for prior categories

- Protected/unmodified reproduction with physical-CPU possession remains blocked/unresolved by the physical CPU ownership and execution-injection evidence.
- INIT/SIPI/manual offline injection remains unresolved and cannot presume module-safe reimplementation.
- Cooperative patched execution avoids physical-CPU possession but, under current evidence, is not production-safe as a stock-kernel module plus XNU-only patch path.

The next technically meaningful question is therefore not another decomposition of the same blockers. It is whether the project can obtain a production-safe mediation boundary through one of the enabler categories above, or whether the x86 goal must be reframed around a different architecture.
