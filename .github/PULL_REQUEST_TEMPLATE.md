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
- [ ] The x86-64 QEMU TCG → ARM64 Linux → PureDarwin console route is identified as the active portability target; the macOS/QEMU/KVM experiment is labeled separately.
- [ ] Every related agent has visible scope, interface, and result updates in PR comments.
- [ ] Required automated checks are green; merge or auto-merge has explicit user authorization.
