# rk3576-setup.ps1 - install, finish or remove the RK3576 tools
#
#   -Action Install -Source <dir>   copy <dir> (the bundle's RK3576 folder) to
#                                   %ProgramFiles%\RK3576, then everything below
#   -Action FirstBoot               what an offline install could not do: the
#                                   Start menu, the two tasks, the counters
#                                   (run as SYSTEM from SetupComplete.cmd)
#   -Action Uninstall               remove all of it again
#   -Action NpuAccess               let ordinary users open the NPU (below);
#                                   runs itself as SYSTEM when it has to
#   -Action NpuAccessRemove         take that back, if this script set it
#
# What an install leaves on the system, and nothing else:
#   %ProgramFiles%\RK3576\                          the files
#   Task Scheduler \RK3576\RK3576 Monitor           rk-monitor at every logon, any user, as
#                                                   that user, not elevated, no delay
#   HKLM\SOFTWARE\RK3576                            InstallDir, Version, Counters\ (state)
#   HKLM\SOFTWARE\RK3576\Monitor  Widget = 1        the desktop widget on by default
#                                  AutostartTask = 1 the logon task exists (rk-monitor's menu)
#   HKLM\...\Uninstall\RK3576Tools                  the entry in Settings, Apps
#   Start menu\Programs\RK3576\                     four shortcuts (all users)
#   machine Path  %ProgramFiles%\RK3576\npu\bin    the `charsiu` command (bin\charsiu.cmd)
#   Enum\ACPI\RKCP3576\*\Device Parameters        UserAccess = 1, only where it was not set,
#     and HKLM\SOFTWARE\RK3576 NpuUserAccess = 1    recording that this script set it
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
    [ValidateSet('Install', 'FirstBoot', 'Uninstall', 'NpuAccess', 'NpuAccessRemove')][string]$Action,
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
$EnvKey   = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
$NpuBin   = '%ProgramFiles%\RK3576\npu\bin'
$NpuEnum  = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576'
$Log = Join-Path $LogDir 'setup.log'
function Say([string]$m) { "$(Get-Date -Format s) $m" | Tee-Object -FilePath $Log -Append }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-System { return [Security.Principal.WindowsIdentity]::GetCurrent().IsSystem }

# ---- the charsiu command: npu\bin on the machine Path ------------------------
#
# npu\bin holds only charsiu.cmd, which runs npu\charsiu.ps1 (charsiu's
# front door from woa-rk3576-npu) with the execution policy bypassed for
# that process, so `charsiu` works in cmd and in PowerShell, elevated or not.
# The front door sets the NPU variables (CHARSIU_NPU and the rest) from
# [npu] in the user's config, with the board's defaults when there is none.
# The Path is read and written as REG_EXPAND_SZ, so the entry stays
# %ProgramFiles%\... and other entries' variables are not expanded.

