# x86_64 entry-architecture decision matrix

## Status and scope

This is a validation-only decision matrix comparing entry-architecture categories against the execution-injection and ownership evidence recorded in `docs/x86_64-logical-offline-proof-obligations.md`. The helper in `fki-ko/fki_x86_64_entry_architecture_decision.h` exposes the same matrix to userspace fixtures.

No category is accepted. This document is not a protocol, route selection, ABI, loader, patch design, or authorization to execute XNU. It does not alter or integrate with the transaction, exception, syscall, control/memory ownership, CPU-quiescence, or logical-offline tables; it only references them as unresolved inputs.

## Evidence inputs and limits

- The logical-offline table records that Linux `remove_cpu`/`add_cpu`, cpuhp callbacks, hotplug blocking, and `stop_machine` are insufficient for production-safe CPU ownership, and that the logical-offline route lacks a documented production-safe execution-injection point.
- The INIT/SIPI evidence records that Linux uses internal APIC wake hooks or static INIT/SIPI code for application-processor startup, but no module-safe exported implementation was observed in the local kernel evidence. Absence of export is not universal proof of impossibility; kernel cooperation or a future module-safe API could change the evidence. Reimplementing INIT/SIPI in a module remains unauthorized as a current solution.
- The ARM64 hosted-XNU implementation is analogical evidence only. In [`xnu-ko/world.S`](../xnu-ko/world.S), XNU runs on the Linux task stack, `TPIDR_EL1` remains Linux-owned so faults return to Linux with valid per-CPU state, host calls may schedule, and the world switch preserves/restores user state, keys, preemption, and interrupt state. This is ARM64 code: it is not x86 proof, does not validate a long-mode handoff, does not authorize an x86 patch, and does not transfer exception, preemption, or IRQ properties to x86 automatically.

## Matrix

| Architecture category | Result constrained to current evidence | Meaning |
| --- | --- | --- |
| Protected/unmodified reproduction with physical-CPU possession | blocked/unresolved by injection and ownership | Requires possession of a physical CPU and a module-safe injection/ownership mechanism. Current evidence provides neither. Kernel cooperation or a future module-safe API could change this, but does not exist here. |
| INIT/SIPI/manual offline injection | blocked/unresolved by injection and ownership | Linux uses INIT/SIPI internally, but no module-safe exported injection mechanism was observed. Treating module reimplementation as currently authorized is forbidden. APIC/x2APIC ownership, trampoline state, reset/rollback, SMI/NMI/MCE, and re-online remain unresolved. |
| Cooperative patched long-mode handoff under Linux ownership | unresolved, but not blocked by the physical-CPU injection gap | This category avoids physical-CPU possession as a requirement because Linux would retain CPU/control ownership. That is the only gap-specific conclusion. It is not viability, selection, production safety, or implementation authorization. |

## Obligations that remain unresolved for every category

- memory ownership and physical backing;
- control-state ownership or mediation;
- exception and syscall ownership/routing;
- rollback, reverse-order restoration, and cleanup;
- preservation of Linux ownership and host integrity.

For the cooperative patched category specifically, the following additional obligations remain unresolved: reviewed XNU patches, a documented ABI/handoff contract, Linux-owned CPU/control-state mediation, x86 exception/preemption/IRQ behavior, memory ownership, syscall semantics, and rollback. The ARM64 analogy does not satisfy any of those x86 obligations.

## Explicit non-decisions

- No route is accepted or selected.
- `XNU_PREBOOT_LONG` remains rejected/unmodeled in ABI v1 and is not validated here.
- `_start+0x5c` remains only a structural anchor, not an authorized entry point.
- No protected-mode reproduction, patched/direct long-mode handoff, or INIT/SIPI implementation is authorized.
- No runtime, XNU execution, loader, patch, CPU/hotplug operation, APIC/MSR/IDT access, userland, ABI/layout change, transaction change, VM harness change, or `#216` work is authorized.

The maximum supported conclusion is narrow: the first two categories remain blocked/unresolved by missing module-safe injection/ownership evidence, while the cooperative patched category avoids physical-CPU possession as a requirement and therefore is not blocked by that specific gap. It remains completely unresolved and requires a separate reviewed design before any implementation could even be proposed.
