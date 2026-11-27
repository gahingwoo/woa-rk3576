# rk3576-setup.ps1 - install, finish or remove the RK3576 tools
#
#   -Action Install -Source <dir>   copy <dir> (the bundle's RK3576 folder) to
#                                   %ProgramFiles%\RK3576, then everything below
#   -Action FirstBoot               what an offline install could not do: the
#                                   Start menu, the two tasks, the counters
#                                   (run as SYSTEM from SetupComplete.cmd)
#   -Action Uninstall               remove all of it again
#
# What an install leaves on the system, and nothing else:
#   %ProgramFiles%\RK3576\                          the files
#   Task Scheduler \RK3576\RK3576 Monitor           rk-monitor at every logon, any user, as
#                                                   that user, not elevated, no delay
#   HKLM\SOFTWARE\RK3576                            InstallDir, Version, Counters\ (state)
#   HKLM\SOFTWARE\RK3576\Monitor  Widget = 1        the desktop widget on by default
#                                  AutostartTask = 1 the logon task exists (rk-monitor's menu)
#   HKLM\...\Uninstall\RK3576Tools                  the entry in Settings, Apps
#   Start menu\Programs\RK3576\                     three shortcuts (all users)
#   Task Scheduler \RK3576\Performance counters     SYSTEM, at start: rk3576-counters.ps1
#   the GPU and NPU counter registrations (lodctr), and %ProgramData%\RK3576 (logs)
#   nothing from rk3576-browser-gpu.ps1: its Install and Uninstall only take
#   out the --disable-direct-composition switch, task \RK3576\Browser GPU,
#   %ProgramFiles%\RK3576\browser-gpu and HKLM\SOFTWARE\RK3576\BrowserGpu
#   an earlier bundle left (../BROWSER-GPU.md)
# An offline install starts rk-monitor through the HKLM Run entry
# "RK3576 Monitor" until its first boot; the task replaces it then (Run
# entries wait for Explorer's startup delay, about 30 s; a logon task does
# not). Uninstall removes all of those, and each user's HKCU\Software\RK3576 and HKCU Run
# "RK3576 Monitor" (rk-monitor's own settings).
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][ValidateSet('Install', 'FirstBoot', 'Uninstall')][string]$Action,
    [string]$Source,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
$Dir      = Join-Path $env:ProgramFiles 'RK3576'
$RunKey   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$RunName  = 'RK3576 Monitor'
$Base     = 'HKLM:\SOFTWARE\RK3576'
$Arp      = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RK3576Tools'
$Menu     = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\RK3576'
$TaskPath = '\RK3576\'
$TaskName = 'Performance counters'
$LogonTask = 'RK3576 Monitor'
$LogDir   = Join-Path $env:ProgramData 'RK3576'

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'setup.log'
function Say([string]$m) { "$(Get-Date -Format s) $m" | Tee-Object -FilePath $Log -Append }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Stop-Monitor {
    Get-Process -Name 'rk-monitor' -ErrorAction SilentlyContinue | ForEach-Object {
        Say "stopping rk-monitor (pid $($_.Id))"
        try { $_.Kill(); $_.WaitForExit(5000) | Out-Null } catch { Say "  $($_.Exception.Message)" }
    }
}

