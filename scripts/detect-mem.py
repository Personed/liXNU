#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Detect a safe Linux mem= and fki.ko carve-out layout from /proc/iomem."""

import argparse
import json
import re
import sys

MIB = 1024 * 1024
GIB = 1024 * MIB
ALIGN_2M = 2 * MIB


def memory_headroom(total_bytes):
    """Reserve space at the top of RAM for EFI, firmware, and the initrd."""
    if total_bytes >= 12 * GIB:
        return 1024 * MIB
    if total_bytes >= 6 * GIB:
        return 512 * MIB
    return 256 * MIB


def parse_mem_size(value):
    """Parse Linux mem= sizes using the kernel's binary K/M/G/T suffixes."""
    match = re.fullmatch(r"([0-9]+)([KMGTP]?)", value.strip(), re.IGNORECASE)
    if not match:
        raise ValueError(f"invalid LINUX_MEM value {value!r} (expected e.g. 7G or 7168M)")
    amount = int(match.group(1))
    suffix = match.group(2).upper()
    power = {"": 0, "K": 1, "M": 2, "G": 3, "T": 4, "P": 5}[suffix]
    size = amount * (1024 ** power)
    if size <= 0:
        raise ValueError("LINUX_MEM must be greater than zero")
    return size


def validate_layout(ranges, linux_mem, carve_base, carve_mb, ram_mb=None):
    """Reject a final host memory layout that cannot safely reserve the FKI carve-out."""
    if not ranges:
        raise ValueError("no System RAM entries found in iomem")

    linux_bytes = parse_mem_size(linux_mem)
    try:
        base = int(carve_base, 0)
    except ValueError as exc:
        raise ValueError(f"invalid CARVE_BASE value {carve_base!r} (expected e.g. 0x200000000)") from exc
    try:
        size_mb = int(carve_mb, 10)
    except ValueError as exc:
        raise ValueError(f"invalid CARVE_MB value {carve_mb!r} (expected a positive integer)") from exc

    if base < 0:
        raise ValueError("CARVE_BASE must be a nonnegative physical address")
    if base % ALIGN_2M:
        raise ValueError("CARVE_BASE must be aligned to 2 MiB")
    if size_mb < 1024 or size_mb % 2:
        raise ValueError("CARVE_MB must be an even number of at least 1024 MiB")

    carve_end = base + size_mb * MIB
    total_bytes = sum(end - start for start, end in ranges)
    headroom = memory_headroom(total_bytes)
    containing = next(((start, end) for start, end in ranges if start <= base and carve_end <= end), None)
    if containing is None:
        raise ValueError(
            f"carve-out [{base:#x}, {carve_end:#x}) is not contained in one System RAM range"
        )
    start, end = containing
    usable_end = align_down(end - headroom, ALIGN_2M)
    if base < align_up(start, ALIGN_2M) or carve_end > usable_end:
        raise ValueError(
            f"carve-out [{base:#x}, {carve_end:#x}) overlaps the reserved RAM headroom; "
            "choose another CARVE_BASE or reduce CARVE_MB"
        )

    linux_ram_below_carve = sum(max(0, min(region_end, base) - region_start) for region_start, region_end in ranges)
    if linux_bytes > linux_ram_below_carve:
        raise ValueError(
            f"LINUX_MEM={linux_mem} needs {linux_bytes // MIB} MiB below CARVE_BASE, "
            f"but only {linux_ram_below_carve // MIB} MiB of System RAM is below it; "
            "lower LINUX_MEM or move CARVE_BASE higher"
        )

    if ram_mb is not None:
        try:
            requested_ram_mb = int(ram_mb, 10)
        except ValueError as exc:
            raise ValueError(f"invalid RAM_MB value {ram_mb!r} (expected a positive integer)") from exc
        overhead_mb = 256 if size_mb >= 4096 else 128
        max_ram_mb = min(7900, size_mb - overhead_mb)
        if requested_ram_mb <= 0 or requested_ram_mb > max_ram_mb:
            raise ValueError(
                f"RAM_MB={requested_ram_mb} exceeds the supported limit of {max_ram_mb} MiB "
                f"for CARVE_MB={size_mb}"
            )


def parse_iomem(text):
    """Return sorted, merged list of (start, end_exclusive) System RAM ranges."""
    ranges = []
    masked = 0
    pat = re.compile(r"^([0-9a-fA-F]+)-([0-9a-fA-F]+)\s*:\s*System RAM\s*$")
    for line in text.splitlines():
        m = pat.match(line)
        if not m:
            continue
        start_s, end_s = m.group(1), m.group(2)
        start = int(start_s, 16)
        end = int(end_s, 16) + 1
        if start == 0 and int(end_s, 16) == 0:
            masked += 1
            continue
        if end > start:
            ranges.append((start, end))
    if not ranges and masked:
        raise ValueError("System RAM addresses in /proc/iomem are masked (run with sudo)")
    ranges.sort()
    merged = []
    for start, end in ranges:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def align_up(val, align):
    return (val + align - 1) // align * align


def align_down(val, align):
    return val // align * align


