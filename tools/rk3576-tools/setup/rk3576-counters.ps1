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
# skipped. Nothing else is changed.
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
    $o = & unlodctr.exe "/g:$guid" 2>&1
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
    if (-not $man -or -not (Test-Path $man)) {
        if ($dir) { Say "${svc}: no $($pv.Manifest) in $dir (a package without counters)" }
        else { Say "${svc}: driver not installed" }
        # the package that was registered is gone: so is its registration
        if ($prevGuid -and $prevMan -and -not (Test-Path $prevMan)) {
            Unregister $prevGuid
            Remove-ItemProperty -Path $state -Name $svc, "${svc}Guid" -ErrorAction SilentlyContinue
        }
        continue
    }
    $guid = ManifestGuid $man
    if (-not $guid) { $rc = 1; continue }
    $registered = Test-Path (Join-Path $v2 $guid)
    if (-not $Force -and $prevMan -eq $man -and $registered) {
        Say "${svc}: $man already registered"
        continue
    }
    Say "${svc}: registering $man ($guid)"
    if ($prevGuid -and $prevGuid -ne $guid) { Unregister $prevGuid }
    Unregister $guid
    $o = & lodctr.exe "/m:$man" "$dir" 2>&1
    $e = $LASTEXITCODE
    Say "  lodctr /m:$man $dir -> $e $o"
    if ($e -ne 0) { $rc = 1; continue }
    Set-ItemProperty -Path $state -Name $svc -Value $man
    Set-ItemProperty -Path $state -Name "${svc}Guid" -Value $guid
}
exit $rc
