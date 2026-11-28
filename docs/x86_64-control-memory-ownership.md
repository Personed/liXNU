# x86_64 control/memory ownership matrix

## Status and scope

This is a validation-only normative matrix for a possible future native x86_64 XNU entry. The helper in `fki-ko/fki_x86_64_control_memory_ownership.h` exposes the same matrix to userspace fixtures. It is not an executor, state machine, loader, ABI, ownership mechanism, or permission to enter foreign code.

The matrix does **not** decide whether CR3 or any other control state can be transferred on a live Linux CPU. It does not choose between protected-mode reproduction and a patched or direct long-mode handoff, does not validate `XNU_PREBOOT_LONG`, and does not reduce the need for a later CPU-quiescence, rollback, memory-ownership, and control-register design. ABI v1 continues to reject the XNU entry profiles.

## Classification meanings

- **host-owned**: Linux retains authoritative control. Foreign access is forbidden unless a later explicit protocol says otherwise.
- **foreign-owned**: XNU would control the contents during foreign execution. Where `HOST_RETAINS_BACKING` is present, Linux still owns the physical backing and must destroy or reclaim the foreign view before releasing it.
- **temporarily-transferred**: host-created content or backing would be lent to the foreign context for a bounded lifetime. This is only a normative requirement for future design; no transfer mechanism exists now.
- **forbidden**: foreign access or reliance is outside the supported profile. Asynchronous NMI, IRQ, and machine-check events are unsupported invariants, not modeled recovery paths.
- **unresolved/requires stronger protocol**: the current evidence is insufficient to decide safe ownership. A later design must answer the resource explicitly before any entry experiment.

`EXACTLY_ONCE_REQUIRED` is only a requirement for a future design. This matrix has no events, transitions, or exactly-once executor.

## Matrix

| Resource or invariant | Classification | Required notes |
| --- | --- | --- |
| CR0 | unresolved/requires stronger protocol | Register transfer, restoration, and rollback side effects are undecided. |
| CR3, including root/PCID interpretation | unresolved/requires stronger protocol | The disposable CR3 probe does not make live-CPU foreign transfer production-safe. |
| CR4 | unresolved/requires stronger protocol | Feature ownership and restoration are undecided. |
| EFER | unresolved/requires stronger protocol | LME/NXE and related restoration are undecided. |
| GDTR | unresolved/requires stronger protocol | XNU expects its own descriptor environment; safe transfer is undecided. |
| IDTR | unresolved/requires stronger protocol | XNU installs boot IDT state; exception ownership is separate and unresolved for production. |
| RFLAGS/interrupt control | unresolved/requires stronger protocol | Incoming interrupt state is not pinned by the source comment and rollback is undecided. |
| Debug state | unresolved/requires stronger protocol | Incoming debug registers are not pinned and must not be invented. |
| Segment selector state, including CS/SS/DS/ES/FS/GS/LDTR/TR | unresolved/requires stronger protocol | Foreign use and restoration are undecided. |
| XCR0/XSAVE ownership | unresolved/requires stronger protocol | Foreign use, host preservation, and rollback are undecided. |
| `MSR_IA32_FS_BASE` | unresolved/requires stronger protocol | Foreign use and restoration are undecided. |
| `MSR_IA32_GS_BASE` | unresolved/requires stronger protocol | `vstart()` writes `EARLY_GSBASE_MAGIC` before the memory guard; rollback must include this side effect. |
| `MSR_IA32_KERNEL_GS_BASE` | unresolved/requires stronger protocol | Linux syscall entry state must be preserved; whether foreign execution may use or replace the register is unresolved. |
| CPU identity | host-owned | The future design must pin the CPU and prevent ownership changes mid-transaction. |
| Linux task identity | host-owned | Native Linux tasks must not become foreign-owned. |
| Physical reservation | temporarily-transferred | Linux retains backing; a future design must bound lifetime, cleanup, and release exactly once. |
| Host memory outside reservation | forbidden | Early XNU mappings must not cover host memory outside the owned reservation. |
| `boot_args` content | temporarily-transferred | Host-created input consumed by XNU; backing remains host-owned. |
| EFI memory-map content | temporarily-transferred | Host-created input advertising only owned memory; backing remains host-owned. |
| ADT content | temporarily-transferred | Host-created input; backing remains host-owned. |
| boot metadata placement | unresolved/requires stronger protocol | It remains undecided whether boot args/EFI/ADT must end before initial `physfree` or only before the first real bootstrap allocation. This [read-only #216 concern](https://github.com/Personed/liXNU/pull/216) is intentionally unresolved. |
| Bootstrap page-table memory | foreign-owned with host backing | Host may construct/validate it, but XNU would control contents during execution; Linux retains physical backing. |
| Bootstrap table rebasing side effect | unresolved/requires stronger protocol | Exactly-once mutation semantics depend on the future entry strategy and rollback design. |
| Low boot stack | foreign-owned with host backing | XNU controls contents during execution; backing remains host-owned. |
| High boot stack alias | foreign-owned with host backing | Same backing/rollback discipline as the low stack. |
| `vstart_idt_init(TRUE)` effect | unresolved/requires stronger protocol | The pinned source installs boot IDT state before the memory guard; this must be represented even though real entry is prohibited. |
| `MSR_IA32_GS_BASE` early write effect | unresolved/requires stronger protocol | The pinned source writes GS base before the memory guard; rollback cannot treat the memory guard as the first side effect. |
| Rollback path and side effects | unresolved/requires stronger protocol | Reverse-order restoration and exactly-once cleanup are future requirements, not implemented behavior. |
| NMI | forbidden/unsupported invariant | No routing or recovery ownership is modeled. |
| External IRQ | forbidden/unsupported invariant | No routing or recovery ownership is modeled. |
| Machine check | forbidden/unsupported invariant | No recovery claim is made. |

## Relation to existing validation-only models

This matrix complements, but does not alter or integrate with:

- the transaction lifecycle oracle, which owns run identity and exactly-once continuation semantics;
- the synchronous exception ownership model, which models synchronous exception ownership and recoverable/fatal classification without owning control registers or memory;
- the syscall ownership/routing model, which classifies future syscall handoff without implementing MSR, syscall-table, or IDT routing.

No model above is duplicated here. A future production design must compose them explicitly instead of treating this matrix as sufficient.

## Explicit non-decisions

- No protected-mode reproduction is selected.
- No patched or direct long-mode handoff is selected.
- `_start+0x5c` remains only a structural anchor, not an authorized entry point.
- `XNU_PREBOOT_LONG` remains rejected and unmodeled in ABI v1.
- No live-CPU CR3/control-state transfer is presumed safe.
- No runtime, loader, patch, XNU execution, real MSR/IDT access, userland, or `#216` reactivation is authorized.

The next architectural work must resolve CPU quiescence, memory/control-register transfer or rejection, and rollback as a reviewed design before any executable entry experiment.
