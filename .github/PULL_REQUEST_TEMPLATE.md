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
- [ ] The x86_64 target is the native FKI/XNU port for Tahoe 26 on Linux; QEMU/KVM and TCG results are labeled as complementary experiments, not native-port acceptance.
- [ ] Every related agent has visible scope, interface, and result updates in PR comments.
- [ ] Each applicable automated review finding has a reply in its thread with the fix and validation, or an evidence-based reason it is duplicate, superseded, or inapplicable.
- [ ] Required automated checks are green before merge; the owner authorized merging without waiting for human review.
