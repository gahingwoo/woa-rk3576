# install-online.ps1 - install.cmd /online: put the driver package for one
# board into this running Windows without starting any of it.
#
#   install-online.ps1 -Board tspi3m|cm5io|rock4d [-Optional] [-Plan]
#
# 1. Trusts certs\*.cer (Root and TrustedPublisher).
# 2. Stages every driver of boards\<board>.txt (and <board>-optional.txt with
#    -Optional): pnputil /add-driver, never /install. Nothing is loaded now.
# 3. Finds the present devices each staged INF matches (hardware or
#    compatible ID) and marks for reinstall (ConfigFlags |= 0x20,
#    CONFIGFLAG_REINSTALL) the ones with no driver or an older one than the
#    package's DriverVer. The marking runs as SYSTEM, the only account that
#    may write the Enum keys, through a one-shot scheduled task running
#    mark-reinstall.ps1. At the next boot PnP installs them from the store.
#    Never marked: the SD/eMMC hosts (dwcsdhc, dwcmshc), which the running
#    Windows may be booted from, and the Ethernet adapter (dwc_eqos) unless
#    it has no driver at all. Their packages are staged only.
# 4. Asks for a restart. Nothing else is changed.
#
# -Plan does step 3's matching, prints what would be marked, and
# changes nothing (no certutil, pnputil or scheduled task). CI runs it.
# Log: install-online.log in a new %SystemRoot%\Temp\rk3576-drivers-<id>
# folder (made here, so only administrators can write to it: the SYSTEM
# task runs the copy of mark-reinstall.ps1 in it).
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Board,
    [switch]$Optional,
    [switch]$Plan
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$work = Join-Path $env:SystemRoot ('Temp\rk3576-drivers-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$log = @()
function Say([string]$t) { $script:log += $t; Write-Host $t }

# Drivers whose devices are never marked, and why.
$NeverMark = @{
    'dwcsdhc' = 'eMMC host: Windows may be running from it; staged only, it is taken by an offline install (install.cmd W:\ from WinPE)'
    'dwcmshc' = 'SD/SDIO host: Windows may be running from it; staged only, it is taken by an offline install (install.cmd W:\ from WinPE)'
}
$MarkOnlyIfNoDriver = @{
    'dwc_eqos' = 'network adapter with a driver: staged only, so a remote session keeps its connection'
}

# The parts of an INF this needs: DriverVer and the IDs of its models sections.
function Read-Inf([string]$path) {
    $sec = @{}
    $cur = $null
    foreach ($raw in Get-Content -LiteralPath $path) {
        # Drop a comment: a ';' outside double quotes.
        $l = ''
        $q = $false
        foreach ($c in $raw.ToCharArray()) {
            if ($c -eq '"') { $q = -not $q }
            elseif ($c -eq ';' -and -not $q) { break }
            $l += $c
        }
        $l = $l.Trim()
        if (-not $l) { continue }
        if ($l -match '^\[(.+)\]$') {
            $cur = $Matches[1].Trim().ToLowerInvariant()
            if (-not $sec.ContainsKey($cur)) { $sec[$cur] = New-Object System.Collections.ArrayList }
            continue
        }
        if ($cur) { [void]$sec[$cur].Add($l) }
    }
    $ver = $null; $date = $null
    foreach ($l in @($sec['version'])) {
        if ($l -match '^DriverVer\s*=\s*(\d{1,2})/(\d{1,2})/(\d{4})\s*(,\s*([\d.]+))?') {
            $date = New-Object DateTime ([int]$Matches[3]), ([int]$Matches[1]), ([int]$Matches[2])
            $v = if ($Matches[5]) { $Matches[5] } else { '0.0.0.0' }
            while (($v -split '\.').Count -lt 4) { $v += '.0' }
            $ver = [version]$v
        }
    }
    $ids = New-Object System.Collections.ArrayList
    foreach ($m in @($sec['manufacturer'])) {
        if ($m -notmatch '=') { continue }
        $f = @(($m -split '=', 2)[1] -split ',' | ForEach-Object { $_.Trim() })
        $models = @($f[0])
        foreach ($dec in $f[1..($f.Count)]) { if ($dec) { $models += "$($f[0]).$dec" } }
        foreach ($ms in $models) {
            foreach ($l in @($sec[$ms.ToLowerInvariant()])) {
                if ($l -notmatch '=') { continue }
                $parts = @(($l -split '=', 2)[1] -split ',' | ForEach-Object { $_.Trim() })
                foreach ($id in $parts[1..($parts.Count)]) {
                    if ($id -and -not $ids.Contains($id.ToUpperInvariant())) { [void]$ids.Add($id.ToUpperInvariant()) }
                }
            }
        }
    }
    [pscustomobject]@{ Path = $path; Date = $date; Version = $ver; Ids = @($ids) }
}

function Get-DriverOf([string]$instance) {
    $keys = 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverDate', 'DEVPKEY_Device_DriverVersion'
    $p = @{}
    foreach ($x in Get-PnpDeviceProperty -InstanceId $instance -KeyName $keys -ErrorAction SilentlyContinue) {
        $p[$x.KeyName] = $x.Data
    }
    $inf = $p['DEVPKEY_Device_DriverInfPath']
    if (-not $inf) { return $null }
    $ver = $null
    try { $ver = [version]$p['DEVPKEY_Device_DriverVersion'] } catch { $ver = [version]'0.0.0.0' }
    $date = $null
    if ($p['DEVPKEY_Device_DriverDate']) { $date = ([DateTime]$p['DEVPKEY_Device_DriverDate']).Date }
    [pscustomobject]@{ Inf = $inf; Date = $date; Version = $ver }
}

# PnP's own order between two packages for one device: the later date wins,
# then the higher version.
function Test-Newer($pkg, $cur) {
    if (-not $pkg.Date) { return $false }
    if (-not $cur.Date) { return $true }
    if ($pkg.Date -ne $cur.Date) { return $pkg.Date -gt $cur.Date }
    return $pkg.Version -gt $cur.Version
}

$lists = @(Join-Path $here "boards\$Board.txt")
if (-not (Test-Path -LiteralPath $lists[0])) { Write-Host "There is no board $Board in this package (boards\$Board.txt)."; exit 2 }
if ($Optional) { $lists += Join-Path $here "boards\$Board-optional.txt" }
$dirs = @()
foreach ($f in $lists) {
    if (Test-Path -LiteralPath $f) {
        $dirs += @(Get-Content -LiteralPath $f | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
    }
}
$verFile = Join-Path $here 'VERSION.txt'
$pkgVer = if (Test-Path -LiteralPath $verFile) { (Get-Content -LiteralPath $verFile -TotalCount 1).Trim() } else { 'unknown' }
Say "RK3576 driver package $pkgVer, board $Board$(if ($Plan) { ', plan only (nothing is changed)' })."

$failed = 0
if (-not $Plan) {
    # pnputil refuses a catalog signed "in the future"; a board without an
    # RTC battery may boot with a clock behind. Best effort.
    & w32tm /resync /force | Out-Null
    foreach ($c in Get-ChildItem (Join-Path $here 'certs') -Filter *.cer) {
        foreach ($store in 'Root', 'TrustedPublisher') {
            & certutil -f -addstore $store $c.FullName | Out-Null
            if ($LASTEXITCODE -ne 0) { Say "Could not trust $($c.Name) ($store, exit $LASTEXITCODE)."; $failed++ }
        }
    }
}

Say 'Present devices...'
$devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
    $ids = @($_.HardwareID) + @($_.CompatibleID) | Where-Object { $_ } | ForEach-Object { $_.ToUpperInvariant() }
    [pscustomobject]@{ Id = $_.InstanceId; Name = $_.FriendlyName; Ids = @($ids) }
})
Say "  $($devices.Count) present."

