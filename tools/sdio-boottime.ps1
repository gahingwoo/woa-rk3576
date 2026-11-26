<#
.SYNOPSIS
  Where this boot's time went on the SDIO Wi-Fi and the SD slot: the
  dwcmshc SdioDiag values, the bcmwlsdio timings per SDIO function, and the
  boot's PnP and Diagnostics-Performance events for those devices.

.DESCRIPTION
  Read-only. Run it elevated, after the boot you want to look at (the
  Diagnostics-Performance log needs an administrator).

  Times from the drivers are milliseconds since boot (interrupt time).
  Event times are converted to the same scale from the boot time Windows
  reports, so the two line up to within a second or so.

  What to look at first:
    dwcmshc  IorxReads / IorxFirstMs..IorxLastMs: CMD52 reads of IORx.
             sdbus polls IORx up to 2000 times per function it enables at
             PDO start (one clock tick apart, ~15.6 ms). Thousands of reads
             over a minute is that poll; a handful is normal.
             MaxCmdUs, SlowCmds, AbandonedCmds, CmdTimeouts: requests that
             took long or never completed.
    bcmwlsdio AddToD0Ms per function: AddDevice to D0, which spans the
             PDO start in sdbus. EarlyOpenStatus 0 means the SDBUS
             interface (with "do not manage IOEx") was set before that start.

  The dwcmshc values exist only with a dwcmshc build that has SdioDiag
  Version 3 (branch sdio-boot-time); older builds show fewer values.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File sdio-boottime.ps1
  powershell -ExecutionPolicy Bypass -File sdio-boottime.ps1 -AllEvents

  Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
  SPDX-License-Identifier: GPL-2.0-only
#>
param(
    # List every Kernel-PnP event of this boot, not just the ones for the
    # SDIO and SD devices.
    [switch]$AllEvents
)

$ErrorActionPreference = 'Continue'

$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
"Boot: $boot  (now +{0:N0} s)" -f ((Get-Date) - $boot).TotalSeconds

function SinceBoot([datetime]$t) {
    [int64](($t - $boot).TotalMilliseconds)
}

# The devices: SDIO controller, SD slot, and the SDIO card's functions.
$patterns = @('ACPI\RKCPFE2D*', 'ACPI\RKCPFE2C*', 'SD\VID_02D0*')
$devices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $id = $_.InstanceId
    ($patterns | Where-Object { $id -like $_ }).Count -gt 0
})

''
'== Devices'
foreach ($d in $devices) {
    $ver = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
    $inf = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
    $prob = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
    '{0,-8} {1}  driver {2} ({3}) problem {4}' -f $d.Status, $d.InstanceId, $ver, $inf, $prob
}
foreach ($f in 'sdbus.sys', 'sdport.sys') {
    $p = Join-Path $env:SystemRoot "System32\drivers\$f"
    if (Test-Path $p) { '{0,-10} {1}' -f $f, (Get-Item $p).VersionInfo.FileVersion }
}

