# SPDX-License-Identifier: GPL-2.0-only
$ErrorActionPreference = 'Stop'

$originalRunnerTemp = $env:RUNNER_TEMP
$temp = Join-Path ([System.IO.Path]::GetTempPath()) "xnulinux-host-report-test-$PID"
New-Item -ItemType Directory -Path $temp -Force | Out-Null
$env:RUNNER_TEMP = $temp
$env:XNULINUX_RUNNER_LABEL = 'windows-report-test'

try {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $preflightScript = Join-Path $scriptDir 'init-host-report.ps1'
    $runner = Join-Path $scriptDir 'run-tcg-e2e.py'

    & pwsh -NoProfile -File $preflightScript
    $reportPath = Join-Path $temp 'puredarwin-tcg-boot-report.json'
    $preflight = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $preflightBytes = $preflight.host.memory_available_before_artifact_preparation_bytes
    if ($null -eq $preflightBytes -or [int64]$preflightBytes -lt 0) {
        throw 'The Windows preflight did not record numeric available RAM.'
    }

    $inputs = Join-Path $temp 'inputs'
    New-Item -ItemType Directory -Path $inputs | Out-Null
    $kernel = Join-Path $inputs 'Image'
    $initramfs = Join-Path $inputs 'initramfs.cpio.gz'
    $missingDisk = Join-Path $inputs 'pd.img'
    [System.IO.File]::WriteAllBytes($kernel, [System.Text.Encoding]::UTF8.GetBytes('kernel'))
    [System.IO.File]::WriteAllBytes($initramfs, [System.Text.Encoding]::UTF8.GetBytes('initramfs'))
    $buildManifest = Join-Path $inputs 'build-manifest.txt'
    $guestManifest = Join-Path $inputs 'guest-manifest.txt'
    Set-Content -LiteralPath $buildManifest -Value ("puredarwin_disk_sha256=" + ('a' * 64)) -Encoding utf8NoBOM
    Set-Content -LiteralPath $guestManifest -Value @(
        'arm64_guest_kernel_release=test-kernel',
        'arm64_guest_page_size=4096',
        'arm64_guest_pac=available',
        'arm64_guest_exception_level=EL1'
    ) -Encoding utf8NoBOM

    $summaryPath = Join-Path $temp 'summary.md'
    $env:GITHUB_STEP_SUMMARY = $summaryPath
    $serialLog = Join-Path $temp 'serial.log'
    $output = & python $runner $kernel $initramfs $missingDisk $serialLog --qemu 'xnulinux-qemu-missing-for-report-test.exe' 2>&1
    $runnerExitCode = $LASTEXITCODE
    if ($runnerExitCode -ne 1) {
        throw "Expected the missing-disk runner case to fail cleanly with exit code 1; got $runnerExitCode. $output"
    }

    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ([int64]$report.host.memory_available_before_artifact_preparation_bytes -ne [int64]$preflightBytes) {
        throw 'The Python runner did not preserve the Windows preflight RAM value.'
    }
    if ($null -eq $report.host.memory_available_at_runner_start_bytes) {
        throw 'The Python runner did not report its own available RAM sample.'
    }
    $expectedCapacity = ([int64]$report.host.memory_total_bytes -ge 20GB) -and
        ([int64]$report.host.memory_available_at_runner_start_bytes -ge 4GB)
    if ([bool]$report.ram_capacity.capacity_screen_passed -ne $expectedCapacity) {
        throw 'The RAM capacity screen does not match the report sample taken at Python runner start.'
    }
    if ($null -ne $report.host.memory_available_before_qemu_bytes) {
        throw 'The runner reported a pre-QEMU sample even though artifact validation stopped the boot.'
    }
    $errors = $report.result.errors -join ' '
    if ($errors -notmatch 'missing input file' -or $errors -notmatch 'could not be computed') {
        throw "Expected a diagnostic missing-disk report; got: $errors"
    }
} finally {
    if ($null -ne $originalRunnerTemp) {
        $env:RUNNER_TEMP = $originalRunnerTemp
    } else {
        Remove-Item Env:RUNNER_TEMP -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output 'Windows preflight-to-runner report regression passed.'