$mark = New-Object System.Collections.ArrayList
foreach ($d in $dirs) {
    $infs = @(Get-ChildItem (Join-Path $here "drivers\$d") -Filter *.inf -ErrorAction SilentlyContinue)
    if (-not $infs) { Say "${d}: no INF in drivers\$d."; $failed++; continue }
    foreach ($i in $infs) {
        if (-not $Plan) {
            $o = & pnputil /add-driver $i.FullName
            $rc = $LASTEXITCODE
            if ($rc -ne 0 -and $rc -ne 3010) {
                Say "${d}: pnputil /add-driver $($i.Name) failed (exit $rc)."
                $o | ForEach-Object { Say "    $_" }
                $failed++
                continue
            }
        }
        $pkg = Read-Inf $i.FullName
        $vtext = if ($pkg.Date) { '{0:MM/dd/yyyy},{1}' -f $pkg.Date, $pkg.Version } else { 'no DriverVer' }
        Say "${d}: $(if ($Plan) { 'would stage' } else { 'staged' }) $($i.Name) ($vtext; IDs: $($pkg.Ids -join ' '))"
        foreach ($dev in $devices) {
            $hit = $false
            foreach ($id in $dev.Ids) { if ($pkg.Ids -contains $id) { $hit = $true; break } }
            if (-not $hit) { continue }
            $cur = Get-DriverOf $dev.Id
            $now = if ($cur) { '{0} {1:MM/dd/yyyy},{2}' -f (Split-Path $cur.Inf -Leaf), $cur.Date, $cur.Version } else { 'no driver' }
            $what = $null
            if ($NeverMark.ContainsKey($d)) {
                $what = "not marked ($($NeverMark[$d]))"
            } elseif ($MarkOnlyIfNoDriver.ContainsKey($d) -and $cur) {
                $what = "not marked ($($MarkOnlyIfNoDriver[$d]))"
            } elseif (-not $cur -or (Test-Newer $pkg $cur)) {
                $what = 'marked for reinstall'
                if (-not $mark.Contains($dev.Id)) { [void]$mark.Add($dev.Id) }
            } else {
                $what = 'up to date'
            }
            Say "    $($dev.Id) [$now]: $what"
        }
    }
}