''
'== dwcmshc SdioDiag (SDIO controller)'
$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\dwcmshc\SdioDiag'
if (Test-Path $key) {
    $v = Get-ItemProperty $key
    $names = $v.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { $_.Name }
    foreach ($n in $names) {
        $x = $v.$n
        if ($x -is [int] -or $x -is [long]) {
            '{0,-20} {1,12}  0x{2:X8}' -f $n, ([uint32]($x -band 0xFFFFFFFFL)), ([uint32]($x -band 0xFFFFFFFFL))
        } else {
            '{0,-20} {1}' -f $n, $x
        }
    }
    if ($null -ne $v.IorxReads) {
        ''
        'IORx polled {0} times over {1} ms; IOEx written {2} times, last 0x{3:X2}' -f `
            $v.IorxReads, ($v.IorxLastMs - $v.IorxFirstMs), $v.IoexWrites, $v.LastIoexValue
        if ($v.Commands -gt 0 -and $null -ne $v.TotalCmdUs) {
            'Commands {0}, {1:N0} us each on average, longest {2} us (CMD{3}), {4} over 100 ms' -f `
                $v.Commands, ($v.TotalCmdUs / [math]::Max(1, $v.Commands)), $v.MaxCmdUs, $v.MaxCmdIndex, $v.SlowCmds
        }
        'First command at {0} ms, CMD7 at {1} ms, 4-bit at {2} ms, last command at {3} ms' -f `
            $v.FirstCmdMs, $v.Cmd7Ms, $v.Bus4BitMs, $v.LastCmdMs
    }
} else {
    'no SdioDiag key: the SDIO controller was not bound by dwcmshc this boot'
}

''
'== bcmwlsdio per SDIO function (Device Parameters)'
$want = 'FunctionNumber', 'StageSeen', 'EarlyOpenStatus', 'SdOpenStatus', 'AddDeviceMs',
        'PrepareHardwareMs', 'D0EntryMs', 'AddToD0Ms', 'BringUpDoneMs', 'FwState', 'LastStep',
        'Cmd52Count', 'Cmd53Count', 'Cmd52Errors', 'Cmd53Errors'
foreach ($d in $devices | Where-Object { $_.InstanceId -like 'SD\*' }) {
    $dp = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
    "-- $($d.InstanceId)"
    if (-not (Test-Path $dp)) { '   (no Device Parameters)'; continue }
    $p = Get-ItemProperty $dp
    foreach ($n in $want) {
        if ($null -ne $p.$n) {
            if ($n -like '*Status') { '   {0,-18} 0x{1:X8}' -f $n, ([uint32]($p.$n -band 0xFFFFFFFFL)) }
            else { '   {0,-18} {1}' -f $n, $p.$n }
        }
    }
}

# Events of this boot. Kernel-PnP: 400 configured, 410 started, 411 problem
# starting, 420 deleted, 430 needs further installation (the Configuration
# log); the System log carries Kernel-PnP warnings such as 219.
''
'== Kernel-PnP events of this boot (ms since boot)'
$match = {
    param($e)
    if ($AllEvents) { return $true }
    $m = $e.Message
    ($m -match 'RKCPFE2D' -or $m -match 'RKCPFE2C' -or $m -match 'VID_02D0' -or $m -match 'dwcmshc' -or $m -match 'bcmwlsdio' -or $m -match 'sdbus')
}
$pnp = @()
foreach ($log in 'Microsoft-Windows-Kernel-PnP/Configuration', 'Microsoft-Windows-Kernel-PnP/Device Configuration') {
    $pnp += @(Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = $boot } -ErrorAction SilentlyContinue)
}
$pnp += @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-PnP'; StartTime = $boot } -ErrorAction SilentlyContinue)
$pnp | Where-Object { & $match $_ } | Sort-Object TimeCreated | ForEach-Object {
    $first = ($_.Message -split "`r?`n" | Where-Object { $_ -match '\S' } | Select-Object -First 2) -join ' | '
    '{0,9} ms  id {1,-4} {2}' -f (SinceBoot $_.TimeCreated), $_.Id, $first
}

# Diagnostics-Performance: 100 boot, 101 slow application start, 102 slow
# driver initialisation, 103 slow service start, 109 slow device
# initialisation, 110 session manager initialisation.
''
'== Diagnostics-Performance events of this boot'
$perf = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Diagnostics-Performance/Operational'; Id = 100, 101, 102, 103, 109, 110; StartTime = $boot.AddMinutes(-1) } -ErrorAction SilentlyContinue)
if ($perf.Count -eq 0) {
    'none (not elevated, or Windows did not record this boot: it writes them a while after the desktop is up)'
}
foreach ($e in $perf | Sort-Object TimeCreated) {
    $x = [xml]$e.ToXml()
    $data = @{}
    foreach ($d in $x.Event.EventData.Data) { $data[$d.Name] = $d.'#text' }
    switch ($e.Id) {
        100 { 'boot: BootTime {0} ms, MainPathBootTime {1} ms, BootPostBootTime {2} ms' -f $data.BootTime, $data.MainPathBootTime, $data.BootPostBootTime }
        default {
            $name = @($data.FriendlyName, $data.Name, $data.FileName) | Where-Object { $_ } | Select-Object -First 1
            'id {0}: {1}  TotalTime {2} ms, DegradationTime {3} ms' -f $e.Id, $name, $data.TotalTime, $data.DegradationTime
        }
    }
}
