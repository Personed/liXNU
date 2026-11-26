# Live USB: final release milestone

**Status:** Proposed roadmap, not a bootable release or a hardware support claim.

The final deliverable is a hybrid bootable ISO with **Try xnulinux** as the default entry. A user writes it to a USB drive, boots it directly from firmware, and gets a usable live session without installing xnulinux or changing the computer's internal drives.

## Release sequence

1. Stabilize the supported console boot and publish a repeatable QEMU bundle. Keep the emulated ARM64/PureDarwin result distinct from the native hosted-XNU x86-64 target.
2. Validate a daily-use session on the intended runtime: graphical login and desktop, display and input, networking, sound, removable storage, and clean shutdown. Record working drivers and limitations. Test GPU acceleration independently of software rendering.
3. Package the validated runtime as the Live USB. Start with x86-64 UEFI for the PC edition. Evaluate legacy BIOS, Secure Boot, and ARM64 as separate, explicitly tested targets. If the ISO boots a minimal Linux host that then launches XNU, document that architecture. An emulated rescue mode may be offered but does not satisfy native hosted-XNU acceptance.

## Live session contract

- Use the same runtime and hardware paths as the corresponding supported installed edition. Include the desktop, supported drivers, and basic apps/diagnostics on the USB, without requiring a network download to start a session. Hardware-dependent features must be reported as working, limited, or unavailable.
- Boot from a read-only base image with a temporary RAM-backed writable layer. Do not automatically mount internal disks or use them for swap, logs, persistence, or crash dumps. Any installer or persistent storage must be a separate, explicit choice; the live entry never starts installation.
- Run from USB by default, reading system files as needed so the complete image need not fit in RAM. Offer a separate copy-to-RAM entry only if validated and enough memory is available.
- Bundle only redistributable components. Do not ship proprietary Apple system images, binaries, firmware, or credentials. Identify the open components included and describe any separately supplied prerequisites without conflating them with offline Live USB support.

## Release acceptance

- Build the ISO reproducibly from pinned inputs and publish the SHA-256 and build manifest. Test the image written to a physical USB drive, not just attached as a virtual optical disc.
- Boot on distinct physical Intel and AMD PCs within the advertised compatibility matrix. Record firmware, RAM, GPU, network hardware, runtime mode, and test outcome. Validate the lowest RAM profile separately; do not derive it from VM settings alone.
- On each supported configuration, open the desktop, run a bundled app, and test display/input, network, audio, removable storage, and shutdown. Test offline startup, plus any advertised optional modes, independently.
- Compare a disposable internal disk image before and after an automated live session and confirm no change. On physical machines, also verify internal disks are not mounted and no swap is activated on them.
- Publish minimum requirements, known limitations, and fallback modes only to the extent backed by tests. A USB prompt, Linux-only boot, or PureDarwin console alone is not a passing desktop Live USB test.

See [portability validation](portability-validation.md) for the distinction between boot evidence and hardware support.
