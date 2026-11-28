# x86_64 synchronous-exception ownership model

This is a validation-only design oracle for a future production foreign-execution boundary. It is not an IDT implementation, XNU integration, runtime enablement, or real exception ownership. It does not use or change the existing transaction machine.

The model in `fki-ko/fki_x86_64_exception_ownership.h` is per CPU and uses abstract equality cookies for CPU, Linux task, original IDTR image, and controlled stack ownership. Every accepted transition must occur on the same CPU, in the same Linux task, with the same original-IDTR cookie and the same controlled-stack cookie. A mismatch is a consistency failure.

The forward order is exactly: arm preallocated control state, install the boundary IDT shadow, enter foreign/shim execution, then either exit normally, classify a synchronous fault, or abort. A first vector-13/14 fault is recoverable in the model; accepting its recovery is exactly-once and returns to foreign execution. Any other synchronous vector, including double fault, and any nested synchronous fault while a fault is pending is fatal. Fatal means terminal failure/abort only; the model does not claim recovery from it.

Teardown and partial rollback are reverse-order and exactly-once. The original IDT image must be restored before control state is cleared. A second restore or clear is invalid. Partial setup aborts use the same reverse-order rule: control-only setup clears control directly; IDT-armed setup must restore the IDT image before clearing control. Normal completion ends with outcome `COMPLETE`; fault/setup/teardown abort ends `ABORTED`. Completion or abort is proven only after both resources are released.

NMI (vector 2) and external interrupt vectors are unsupported inputs to this model and are rejected. They are documented invariants only: a production design must independently prove how NMI/IRQ state is preserved before foreign execution. This slice creates no executable or modeled NMI/IRQ transition.

Relation to the transaction oracle: the existing FKI transaction machine remains the outer boundary lifecycle oracle. This ownership model is a separate resource/vector-ownership guard and neither duplicates nor extends that machine. A future integration may use an ownership failure as evidence for the existing terminal abort shape, but no such integration is implemented here.
