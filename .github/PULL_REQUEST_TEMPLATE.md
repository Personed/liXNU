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
- [ ] The x86_64 macOS target is identified as Tahoe through QEMU/KVM; x86_64 TCG Linux smoke and ARM64 TCG/PureDarwin evidence are labeled as separate complementary profiles.
- [ ] Every related agent has visible scope, interface, and result updates in PR comments.
- [ ] Required automated checks are green before merge; the owner authorized merging without waiting for human review.
