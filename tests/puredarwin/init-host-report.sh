#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Write host facts before artifact downloads or Python setup can fail.
set -euo pipefail

report="$RUNNER_TEMP/puredarwin-tcg-boot-report.json"
clean() { printf '%s' "$1" | tr -cd '[:alnum:].,:_ /()+-'; }
is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

if [[ "${RUNNER_OS:-Linux}" == "macOS" ]]; then
	system=Darwin
	release=$(sw_vers -productVersion 2>/dev/null || uname -r)
	architecture=$(uname -m)
	processor=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -p)
	logical_cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo null)
	memory_total=$(sysctl -n hw.memsize 2>/dev/null || echo null)
	page_size=$(sysctl -n hw.pagesize 2>/dev/null || echo 4096)
	if is_uint "$page_size"; then
		memory_available=$(vm_stat 2>/dev/null | awk -v page_size="$page_size" '
			/Pages free:|Pages inactive:|Pages speculative:|Pages purgeable:/ {
				gsub(/[^0-9]/, "", $NF); pages += $NF
			}
			END { printf "%.0f\n", pages * page_size }
		')
	else
		memory_available=null
	fi
else
	system=$(uname -s)
	release=$(uname -r)
	architecture=$(uname -m)
	processor=$(awk -F: '/^model name[[:space:]]*:/ { sub(/^[[:space:]]+/, "", $2); print $2; exit }' /proc/cpuinfo)
	logical_cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo null)
	memory_total=$(awk '/^MemTotal:/ { printf "%.0f\n", $2 * 1024; exit }' /proc/meminfo)
	memory_available=$(awk '/^MemAvailable:/ { printf "%.0f\n", $2 * 1024; exit }' /proc/meminfo)
fi

system=$(clean "$system")
release=$(clean "$release")
architecture=$(clean "$architecture")
processor=$(clean "${processor:-unknown}")
runner_label=$(clean "${XNULINUX_RUNNER_LABEL:-unknown}")
is_uint "$logical_cpus" || logical_cpus=null
is_uint "$memory_total" || memory_total=null
is_uint "$memory_available" || memory_available=null

total_min=21474836480
available_min=4294967296
capacity_passed=true
capacity_reasons=
if [[ "$memory_total" == null ]] || (( memory_total < total_min )); then
	capacity_passed=false
	capacity_reasons='"host physical RAM is below 20 GiB (16 GiB guest plus 4 GiB host headroom)"'
fi
if [[ "$memory_available" == null ]] || (( memory_available < available_min )); then
	capacity_passed=false
	if [[ -n "$capacity_reasons" ]]; then capacity_reasons+=,; fi
	capacity_reasons+='"available RAM before QEMU is below the 4 GiB host-headroom allowance"'
fi

cat > "$report" <<EOF
{
  "runner_label": "$runner_label",
  "host": {
    "system": "$system",
    "release": "$release",
    "architecture": "$architecture",
    "processor": "$processor",
    "logical_cpus": $logical_cpus,
    "memory_total_bytes": $memory_total,
    "memory_available_before_artifact_preparation_bytes": $memory_available
  },
  "qemu_version": "not checked",
  "profile": "ARM64 Linux, 4 KiB pages, PAC at EL1, fixed 16 GiB QEMU guest-memory setting",
  "inputs": {"kernel_sha256": null, "initramfs_sha256": null, "disk_sha256": null},
  "manifests": {
    "build": {"path": "build-manifest.txt", "sha256": null, "contents": null},
    "guest": {"path": "guest-manifest.txt", "sha256": null, "contents": null}
  },
  "preparation_steps": {"status": "native host preflight before artifact preparation; boot has not started"},
  "ram_capacity": {
    "minimum_total_ram_bytes": $total_min,
    "minimum_available_before_qemu_bytes": $available_min,
    "capacity_screen_passed": $capacity_passed,
    "reasons": [$capacity_reasons]
  },
  "result": {
    "passed": false,
    "returncode": null,
    "missing_markers": [],
    "error": "The Python boot runner did not complete; see workflow step outcomes and logs."
  }
}
EOF
