# rk3576-counters.ps1 - register the RK3576 drivers' performance counters
#
# maliwddm.sys ("RK3576 GPU") and rknpu.sys ("RK3576 NPU", "RK3576 NPU
# Memory") are kernel-mode counter providers. Windows shows their counters
# only after lodctr /m has registered the manifest each driver package puts
# next to its .sys in the driver store, and no INF can do that. The driver
# store folder changes with every driver update, so the "RK3576\Performance
# counters" task runs this at every start, as SYSTEM: for each driver whose
# manifest is not the one registered last time (or whose registration is
# gone), it unregisters the old one by provider GUID and registers the new.
# A driver that is not installed, or a package without a manifest, is
# skipped. Besides, an NPU that has no UserAccess value yet gets one
# (rk3576-setup.ps1 -Action NpuAccess, at the end).
#
#   rk3576-counters.ps1           register what changed
#   rk3576-counters.ps1 -Force    register again regardless
#   rk3576-counters.ps1 -Remove   unregister what this script registered
#
# State: HKLM\SOFTWARE\RK3576\Counters (<service> = the manifest
# registered, <service>Guid = its provider GUID). Log:
# %ProgramData%\RK3576\counters.log.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param([switch]$Force, [switch]$Remove)

$ErrorActionPreference = 'Continue'
# Windows 10 on Arm runs this in the x86 powershell.exe, where WOW64 turns
# System32 into SysWOW64: the driver store is then not there and lodctr is
# the x86 one. Look at the files through Sysnative and run the native
# lodctr and unlodctr, which take the real System32 paths.
$Sys = Join-Path $env:SystemRoot 'System32'
$SysSeen = $Sys
if ([IntPtr]::Size -eq 4 -and $env:PROCESSOR_ARCHITEW6432) { $SysSeen = Join-Path $env:SystemRoot 'Sysnative' }
function Seen([string]$p) {
    if ($p -and $p.StartsWith($Sys + '\', [StringComparison]::OrdinalIgnoreCase)) { return $SysSeen + $p.Substring($Sys.Length) }
    return $p
}
$providers = @(
    @{ Service = 'rknpu';    Manifest = 'rknpu.man' },
    @{ Service = 'maliwddm'; Manifest = 'maliwddm.man' }
)
$state = 'HKLM:\SOFTWARE\RK3576\Counters'
$v2 = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\_V2Providers'
$logDir = Join-Path $env:ProgramData 'RK3576'
$log = Join-Path $logDir 'counters.log'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
if ((Test-Path $log) -and (Get-Item $log).Length -gt 262144) { Remove-Item $log -Force }
function Say([string]$m) { "$(Get-Date -Format s) $m" | Tee-Object -FilePath $log -Append }

function DriverDir([string]$svc) {
    $p = (Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\$svc" -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
    if (-not $p) { return $null }
    $p = $p -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''
    if ($p -notmatch '^[A-Za-z]:') { $p = Join-Path $env:SystemRoot $p }
    return Split-Path -Parent $p
}

function ManifestGuid([string]$man) {
    try {
        $x = New-Object System.Xml.XmlDocument
        $x.Load($man)
        $p = $x.GetElementsByTagName('provider') | Select-Object -First 1
        if ($p) { return $p.GetAttribute('providerGuid') }
    } catch { Say "cannot read $man : $($_.Exception.Message)" }
    return $null
}

function Unregister([string]$guid) {
    if (-not $guid) { return }
    $o = & (Join-Path $SysSeen 'unlodctr.exe') "/g:$guid" 2>&1
    Say "  unlodctr /g:$guid -> $LASTEXITCODE $o"
}

if (-not (Test-Path $state)) { New-Item -Path $state -Force | Out-Null }
$rc = 0
foreach ($pv in $providers) {
    $svc = $pv.Service
    $prevMan = (Get-ItemProperty -Path $state -Name $svc -ErrorAction SilentlyContinue).$svc
    $prevGuid = (Get-ItemProperty -Path $state -Name "${svc}Guid" -ErrorAction SilentlyContinue)."${svc}Guid"

    if ($Remove) {
        if ($prevGuid) {
            Say "${svc}: removing $prevGuid"
            Unregister $prevGuid
        }
        Remove-ItemProperty -Path $state -Name $svc, "${svc}Guid" -ErrorAction SilentlyContinue
        continue
    }

    $dir = DriverDir $svc
    $man = if ($dir) { Join-Path $dir $pv.Manifest } else { $null }
    if (-not $man -or -not (Test-Path (Seen $man))) {
        if ($dir) { Say "${svc}: no $($pv.Manifest) in $dir (a package without counters)" }
        else { Say "${svc}: driver not installed" }
        # the package that was registered is gone: so is its registration
        if ($prevGuid -and $prevMan -and -not (Test-Path (Seen $prevMan))) {
            Unregister $prevGuid
            Remove-ItemProperty -Path $state -Name $svc, "${svc}Guid" -ErrorAction SilentlyContinue
        }
        continue
    }
    $guid = ManifestGuid (Seen $man)
    if (-not $guid) { $rc = 1; continue }
    $registered = Test-Path (Join-Path $v2 $guid)
    if (-not $Force -and $prevMan -eq $man -and $registered) {
        Say "${svc}: $man already registered"
        continue
    }
    Say "${svc}: registering $man ($guid)"
    if ($prevGuid -and $prevGuid -ne $guid) { Unregister $prevGuid }
    Unregister $guid
    $o = & (Join-Path $SysSeen 'lodctr.exe') "/m:$man" "$dir" 2>&1
    $e = $LASTEXITCODE
    Say "  lodctr /m:$man $dir -> $e $o"
    if ($e -ne 0) { $rc = 1; continue }
    Set-ItemProperty -Path $state -Name $svc -Value $man
    Set-ItemProperty -Path $state -Name "${svc}Guid" -Value $guid
}

# At start, as SYSTEM: an NPU turned on in the firmware after the install
# gets UserAccess too (rk3576-setup.ps1 -Action NpuAccess, which leaves a
# value that is there alone).
$setup = Join-Path $PSScriptRoot 'rk3576-setup.ps1'
$npuNew = @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576' -ErrorAction SilentlyContinue | Where-Object {
    $null -eq (Get-ItemProperty -Path (Join-Path $_.PSPath 'Device Parameters') -Name UserAccess -ErrorAction SilentlyContinue) })
if (-not $Remove -and $npuNew.Count -and [Security.Principal.WindowsIdentity]::GetCurrent().IsSystem -and (Test-Path $setup)) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $setup -Action NpuAccess 2>&1 | ForEach-Object { Say "  $_" }
}
exit $rc
