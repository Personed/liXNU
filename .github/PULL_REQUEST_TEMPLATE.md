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
- [ ] The active Intel/AMD PC compatibility route is QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin; direct hosted XNU on compatible ARM64 Linux remains a separate host profile.
- [ ] Native x86_64/Tahoe and QEMU/KVM macOS results are labeled as separate experiments and are not used as acceptance evidence for the TCG PC route.
- [ ] Every related agent has visible scope, interface, and result updates in PR comments.
- [ ] Required automated checks are green before merge; the owner authorized merging without waiting for human review.