# The machine-wide registry values. The offline installer writes the same
# ones into a mounted image (offline-install.cmd), so they stay in step.
function Set-MachineRegistry {
    $ver = 'unknown'
    $vf = Join-Path $Dir 'VERSION.txt'
    if (Test-Path $vf) { $ver = (Get-Content $vf -TotalCount 1).Trim() }

    New-Item -Path "$Base\Monitor" -Force | Out-Null
    if ($null -eq (Get-ItemProperty -Path "$Base\Monitor" -Name Widget -ErrorAction SilentlyContinue)) {
        New-ItemProperty -Path "$Base\Monitor" -Name Widget -PropertyType DWord -Value 1 | Out-Null
    }
    New-ItemProperty -Path $Base -Name InstallDir -PropertyType ExpandString -Value '%ProgramFiles%\RK3576' -Force | Out-Null
    New-ItemProperty -Path $Base -Name Version -PropertyType String -Value $ver -Force | Out-Null

    New-Item -Path $Arp -Force | Out-Null
    $kb = [int]((Get-ChildItem $Dir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1024)
    $vals = @{
        DisplayName     = 'RK3576 tools'
        DisplayVersion  = $ver
        Publisher       = 'woa-rk3576'
        InstallLocation = $Dir
        DisplayIcon     = (Join-Path $Dir 'rk-monitor.exe')
        UninstallString = '"' + (Join-Path $Dir 'setup\uninstall.cmd') + '"'
        URLInfoAbout    = 'https://github.com/gahingwoo/woa-rk3576'
    }
    foreach ($k in $vals.Keys) { New-ItemProperty -Path $Arp -Name $k -PropertyType String -Value $vals[$k] -Force | Out-Null }
    foreach ($k in 'NoModify', 'NoRepair') { New-ItemProperty -Path $Arp -Name $k -PropertyType DWord -Value 1 -Force | Out-Null }
    New-ItemProperty -Path $Arp -Name EstimatedSize -PropertyType DWord -Value $kb -Force | Out-Null
    Say "registry: Monitor\Widget, RK3576, Uninstall\RK3576Tools ($ver)"
}

# rk-monitor at logon: a task for the Users group (any user, in that
# user's session, not elevated), started at once, not after Explorer's
# startup delay as a Run entry is. It replaces the offline install's Run
# entry.
function Set-LogonTask {
    $exe = Join-Path $Dir 'rk-monitor.exe'
    $a = New-ScheduledTaskAction -Execute $exe -Argument '--autostart' -WorkingDirectory $Dir
    $t = New-ScheduledTaskTrigger -AtLogOn
    $p = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
           -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances Parallel -Priority 6
    Register-ScheduledTask -TaskPath $TaskPath -TaskName $LogonTask -Action $a -Trigger $t -Principal $p `
        -Settings $s -Description 'Starts the RK3576 Monitor tray app and desktop widget at logon.' `
        -Force | Out-Null
    New-ItemProperty -Path "$Base\Monitor" -Name AutostartTask -PropertyType DWord -Value 1 -Force | Out-Null
    Remove-ItemProperty -Path $RunKey -Name $RunName -ErrorAction SilentlyContinue
    Say "task: $TaskPath$LogonTask (Users, at logon); no Run entry"
}

function New-Shortcut([string]$lnk, [string]$target, [string]$arguments, [string]$desc) {
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath = $target
    if ($arguments) { $s.Arguments = $arguments }
    $s.WorkingDirectory = $Dir
    $s.Description = $desc
    $s.Save()
}

function Set-Shortcuts {
    New-Item -ItemType Directory -Force -Path $Menu | Out-Null
    New-Shortcut (Join-Path $Menu 'RK3576 Monitor.lnk') (Join-Path $Dir 'rk-monitor.exe') '' 'CPU, GPU and NPU use and the temperatures, in the tray and on the desktop'
    New-Shortcut (Join-Path $Menu 'RK3576 tools folder.lnk') (Join-Path $env:SystemRoot 'explorer.exe') "`"$Dir`"" 'rknputest, charsiu_run and the scripts'
    New-Shortcut (Join-Path $Menu 'Uninstall RK3576 tools.lnk') (Join-Path $Dir 'setup\uninstall.cmd') '' 'Remove the RK3576 tools'
    Say "Start menu: $Menu"
}

function Set-CounterTask {
    $script = Join-Path $Dir 'setup\rk3576-counters.ps1'
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' `
           -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`""
    $t = New-ScheduledTaskTrigger -AtStartup
    $t.Delay = 'PT30S'
    $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
           -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
    Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $a -Trigger $t -Principal $p `
        -Settings $s -Description 'Registers the RK3576 GPU and NPU performance counters again when a driver update moved them.' `
        -Force | Out-Null
    Say "task: $TaskPath$TaskName"
}

function Invoke-Counters([string[]]$extra) {
    $ErrorActionPreference = 'Continue'     # a native program's stderr is not an error here
    $script = Join-Path $Dir 'setup\rk3576-counters.ps1'
    if (-not (Test-Path $script)) { return }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script @extra 2>&1 | ForEach-Object { Say "  $_" }
}

# Takes an earlier bundle's browser switch back out (BROWSER-GPU.md). Its
# own process and log; a failure here never fails the install.
function Invoke-BrowserGpu([string]$what) {
    $ErrorActionPreference = 'Continue'
    $script = Join-Path $Dir 'setup\rk3576-browser-gpu.ps1'
    if (-not (Test-Path $script)) { $script = Join-Path $Dir 'browser-gpu\rk3576-browser-gpu.ps1' }
    if (-not (Test-Path $script)) { return }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Action $what 2>&1 | ForEach-Object { Say "  $_" }
    Say "browser GPU: $what, exit $LASTEXITCODE"
}

function Complete-Setup {
    Set-MachineRegistry
    Set-Shortcuts
    Set-LogonTask
    Set-CounterTask
    Invoke-Counters @('-Force')
    Invoke-BrowserGpu 'Install'
    Remove-ItemProperty -Path $Base -Name FirstBootPending -ErrorAction SilentlyContinue
}

# Every user's own rk-monitor state: loaded hives in HKEY_USERS, and the
# profiles that are not loaded, through their NTUSER.DAT.
function Remove-UserState {
    $ErrorActionPreference = 'Continue'
    $done = @{}
    $users = Get-ChildItem Registry::HKEY_USERS -ErrorAction SilentlyContinue |
             Where-Object { $_.PSChildName -match '^S-1-5-21-[\d-]+$' }
    foreach ($u in $users) {
        $root = "Registry::HKEY_USERS\$($u.PSChildName)"
        Remove-Item -Path "$root\Software\RK3576" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path "$root\Software\Microsoft\Windows\CurrentVersion\Run" -Name $RunName -ErrorAction SilentlyContinue
        $done[$u.PSChildName] = $true
    }
    $plist = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
    foreach ($p in Get-ChildItem $plist -ErrorAction SilentlyContinue) {
        $sid = $p.PSChildName
        if ($sid -notmatch '^S-1-5-21-' -or $done[$sid]) { continue }
        $hive = Join-Path ([Environment]::ExpandEnvironmentVariables((Get-ItemProperty $p.PSPath).ProfileImagePath)) 'NTUSER.DAT'
        if (-not (Test-Path $hive)) { continue }
        & reg.exe load HKU\RK3576TMP "$hive" 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { continue }
        & reg.exe delete HKU\RK3576TMP\Software\RK3576 /f 2>&1 | Out-Null
        & reg.exe delete HKU\RK3576TMP\Software\Microsoft\Windows\CurrentVersion\Run /v $RunName /f 2>&1 | Out-Null
        [GC]::Collect()
        & reg.exe unload HKU\RK3576TMP 2>&1 | Out-Null
    }
    Say 'per-user settings removed'
}

function Start-AsUser {
    # explorer.exe starts it in the signed-in user's normal, not elevated, token
    $exe = Join-Path $Dir 'rk-monitor.exe'
    try { Start-Process -FilePath (Join-Path $env:SystemRoot 'explorer.exe') -ArgumentList "`"$exe`"" } catch { Say "start: $($_.Exception.Message)" }
}

if (-not (Test-Admin)) { Say 'needs to run as administrator (or SYSTEM)'; exit 1 }
Say "--- $Action"

switch ($Action) {
    'Install' {
        if (-not $Source -or -not (Test-Path (Join-Path $Source 'rk-monitor.exe'))) {
            Say "no rk-monitor.exe in '$Source'"; exit 1
        }
        $Source = (Resolve-Path $Source).Path
        Stop-Monitor
        if (Test-Path $Dir) { Remove-Item $Dir -Recurse -Force }
        Copy-Item -Path $Source -Destination $Dir -Recurse -Force
        Say "files: $Source -> $Dir"
        Complete-Setup
        if (-not $NoStart) { Start-AsUser }
        Say 'installed'
    }
    'FirstBoot' {
        if (-not (Test-Path (Join-Path $Dir 'rk-monitor.exe'))) { Say "no $Dir"; exit 1 }
        Complete-Setup
        Say 'first boot done'
    }
    'Uninstall' {
        Stop-Monitor
        Invoke-Counters @('-Remove')
        Invoke-BrowserGpu 'Uninstall'
        foreach ($n in $TaskName, $LogonTask) {
            Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $n -Confirm:$false -ErrorAction SilentlyContinue
        }
        try {
            $svc = New-Object -ComObject Schedule.Service
            $svc.Connect()
            $svc.GetFolder('\').DeleteFolder('RK3576', 0)
        } catch { }
        Remove-Item $Menu -Recurse -Force -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $RunKey -Name $RunName -ErrorAction SilentlyContinue
        Remove-Item $Base -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $Arp -Recurse -Force -ErrorAction SilentlyContinue
        Remove-UserState
        if (Test-Path $Dir) { Remove-Item $Dir -Recurse -Force }
        Say 'removed'
        # the log last, with its folder
        Remove-Item $LogDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
exit 0
