# verify-bound.ps1 - the follow-up check of install.cmd /online, run as
# SYSTEM by the scheduled task rk3576-drivers-verify at each boot (2 minutes
# after startup) until it removes itself.
#
# install-online.ps1 writes expected.json next to this script: each device
# it marked for reinstall and the package DriverVer it should be on now.
# On 2026-11-27 PnP selected the new maliwddm for ACPI\RKCP7402\0 at the
# reinstall boot but never configured it; the GPU stayed on the older
# package and ConfigFlags was cleared. Marking that one device again and
# restarting bound the new one. This script does that, once:
#
# 1. Waits until PnP has no device installs pending (at most 10 minutes,
#    CMP_WaitNoPendingInstallEvents).
# 2. For each device of expected.json reads the bound driver
#    (DEVPKEY_Device_DriverInfPath, DriverDate, DriverVersion). A device is
#    done when that is the package's DriverVer or newer (date first, then
#    version, as PnP ranks them). An absent device is left alone.
# 3. All done: removes the task. Not done and the retry is not spent: marks
#    those devices again (ConfigFlags |= 0x20, mark-reinstall.ps1) and asks
#    for one more restart. It restarts by itself only when nobody is signed
#    in (no process of a user account), after a minute and a second check;
#    otherwise it tells the user (msg, a notice at sign-in, the Application
#    event log) and does not restart. Not done after the retry: logs it and
#    removes the task.
# Never re-marked whatever expected.json says: the SD/eMMC hosts (dwcsdhc,
# dwcmshc), and the Ethernet adapter (dwc_eqos, dwc_eqos10) once it has any
# driver.
# It never installs a driver or restarts a device itself.
#
#   verify-bound.ps1                      the boot check
#   verify-bound.ps1 -Notice              shows notice.txt (the sign-in task)
#   verify-bound.ps1 -Simulate fake.json  the check against made-up devices
#                                         and sign-ins; prints WOULD lines for
#                                         what it would change, writes only
#                                         reinstall.txt, state.json and the
#                                         log (CI runs it)
# Simulate file: {"devices": {"<instance>": {"inf": "oem71.inf", "date":
# "2026-11-27", "version": "5.59.40.53", "problem": 0}, ...}, "owners":
# ["S-1-5-18", "S-1-5-21-..."]}; a device missing from "devices" is absent,
# one with no "inf" has no driver.
# Log: verify-bound.log next to this script.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string]$Simulate,
    [switch]$Notice
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$TaskName = 'rk3576-drivers-verify'
$NoticeTask = 'rk3576-drivers-restart-notice'
$noticeFile = Join-Path $here 'notice.txt'

if ($Notice) {
    if (Test-Path -LiteralPath $noticeFile) {
        Add-Type -AssemblyName PresentationFramework
        $t = (Get-Content -LiteralPath $noticeFile) -join "`r`n"
        [void][System.Windows.MessageBox]::Show($t, 'RK3576 drivers')
    }
    exit 0
}

$sim = $null
if ($Simulate) { $sim = Get-Content -LiteralPath $Simulate -Raw | ConvertFrom-Json }
$logFile = Join-Path $here 'verify-bound.log'
$out = New-Object System.Collections.ArrayList
function Say([string]$t) { [void]$script:out.Add($t); Write-Host $t }
function Save-Log { try { $script:out | Out-File -FilePath $script:logFile -Encoding utf8 -Append } catch { } }

$NeverMark = @('dwcsdhc', 'dwcmshc')
$MarkOnlyIfNoDriver = @('dwc_eqos', 'dwc_eqos10')

# PnP's order between two packages for one device: the later date wins,
# then the higher version. $a and $b: @{ Date = [DateTime]; Version = [version] }.
function Test-Older($a, $b) {
    if (-not $a.Date) { return $true }
    if (-not $b.Date) { return $false }
    if ($a.Date -ne $b.Date) { return $a.Date -lt $b.Date }
    return $a.Version -lt $b.Version
}

function ConvertTo-Ver($date, $ver) {
    $d = $null
    if ($date) { $d = ([DateTime]$date).Date }
    $v = [version]'0.0.0.0'
    try { if ($ver) { $v = [version]$ver } } catch { }
    @{ Date = $d; Version = $v }
}