# One Path entry the same folder as another: variables expanded, no
# trailing backslash, quotes off, any case.
function Get-PathKey([string]$e) {
    $x = [Environment]::ExpandEnvironmentVariables($e.Trim().Trim('"')).TrimEnd('\')
    return $x.ToLowerInvariant()
}

function Add-PathEntry([string]$PathValue, [string]$Entry) {
    $k = Get-PathKey $Entry
    $parts = @(($PathValue -split ';') | Where-Object { $_ -ne '' })
    if (@($parts | Where-Object { (Get-PathKey $_) -eq $k }).Count) { return $PathValue }
    return (@($parts) + $Entry) -join ';'
}

function Remove-PathEntry([string]$PathValue, [string]$Entry) {
    $k = Get-PathKey $Entry
    return (@(($PathValue -split ';') | Where-Object { $_ -ne '' -and (Get-PathKey $_) -ne $k })) -join ';'
}

function Get-MachinePath {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($EnvKey)
    try { return [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
    finally { $key.Close() }
}

function Set-MachinePath([string]$v) {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($EnvKey, $true)
    try { $key.SetValue('Path', $v, [Microsoft.Win32.RegistryValueKind]::ExpandString) } finally { $key.Close() }
    # Explorer reads the new Path, so the next console started from it has it
    try {
        if (-not ('Rk3576.Env' -as [type])) {
            Add-Type -Namespace Rk3576 -Name Env -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr h, uint m, System.UIntPtr w, string l, uint f, uint t, out System.UIntPtr r);
'@
        }
        $r = [UIntPtr]::Zero
        [void][Rk3576.Env]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$r)
    } catch { Say "  WM_SETTINGCHANGE: $($_.Exception.Message)" }
}

function Set-NpuPath {
    $old = Get-MachinePath
    $new = Add-PathEntry $old $NpuBin
    if ($new -ceq $old) { Say "Path: $NpuBin already there"; return }
    Set-MachinePath $new
    Say "Path: $NpuBin added (charsiu in any new console)"
}

function Remove-NpuPath {
    $old = Get-MachinePath
    $new = Remove-PathEntry $old $NpuBin
    if ($new -ceq $old) { return }
    Set-MachinePath $new
    Say "Path: $NpuBin removed"
}

# ---- who may open the NPU ----------------------------------------------------
#
# rknpu.sys lets only SYSTEM and administrators open \\.\RKNPU unless its
# Device Parameters\UserAccess is 1 (and the IOMMU is on), which it reads
# when the device starts, so it takes effect at the next restart. charsiu's
# own installer sets it by default as well (woa-rk3576-npu docs/charsiu.md,
# "Who may use the NPU", with what it means on a machine with several
# users). This sets it only where no value exists: a UserAccess = 0 that
# someone chose (charsiu-install.ps1 --admin-only) stays. The Enum keys are
# SYSTEM's to write, so an administrator runs this through a one-time task.

function Get-NpuParamKeys([string]$Enum = $NpuEnum) {
    if (-not (Test-Path $Enum)) { return @() }
    return @(Get-ChildItem -Path $Enum -ErrorAction SilentlyContinue | ForEach-Object { Join-Path $_.PSPath 'Device Parameters' })
}

# 'set', 'kept 0' or 'kept 1' per instance; no instance: nothing
function Set-NpuUserAccessIn([string]$Enum) {
    $out = @()
    foreach ($dp in Get-NpuParamKeys $Enum) {
        if (-not (Test-Path $dp)) { New-Item -Path $dp -Force | Out-Null }
        $v = (Get-ItemProperty -Path $dp -Name UserAccess -ErrorAction SilentlyContinue).UserAccess
        if ($null -ne $v) { $out += "kept $v"; continue }
        New-ItemProperty -Path $dp -Name UserAccess -PropertyType DWord -Value 1 -Force | Out-Null
        $out += 'set'
    }
    return $out
}

function Remove-NpuUserAccessIn([string]$Enum) {
    $n = 0
    foreach ($dp in Get-NpuParamKeys $Enum) {
        if ((Get-ItemProperty -Path $dp -Name UserAccess -ErrorAction SilentlyContinue).UserAccess -eq 1) {
            Remove-ItemProperty -Path $dp -Name UserAccess -ErrorAction SilentlyContinue
            $n++
        }
    }
    return $n
}

# This script, as SYSTEM, once: a task that is gone again when it returns.
function Invoke-SelfAsSystem([string]$What) {
    $name = "NPU access $PID"
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' `
           -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Action $What"
    $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    try {
        Register-ScheduledTask -TaskPath $TaskPath -TaskName $name -Action $a -Principal $p -Settings $s -Force | Out-Null
        Start-ScheduledTask -TaskPath $TaskPath -TaskName $name
        # 0x41303: has not run yet, 0x41301: running
        for ($i = 0; $i -lt 120; $i++) {
            Start-Sleep -Milliseconds 500
            $t = Get-ScheduledTaskInfo -TaskPath $TaskPath -TaskName $name -ErrorAction SilentlyContinue
            if (-not $t) { break }
            if ($t.LastTaskResult -ne 0x41303 -and $t.LastTaskResult -ne 0x41301) { break }
        }
    } catch { Say "  $What as SYSTEM: $($_.Exception.Message)" }
    finally { Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue }
}

function Set-NpuAccess {
    if (-not (Test-System)) { Invoke-SelfAsSystem 'NpuAccess'; return }
    $r = @(Set-NpuUserAccessIn $NpuEnum)
    if (-not $r.Count) { Say "NPU: no ACPI\RKCP3576 device (the firmware's NPU option is off); the counter task sets UserAccess once it is there"; return }
    if ($r -contains 'set') {
        New-Item -Path $Base -Force | Out-Null
        New-ItemProperty -Path $Base -Name NpuUserAccess -PropertyType DWord -Value 1 -Force | Out-Null
        Say 'NPU: UserAccess = 1, ordinary users may use it from the next restart'
    } else { Say "NPU: UserAccess left as it was ($($r -join ', '))" }
}

function Remove-NpuAccess {
    if ((Get-ItemProperty -Path $Base -Name NpuUserAccess -ErrorAction SilentlyContinue).NpuUserAccess -ne 1) { return }
    if (-not (Test-System)) { Invoke-SelfAsSystem 'NpuAccessRemove'; return }
    $n = Remove-NpuUserAccessIn $NpuEnum
    Remove-ItemProperty -Path $Base -Name NpuUserAccess -ErrorAction SilentlyContinue
    Say "NPU: UserAccess taken out again ($n)"
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
    New-Shortcut (Join-Path $Menu 'RK3576 tools folder.lnk') (Join-Path $env:SystemRoot 'explorer.exe') "`"$Dir`"" 'rknputest, charsiu and the scripts'
    New-Shortcut (Join-Path $Menu 'Charsiu chat.lnk') $env:ComSpec "/k `"$(Join-Path $Dir 'npu\bin\charsiu.cmd')`"" 'A conversation with a language model on the NPU (charsiu)'
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
    Set-NpuPath
    Set-NpuAccess
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

# dot-sourced (tests\charsiu-cli.tests.ps1): the functions only
if ($MyInvocation.InvocationName -eq '.') { return }

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
if (-not $Action) { Write-Output 'rk3576-setup.ps1 -Action Install|FirstBoot|Uninstall|NpuAccess|NpuAccessRemove'; exit 2 }
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
    'NpuAccess' { Set-NpuAccess }
    'NpuAccessRemove' { Remove-NpuAccess }
    'Uninstall' {
        Stop-Monitor
        Remove-NpuAccess
        Remove-NpuPath
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