def compute_layout(ranges):
    """Compute (linux_mem_str, linux_mb, carve_base, carve_mb, ram_mb, total_mb)."""
    if not ranges:
        raise ValueError("no System RAM entries found in iomem")

    total_bytes = sum(end - start for start, end in ranges)
    total_mb = total_bytes // MIB
    if total_mb < 3584:
        raise ValueError(
            f"need at least 4 GiB of System RAM to reserve memory for XNU (found {total_mb} MiB)"
        )

    # Pick the largest contiguous System RAM block (preferring higher addresses on ties)
    # so the carve-out fits in one contiguous physical range.
    target_idx = max(range(len(ranges)), key=lambda i: (ranges[i][1] - ranges[i][0], i))
    t_start, t_end = ranges[target_idx]
    before_bytes = sum(ranges[i][1] - ranges[i][0] for i in range(target_idx))

    # Leave headroom at the top of the target block for EFI/firmware/initrd regions.
    t_size = t_end - t_start
    headroom = memory_headroom(total_bytes)

    if t_size <= headroom + 1024 * MIB:
        raise ValueError(
            f"largest contiguous System RAM block ({t_size // MIB} MiB) is too small"
        )

    usable_end = align_down(t_end - headroom, ALIGN_2M)

    # Choose desired XNU carve-out size and Linux memory size.
    # For >= 15 GiB total RAM (e.g. 16 GiB, 24 GiB, 32 GiB), cap carve_mb at 8156 MiB
    # (matching the 8 GiB XNU layout with 7900 MiB guest RAM) and give Linux the rest.
    if total_bytes >= 15 * GIB:
        desired_carve_mb = 8156
        min_linux_mb = 4096
    elif total_bytes >= 7 * GIB:
        desired_carve_mb = 4096
        min_linux_mb = 3072
    else:
        desired_carve_mb = 2048
        min_linux_mb = 1536

    # Maximum carve-out that still leaves min_linux_mb across regions [0..target_idx].
    max_carve_in_target = (usable_end - align_up(t_start, ALIGN_2M)) // MIB
    avail_for_carve = (before_bytes + (usable_end - t_start)) // MIB - min_linux_mb
    carve_mb = min(desired_carve_mb, max_carve_in_target, avail_for_carve)
    carve_mb = (carve_mb // 2) * 2
    if carve_mb < 1024:
        raise ValueError(
            f"cannot carve out at least 1024 MiB while keeping {min_linux_mb} MiB for Linux"
        )

    carve_bytes = carve_mb * MIB
    # Place carve-out immediately below usable_end, or at a 1 GiB-aligned boundary for Linux mem=.
    raw_carve_base = align_down(usable_end - carve_bytes, ALIGN_2M)
    linux_bytes_in_target = raw_carve_base - t_start
    linux_total_mb = (before_bytes + linux_bytes_in_target) // MIB

    # Round linux_total_mb down to a whole GiB if >= 3 GiB and it doesn't shrink carve-out excessively,
    # keeping clean mem=XG values (e.g. mem=7G on 16 GiB -> carve_base=0x200000000, carve_mb=8156).
    if linux_total_mb >= 3072:
        linux_gb = linux_total_mb // 1024
        linux_mb = linux_gb * 1024
        linux_mem_str = f"{linux_gb}G"
    else:
        linux_mb = (linux_total_mb // 2) * 2
        linux_mem_str = f"{linux_mb}M"

    # The whole-GiB rounding can put Linux's memory limit below the total size of
    # earlier regions. In that case Linux stops before this region, so start the
    # carve-out at its beginning instead of applying a negative offset here.
    linux_bytes_before_target = before_bytes
    linux_bytes_in_target = max(0, linux_mb * MIB - linux_bytes_before_target)
    carve_base = align_up(t_start + linux_bytes_in_target, ALIGN_2M)

    if carve_base + carve_bytes > usable_end:
        # Adjust carve_mb to fit cleanly up to usable_end.
        carve_mb = ((usable_end - carve_base) // ALIGN_2M) * 2
        if carve_mb < 1024:
            raise ValueError("insufficient contiguous RAM after aligning Linux mem=")

    # Leave ~256 MiB inside the carve-out for XNU kernel/collection overhead (matching 7900 in 8156).
    overhead_mb = 256 if carve_mb >= 4096 else 128
    ram_mb = min(7900, carve_mb - overhead_mb)

    return {
        "linux_mem": linux_mem_str,
        "linux_mb": linux_mb,
        "carve_base": hex(carve_base),
        "carve_mb": carve_mb,
        "ram_mb": ram_mb,
        "total_mb": total_mb,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iomem", default="/proc/iomem", help="Path to iomem file")
    p.add_argument("--json", action="store_true", help="Output JSON instead of shell vars")
    p.add_argument("--validate", action="store_true", help="Validate an explicit final memory layout")
    p.add_argument("--linux-mem", help="Linux mem= limit to validate, e.g. 7G")
    p.add_argument("--carve-base", help="FKI carve-out base to validate, e.g. 0x200000000")
    p.add_argument("--carve-mb", help="FKI carve-out size in MiB to validate")
    p.add_argument("--ram-mb", help="Optional explicit XNU RAM size in MiB to validate")
    args = p.parse_args()

    try:
        with open(args.iomem, "r", encoding="utf-8") as f:
            ranges = parse_iomem(f.read())
        if args.validate:
            if not args.linux_mem or not args.carve_base or not args.carve_mb:
                p.error("--validate requires --linux-mem, --carve-base, and --carve-mb")
            validate_layout(ranges, args.linux_mem, args.carve_base, args.carve_mb, args.ram_mb)
            print("detect-mem: memory layout is valid")
            return
        layout = compute_layout(ranges)
    except (OSError, ValueError) as exc:
        print(f"detect-mem: {exc}", file=sys.stderr)
        sys.exit(1)

    if args.json:
        print(json.dumps(layout, indent=2))
    else:
        print(f"LINUX_MEM={layout['linux_mem']}")
        print(f"CARVE_BASE={layout['carve_base']}")
        print(f"CARVE_MB={layout['carve_mb']}")
        print(f"RAM_MB={layout['ram_mb']}")
        print(f"TOTAL_MB={layout['total_mb']}")


if __name__ == "__main__":
    main()
