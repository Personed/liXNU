## Purpose

<!-- State the user outcome and the architecture path this changes. -->

## Related work and coordination

- Related PRs/issues:
- Related PR conversation and review threads read by each participating agent:
- Coordination and result comments posted for other agents, with links:

## Validation

- Commands and results:
- Hardware/artifact acceptance still missing:
- Support claims changed (if any):

## Checklist

- [ ] ARM64 behavior remains covered when shared module code changes.
- [ ] No Apple system images, firmware, or local SMC OSK values are included.
- [ ] The active x86-64 PC target is QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin console; native Tahoe hosting and QEMU/KVM remain separate experiments.
- [ ] Every related agent has visible scope, interface, and result updates in PR comments.
- [ ] Each applicable automated review finding has a reply in its thread with the fix and validation, or an evidence-based reason it is duplicate, superseded, or inapplicable.
- [ ] Do not merge or enable auto-merge without explicit owner authorization for this PR.
