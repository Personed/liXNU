# SPDX-License-Identifier: GPL-2.0-only
# Write host facts before artifact downloads or Python setup can fail.
$ErrorActionPreference = 'Stop'
$os = Get-CimInstance Win32_OperatingSystem
$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
$totalBytes = [int64]$os.TotalVisibleMemorySize * 1024
$availableBytes = [int64]$os.FreePhysicalMemory * 1024
$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($architecture -eq 'x64') { $architecture = 'x86_64' }

$minimumTotal = [int64]20 * 1GB
$minimumAvailable = [int64]4 * 1GB
$capacityReasons = [System.Collections.Generic.List[string]]::new()
if ($totalBytes -lt $minimumTotal) {
    $capacityReasons.Add('host physical RAM is below 20 GiB (16 GiB guest plus 4 GiB host headroom)')
}
if ($availableBytes -lt $minimumAvailable) {
    $capacityReasons.Add('available RAM before QEMU is below the 4 GiB host-headroom allowance')
}

$report = [ordered]@{
    runner_label = $env:XNULINUX_RUNNER_LABEL
    host = [ordered]@{
        system = 'Windows'
        release = $os.Version
        architecture = $architecture
        processor = $processor.Name
        logical_cpus = [Environment]::ProcessorCount
        memory_total_bytes = $totalBytes
        memory_available_bytes = $availableBytes
        memory_available_at_report_bytes = $availableBytes
        memory_available_at_report_phase = 'host_preflight_before_artifact_downloads'
    }
    qemu_version = 'not checked'
    profile = 'ARM64 Linux, 4 KiB pages, PAC at EL1, fixed 16 GiB QEMU guest-memory setting'
    inputs = [ordered]@{ kernel_sha256 = $null; initramfs_sha256 = $null; disk_sha256 = $null }
    manifests = [ordered]@{
        build = [ordered]@{ path = 'build-manifest.txt'; sha256 = $null; contents = $null }
        guest = [ordered]@{ path = 'guest-manifest.txt'; sha256 = $null; contents = $null }
    }
    preparation_steps = [ordered]@{ status = 'native host preflight only; boot has not started' }
    ram_capacity = [ordered]@{
        minimum_total_ram_bytes = $minimumTotal
        minimum_available_before_qemu_bytes = $minimumAvailable
        measurement_phase = 'host_preflight_before_artifact_downloads'
        capacity_screen_passed = ($capacityReasons.Count -eq 0)
        reasons = @($capacityReasons.ToArray())
    }
    result = [ordered]@{
        passed = $false
        returncode = $null
        missing_markers = @()
        error = 'The Python boot runner did not complete; see workflow step outcomes and logs.'
    }
}

$path = Join-Path $env:RUNNER_TEMP 'puredarwin-tcg-boot-report.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
