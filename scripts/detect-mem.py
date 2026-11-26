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
    if total_bytes >= 12 * GIB:
        headroom = 1024 * MIB
    elif total_bytes >= 6 * GIB:
        headroom = 512 * MIB
    else:
        headroom = 256 * MIB

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
    args = p.parse_args()

    try:
        with open(args.iomem, "r", encoding="utf-8") as f:
            text = f.read()
        layout = compute_layout(parse_iomem(text))
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