if ($Plan) {
    Say "$($mark.Count) device(s) would be marked for reinstall."
    exit $(if ($failed) { 1 } else { 0 })
}

if ($mark.Count) {
    # The task runs mark-reinstall.ps1 from a folder with no spaces in it.
    if ($work -match '\s') { Say "The work folder has a space in its path: $work"; exit 1 }
    New-Item -ItemType Directory -Path $work | Out-Null
    Copy-Item -LiteralPath (Join-Path $here 'mark-reinstall.ps1') -Destination $work -Force
    $mark | Out-File -FilePath (Join-Path $work 'reinstall.txt') -Encoding ascii
    $mlog = Join-Path $work 'mark-reinstall.log'
    Remove-Item -LiteralPath $mlog -ErrorAction SilentlyContinue
    $task = 'rk3576-drivers-mark'
    $tr = "powershell -NoProfile -ExecutionPolicy Bypass -File $work\mark-reinstall.ps1"
    & schtasks /create /tn $task /ru SYSTEM /sc once /st 00:00 /f /tr $tr | Out-Null
    if ($LASTEXITCODE -ne 0) { Say "schtasks /create failed (exit $LASTEXITCODE)."; $failed++ }
    else {
        & schtasks /run /tn $task | Out-Null
        for ($n = 0; $n -lt 90 -and -not (Test-Path -LiteralPath $mlog); $n++) { Start-Sleep -Seconds 1 }
        & schtasks /delete /tn $task /f | Out-Null
        if (-not (Test-Path -LiteralPath $mlog)) { Say 'mark-reinstall.ps1 did not run (no log after 90 s).'; $failed++ }
        else {
            Get-Content -LiteralPath $mlog | ForEach-Object { Say "  $_" }
            foreach ($id in $mark) {
                $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
                $cf = 0
                try { $cf = [int](Get-ItemProperty -LiteralPath $k -Name ConfigFlags).ConfigFlags } catch { }
                if (-not ($cf -band 0x20)) { Say ("{0}: ConfigFlags 0x{1:x8}, the reinstall bit is not set." -f $id, $cf); $failed++ }
            }
        }
    }
}
Say "$($mark.Count) device(s) marked for reinstall."

try {
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $log | Out-File -FilePath (Join-Path $work 'install-online.log') -Encoding utf8
} catch { }
if ($failed) {
    Write-Host "$failed step(s) failed (log: $work\install-online.log)."
    exit 1
}
Write-Host "RK3576 drivers $pkgVer staged. Restart Windows (shutdown /r /t 0): the marked devices install at the next boot."
exit 0