# The bound driver of one device: $null when absent, Inf $null for no driver.
function Get-Bound([string]$id) {
    if ($sim) {
        $p = $sim.devices.PSObject.Properties[$id]
        if (-not $p) { return $null }
        $d = $p.Value
        $v = ConvertTo-Ver $d.date $d.version
        return [pscustomobject]@{ Inf = $d.inf; Date = $v.Date; Version = $v.Version; Problem = [int]$d.problem }
    }
    $dev = Get-PnpDevice -InstanceId $id -PresentOnly -ErrorAction SilentlyContinue
    if (-not $dev) { return $null }
    $keys = 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverDate', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_ProblemCode'
    $p = @{}
    foreach ($x in Get-PnpDeviceProperty -InstanceId $id -KeyName $keys -ErrorAction SilentlyContinue) { $p[$x.KeyName] = $x.Data }
    $v = ConvertTo-Ver $p['DEVPKEY_Device_DriverDate'] $p['DEVPKEY_Device_DriverVersion']
    $prob = 0
    if ($p['DEVPKEY_Device_ProblemCode']) { $prob = [int]$p['DEVPKEY_Device_ProblemCode'] }
    [pscustomobject]@{ Inf = $p['DEVPKEY_Device_DriverInfPath']; Date = $v.Date; Version = $v.Version; Problem = $prob }
}

# Is anyone signed in? Any process of a user account: a local or domain
# account (S-1-5-21-...) or a Microsoft Entra one (S-1-12-...). SSH and
# remote desktop sessions count. Service, DWM and font driver accounts not.
function Test-UserPresent($sids) {
    foreach ($s in @($sids)) { if ("$s" -match '^S-1-(5-21|12)-') { return $true } }
    return $false
}

function Get-OwnerSids {
    if ($sim) { return @($sim.owners) }
    $r = @()
    foreach ($p in Get-CimInstance Win32_Process -ErrorAction SilentlyContinue) {
        try { $r += (Invoke-CimMethod -InputObject $p -MethodName GetOwnerSid -ErrorAction Stop).Sid } catch { }
    }
    $r
}

function Act([string]$what, [scriptblock]$do) {
    if ($sim) { Say "WOULD $what"; return }
    try { & $do } catch { Say "  failed: $what ($_)" }
}

function Remove-Tasks {
    foreach ($t in $TaskName, $NoticeTask) {
        Act "delete task $t" { Unregister-ScheduledTask -TaskName $t -Confirm:$false -ErrorAction SilentlyContinue }
    }
}

function Write-Event([string]$type, [string]$text) {
    Act "log event ($type): $text" { & eventcreate /l APPLICATION /so RK3576-drivers /t $type /id 100 /d $text | Out-Null }
}

Say "verify-bound $(Get-Date -Format s)$(if ($sim) { ' (simulated)' })"
try {
    $expFile = Join-Path $here 'expected.json'
    if (-not (Test-Path -LiteralPath $expFile)) {
        Say 'no expected.json: nothing to check'
        Remove-Tasks
        Save-Log
        exit 0
    }
    $exp = Get-Content -LiteralPath $expFile -Raw | ConvertFrom-Json
    $stFile = Join-Path $here 'state.json'
    $st = [pscustomobject]@{ retries = 0; max = 1 }
    if (Test-Path -LiteralPath $stFile) { $st = Get-Content -LiteralPath $stFile -Raw | ConvertFrom-Json }
    Say "package $($exp.package), board $($exp.board), staged $($exp.created); retry $($st.retries) of $($st.max) used"

    if (-not $sim) {
        try {
            Add-Type -Namespace Rk3576 -Name Cfg -MemberDefinition '[DllImport("cfgmgr32.dll")] public static extern uint CMP_WaitNoPendingInstallEvents(uint ms);'
            $w = [Rk3576.Cfg]::CMP_WaitNoPendingInstallEvents(600000)
            Say $(if ($w -eq 0) { 'no device installs pending' } else { "device installs still pending after 10 minutes (0x$('{0:x}' -f $w)); checking anyway" })
        } catch { Say "could not wait for pending device installs ($_); checking anyway" }
    }

    $bad = New-Object System.Collections.ArrayList
    foreach ($e in @($exp.devices)) {
        $want = ConvertTo-Ver $e.date $e.version
        $wtext = '{0} {1},{2}' -f $e.inf, $e.date, $e.version
        $b = Get-Bound $e.id
        if (-not $b) { Say "$($e.id) ($($e.dir)): absent, not checked"; continue }
        $btext = if ($b.Inf) { '{0} {1:yyyy-MM-dd},{2}' -f $b.Inf, $b.Date, $b.Version } else { 'no driver' }
        if ($b.Problem) { $btext += ", problem $($b.Problem)" }
        if ($b.Inf -and -not (Test-Older $b $want)) { Say "$($e.id) ($($e.dir)): done [$btext]"; continue }
        if ($NeverMark -contains $e.dir) { Say "$($e.id) ($($e.dir)): wants $wtext, has [$btext]; storage host, not re-marked"; continue }
        if ($MarkOnlyIfNoDriver -contains $e.dir -and $b.Inf) { Say "$($e.id) ($($e.dir)): wants $wtext, has [$btext]; network adapter with a driver, not re-marked"; continue }
        Say "$($e.id) ($($e.dir)): wants $wtext, has [$btext]; NOT BOUND"
        [void]$bad.Add($e.id)
    }

    if (-not $bad.Count) {
        Say 'every device took its new driver'
        Write-Event 'INFORMATION' "RK3576 drivers $($exp.package): every updated device took its new driver."
        Remove-Tasks
    } elseif ([int]$st.retries -ge [int]$st.max) {
        $t = "RK3576 drivers $($exp.package): $($bad.Count) device(s) still on the old driver after $($st.retries) retry: $($bad -join ', '). See $logFile."
        Say "giving up: $t"
        Write-Event 'WARNING' $t
        Remove-Tasks
    } else {
        $st.retries = [int]$st.retries + 1
        $st | ConvertTo-Json | Out-File -FilePath $stFile -Encoding ascii
        $bad | Out-File -FilePath (Join-Path $here 'reinstall.txt') -Encoding ascii
        Act "mark for reinstall: $($bad -join ' ')" {
            $ml = Join-Path $here 'mark-reinstall.log'
            Remove-Item -LiteralPath $ml -ErrorAction SilentlyContinue
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'mark-reinstall.ps1')
            Get-Content -LiteralPath $ml | ForEach-Object { Say "  $_" }
        }
        $t = "RK3576 drivers $($exp.package): $($bad.Count) device(s) did not take the new driver at this boot ($($bad -join ', ')). They are marked again; one more restart of Windows finishes the update."
        Say "retry $($st.retries) of $($st.max): marked again"
        Write-Event 'WARNING' $t
        $short = "One more restart of Windows finishes the RK3576 driver update: $($bad.Count) device(s) did not take the new driver at this boot and are marked again."
        $present = Test-UserPresent (Get-OwnerSids)
        if (-not $present -and -not $sim) {
            Start-Sleep -Seconds 60
            $present = Test-UserPresent (Get-OwnerSids)
        }
        if ($present) {
            Say 'a user is signed in: not restarting, telling them'
            $short | Out-File -FilePath $noticeFile -Encoding utf8
            Act "register task $NoticeTask (a notice at each sign-in until the next boot check)" {
                $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $here\verify-bound.ps1 -Notice"
                $p = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
                $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
                Register-ScheduledTask -TaskName $NoticeTask -Action $a -Principal $p -Settings $s -Trigger (New-ScheduledTaskTrigger -AtLogOn) -Force | Out-Null
            }
            Act 'tell the signed-in users now' {
                $msg = Join-Path $env:SystemRoot 'System32\msg.exe'
                if (Test-Path -LiteralPath $msg) { & $msg * /time:86400 $short }
                else { Start-ScheduledTask -TaskName $NoticeTask }
            }
        } else {
            Say 'nobody is signed in: restarting in 60 seconds'
            Act 'restart in 60 seconds' { & shutdown /r /t 60 /d p:2:4 /c 'RK3576 drivers: restarting once to finish a driver update.' }
        }
    }
} catch {
    # A check that cannot run would fail at every boot: stop here.
    Say "FAILED: $_"
    Remove-Tasks
    Save-Log
    exit 1
}
Save-Log
exit 0
