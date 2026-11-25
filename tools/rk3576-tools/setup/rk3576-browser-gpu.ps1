# rk3576-browser-gpu.ps1 - GPU rendering for Edge, Chrome and WebView2 on
# RK3576 boards whose Mali-G52 runs maliwddm at WDDM 2.x
#
# Why: at WDDM 2.x, Chromium's DirectComposition presents fail on maliwddm,
# its GPU process crashes three times and the browser falls back to software
# rendering. Started with --disable-direct-composition, Edge 158 and
# Chrome 158 (ARM64) render on the Mali through ANGLE D3D11 (FL 10_1) with
# no GPU process crash (board, 2026-11-25). The browser process reads the
# switch too (it keeps the window's redirection surface), so the driver
# cannot do this on its own, and neither browser has a policy for it.
# BROWSER-GPU.md has the full story.
#
#   -Action Status      what the gate says and which entries have the switch
#   -Action Apply       add the switch where it is missing, or take it out
#                       again when the switch is off or the gate says no
#                       (this is what the scheduled task runs)
#   -Action Revert      take everything out now (the task puts it back on
#                       its next run unless the mode is off: use Disable)
#   -Action Enable      mode auto (the default), then Apply
#   -Action Disable     mode off, then take everything out
#   -Action Install     copy this script to %ProgramFiles%\RK3576\browser-gpu,
#                       register the task \RK3576\Browser GPU, then Apply
#   -Action Uninstall   take everything out, remove the task, the copy and
#                       the settings
#   -DryRun             say what would change, change nothing
#   -Quiet              no output (the log still gets the changes)
#
# Where the switch goes (each entry only when its program is msedge.exe,
# chrome.exe or their _proxy.exe, and only when the switch is missing):
#   - shortcuts (.lnk): all users' Start menu and desktop, and each user's
#     Start menu, desktop and Quick Launch (User Pinned\TaskBar,
#     ImplicitAppShortcuts): the Arguments get it in front
#   - open commands: HKLM\SOFTWARE\Classes and each user's classes, for the
#     keys MSEdge*, microsoft-edge*, Chrome*, Chromium*,
#     Applications\msedge.exe and Applications\chrome.exe, and
#     Clients\StartMenuInternet\*: every shell\<verb>\command, right after the
#     program (before --single-argument, after which everything is the URL)
#   - Run values of the machine and of each user (Edge's startup boost
#     starts Edge from one)
#   - policies (each value's earlier state is kept and given back on revert):
#     Edge StartupBoostEnabled 0 and BackgroundModeEnabled 0, Chrome
#     BackgroundModeEnabled 0, so no browser instance that was started
#     without the switch is still running when a user opens a window (a
#     new window joins the running instance and its switches)
#   - WebView2: the machine environment variable
#     WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS and the registry fallback
#     HKLM\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments
#     "*", both appended to (WebView2 appends them to an app's own arguments)
#
# The gate (mode auto): ACPI\RKCP7402 is driven by maliwddm, has no problem
# code, and its Device Parameters\maliwddm\WddmVersion is 0x2000 or more.
# A clear no takes the switch out again; an answer that cannot be read
# changes nothing.
#
# Settings, HKLM\SOFTWARE\RK3576\BrowserGpu (all optional):
#   Mode      DWORD  0 off, 1 auto (default), 2 always (no gate)
#   Policies  DWORD  0: leave the Edge and Chrome policies alone
#   WebView2  DWORD  0: leave WebView2 alone
# and per user, HKCU\Software\RK3576\BrowserGpu Mode 0: not for this user.
#
# PowerShell 5.1, ASCII only. Dot-source it (". .\rk3576-browser-gpu.ps1")
# to get the functions without running anything; the tests do that.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [ValidateSet('Status', 'Apply', 'Revert', 'Enable', 'Disable', 'Install', 'Uninstall')]
    [string]$Action = 'Status',
    [switch]$DryRun,
    [switch]$Quiet
)

# Raise when anything here changes; Install keeps the newer of two copies.
$BgVersion = 1
$BgFlag = '--disable-direct-composition'
$BgBrowsers = @('msedge.exe', 'chrome.exe', 'msedge_proxy.exe', 'chrome_proxy.exe')
$BgClassPattern = '^(MSEdge|microsoft-edge|Chrome|Chromium)'
$BgEnvName = 'WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS'
$BgTaskPath = '\RK3576\'
$BgTaskName = 'Browser GPU'
$BgStateKey = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RK3576\BrowserGpu'
$BgUserStateSub = 'Software\RK3576\BrowserGpu'
$BgAbsent = '<absent>'

# ---------------------------------------------------------------------------
# Strings: arguments and command lines
# ---------------------------------------------------------------------------

# The part of a command's arguments that is still switches: everything
# before --single-argument (Chromium takes all after it as one URL) or a
# bare "--".
function Get-SwitchPart([string]$Rest) {
    if ($null -eq $Rest) { return '' }
    $m = [regex]::Match($Rest, '(^|\s)(--single-argument|--)(\s|$)')
    if ($m.Success) { return $Rest.Substring(0, $m.Index) }
    return $Rest
}

function Test-HasFlag([string]$Text) {
    if (-not $Text) { return $false }
    return [regex]::IsMatch($Text, '(^|\s)' + [regex]::Escape($BgFlag) + '(\s|$)')
}

# A shortcut's Arguments with the switch in front.
function Add-FlagToArgs([string]$Arguments) {
    if (Test-HasFlag (Get-SwitchPart $Arguments)) { return $Arguments }
    if (-not $Arguments) { return $BgFlag }
    return "$BgFlag $Arguments"
}

# The switch and the blanks after it taken out, wherever it is.
function Remove-FlagFromText([string]$Text) {
    if (-not (Test-HasFlag $Text)) { return $Text }
    $r = [regex]::Replace($Text, '(?<![^\s])' + [regex]::Escape($BgFlag) + '(?:\s+|$)', '')
    if ($r -ne $Text) { $r = $r.TrimEnd() }
    return $r
}

# A command line split into the program (as written, quotes included), its
# path, and the rest. $null when there is no program to find.
function Split-Command([string]$Command) {
    if (-not $Command) { return $null }
    $m = [regex]::Match($Command, '^(\s*"([^"]+)")(.*)$', 'Singleline')
    if (-not $m.Success) { $m = [regex]::Match($Command, '^(\s*((?:[^"\s]|\s(?!-))+?\.exe))(?=\s|$)(.*)$', 'Singleline,IgnoreCase') }
    if (-not $m.Success) { return $null }
    return [pscustomobject]@{ Head = $m.Groups[1].Value; Exe = $m.Groups[2].Value; Rest = $m.Groups[3].Value }
}

function Test-BrowserExe([string]$Path) {
    if (-not $Path) { return $false }
    $leaf = ($Path.Trim().Trim('"') -split '[\\/]')[-1]
    return ($BgBrowsers -contains $leaf.ToLowerInvariant())
}

# A browser's command line with the switch right after the program; any
# other command line unchanged.
function Add-FlagToCommand([string]$Command) {
    $c = Split-Command $Command
    if (-not $c -or -not (Test-BrowserExe $c.Exe)) { return $Command }
    if (Test-HasFlag (Get-SwitchPart $c.Rest)) { return $Command }
    return $c.Head + ' ' + $BgFlag + $c.Rest
}

function Remove-FlagFromCommand([string]$Command) {
    $c = Split-Command $Command
    if (-not $c -or -not (Test-BrowserExe $c.Exe)) { return $Command }
    if (-not (Test-HasFlag $c.Rest)) { return $Command }
    $rest = [regex]::Replace($c.Rest, '\s+' + [regex]::Escape($BgFlag) + '(?=\s|$)', '')
    return $c.Head + $rest
}

# A list value such as WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS with the switch at the end.
function Add-FlagToList([string]$Value) {
    if (Test-HasFlag $Value) { return $Value }
    if (-not $Value -or -not $Value.Trim()) { return $BgFlag }
    return $Value.TrimEnd() + ' ' + $BgFlag
}

# ---------------------------------------------------------------------------
# Registry through .NET (fast, keeps value kinds, no expansion)
# ---------------------------------------------------------------------------

function Open-RegKey([string]$Path, [bool]$Writable = $false, [bool]$Create = $false) {
    $p = $Path -replace '^Registry::', '' -replace '^HKLM:\\?', 'HKEY_LOCAL_MACHINE\' -replace '^HKCU:\\?', 'HKEY_CURRENT_USER\'
    $i = $p.IndexOf('\')
    if ($i -lt 0) { $hive = $p; $sub = '' } else { $hive = $p.Substring(0, $i); $sub = $p.Substring($i + 1) }
    switch ($hive.ToUpperInvariant()) {
        'HKEY_LOCAL_MACHINE' { $root = [Microsoft.Win32.Registry]::LocalMachine }
        'HKEY_CURRENT_USER' { $root = [Microsoft.Win32.Registry]::CurrentUser }
        'HKEY_USERS' { $root = [Microsoft.Win32.Registry]::Users }
        'HKEY_CLASSES_ROOT' { $root = [Microsoft.Win32.Registry]::ClassesRoot }
        default { return $null }
    }
    if (-not $sub) { return $root }
    try {
        if ($Create) { return $root.CreateSubKey($sub) }
        return $root.OpenSubKey($sub, $Writable)
    } catch { return $null }
}

function Get-RegValueRaw($Key, [string]$Name) {
    return $Key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
}

function New-Change([string]$Kind, [string]$Where, $Before, $After) {
    return [pscustomobject]@{ Kind = $Kind; Where = $Where; Before = $Before; After = $After }
}

# Every shell\<verb>\command under the browser keys of one classes root.
function Sync-ClassCommands([string]$ClassesRoot, [bool]$Add, [bool]$DryRun) {
    $out = @()
    $root = Open-RegKey $ClassesRoot
    if (-not $root) { return $out }
    try {
        $names = @($root.GetSubKeyNames() | Where-Object { $_ -match $BgClassPattern })
        $app = Open-RegKey "$ClassesRoot\Applications"
        if ($app) {
            foreach ($a in $app.GetSubKeyNames()) { if (Test-BrowserExe $a) { $names += "Applications\$a" } }
            $app.Close()
        }
    } finally { if ($root.Name -match '\\') { $root.Close() } }
    foreach ($n in $names) { $out += Sync-ShellCommands "$ClassesRoot\$n" $Add $DryRun }
    return $out
}

# Clients\StartMenuInternet\<browser>\shell\<verb>\command.
function Sync-StartMenuInternet([string]$ClientsRoot, [bool]$Add, [bool]$DryRun) {
    $out = @()
    $root = Open-RegKey $ClientsRoot
    if (-not $root) { return $out }
    $names = $root.GetSubKeyNames()
    $root.Close()
    foreach ($n in $names) { $out += Sync-ShellCommands "$ClientsRoot\$n" $Add $DryRun }
    return $out
}

function Sync-ShellCommands([string]$KeyPath, [bool]$Add, [bool]$DryRun) {
    $out = @()
    $shell = Open-RegKey "$KeyPath\shell"
    if (-not $shell) { return $out }
    $verbs = $shell.GetSubKeyNames()
    $shell.Close()
    foreach ($v in $verbs) {
        $path = "$KeyPath\shell\$v\command"
        $k = Open-RegKey $path
        if (-not $k) { continue }
        $cur = Get-RegValueRaw $k ''
        $kind = $null
        if ($null -ne $cur) { $kind = $k.GetValueKind('') }
        $k.Close()
        if ($cur -isnot [string]) { continue }
        if ($Add) { $new = Add-FlagToCommand $cur } else { $new = Remove-FlagFromCommand $cur }
        if ($new -ceq $cur) { continue }
        if (-not $DryRun) {
            $w = Open-RegKey $path $true
            if (-not $w) { $out += New-Change 'error' $path $cur 'not writable'; continue }
            try { $w.SetValue('', $new, $kind) } catch { $out += New-Change 'error' $path $cur $_.Exception.Message; continue } finally { $w.Close() }
        }
        $out += New-Change 'command' $path $cur $new
    }
    return $out
}

# Run values that start a browser.
function Sync-RunValues([string]$RunKey, [bool]$Add, [bool]$DryRun) {
    $out = @()
    $k = Open-RegKey $RunKey
    if (-not $k) { return $out }
    $vals = @()
    foreach ($n in $k.GetValueNames()) {
        $cur = Get-RegValueRaw $k $n
        if ($cur -is [string]) { $vals += , @($n, $cur, $k.GetValueKind($n)) }
    }
    $k.Close()
    foreach ($v in $vals) {
        if ($Add) { $new = Add-FlagToCommand $v[1] } else { $new = Remove-FlagFromCommand $v[1] }
        if ($new -ceq $v[1]) { continue }
        if (-not $DryRun) {
            $w = Open-RegKey $RunKey $true
            try { $w.SetValue($v[0], $new, $v[2]) } catch { $out += New-Change 'error' "$RunKey\$($v[0])" $v[1] $_.Exception.Message; continue } finally { if ($w) { $w.Close() } }
        }
        $out += New-Change 'run' "$RunKey\$($v[0])" $v[1] $new
    }
    return $out
}

# A list value (the WebView2 environment variable, its registry fallback):
# the switch appended, or taken out (the value deleted when nothing is left).
function Sync-ListValue([string]$KeyPath, [string]$Name, [bool]$Add, [bool]$DryRun, [string]$Kind, [bool]$IsEnvironment) {
    $k = Open-RegKey $KeyPath
    $cur = $null
    $vk = [Microsoft.Win32.RegistryValueKind]::String
    if ($k) {
        $cur = Get-RegValueRaw $k $Name
        if ($null -ne $cur) { $vk = $k.GetValueKind($Name) }
        $k.Close()
    }
    if ($cur -isnot [string]) { $cur = $null }
    if ($Add) { $new = Add-FlagToList $cur } else { $new = Remove-FlagFromText $cur }
    if (-not $new -or -not $new.Trim()) { $new = $null }
    if ($new -ceq $cur) { return @() }
    if (-not $DryRun) {
        if ($IsEnvironment) {
            # Writes the machine environment and tells running programs (WM_SETTINGCHANGE).
            [Environment]::SetEnvironmentVariable($Name, $new, 'Machine')
        } else {
            $w = Open-RegKey $KeyPath $true $true
            try { if ($null -eq $new) { $w.DeleteValue($Name, $false) } else { $w.SetValue($Name, $new, $vk) } } finally { $w.Close() }
        }
    }
    return , (New-Change $Kind "$KeyPath\$Name" $cur $new)
}

# A policy DWORD set to $Want; its earlier state goes into the state key
# first (once), and Remove gives it back, but only while the value is still
# ours (an administrator's later change is left alone).
function Sync-PolicyDword([string]$KeyPath, [string]$Name, [int]$Want, [string]$StateKey, [bool]$Add, [bool]$DryRun) {
    $saveName = ($KeyPath -replace '^Registry::', '') + '|' + $Name
    $k = Open-RegKey $KeyPath
    $cur = $null
    if ($k) { $cur = $k.GetValue($Name, $null); $k.Close() }
    $sk = Open-RegKey "$StateKey\Saved"
    $saved = $null
    if ($sk) { $saved = $sk.GetValue($saveName, $null); $sk.Close() }
    if ($Add) {
        if ($cur -is [int] -and $cur -eq $Want) { return @() }
        if (-not $DryRun) {
            if ($null -eq $saved) {
                $s = Open-RegKey "$StateKey\Saved" $true $true
                if ($null -eq $cur) { $s.SetValue($saveName, $BgAbsent) } else { $s.SetValue($saveName, [string]$cur) }
                $s.Close()
            }
            $w = Open-RegKey $KeyPath $true $true
            $w.SetValue($Name, $Want, [Microsoft.Win32.RegistryValueKind]::DWord)
            $w.Close()
        }
        return , (New-Change 'policy' "$KeyPath\$Name" $cur $Want)
    }
    if ($null -eq $saved) { return @() }
    $out = @()
    if ($cur -is [int] -and $cur -eq $Want) {
        if (-not $DryRun) {
            $w = Open-RegKey $KeyPath $true
            if ($w) {
                if ($saved -eq $BgAbsent) { $w.DeleteValue($Name, $false) } else { $w.SetValue($Name, [int]$saved, [Microsoft.Win32.RegistryValueKind]::DWord) }
                $w.Close()
            }
        }
        $after = $saved
        if ($saved -eq $BgAbsent) { $after = $null }
        $out += New-Change 'policy' "$KeyPath\$Name" $cur $after
    }
    if (-not $DryRun) {
        $s = Open-RegKey "$StateKey\Saved" $true
        if ($s) { $s.DeleteValue($saveName, $false); $s.Close() }
    }
    return $out
}

# ---------------------------------------------------------------------------
# Shortcuts
# ---------------------------------------------------------------------------

# Cheap first look: a .lnk that names neither browser anywhere is skipped
# without asking the shell to load it.
function Test-LnkMentionsBrowser([string]$Path) {
    try { $b = [IO.File]::ReadAllBytes($Path) } catch { return $false }
    $a = [Text.Encoding]::GetEncoding(28591).GetString($b)
    $u = [Text.Encoding]::Unicode.GetString($b)
    $re = '(?i)(msedge|chrome)(_proxy)?\.exe'
    return ([regex]::IsMatch($a, $re) -or [regex]::IsMatch($u, $re))
}

function Sync-Shortcuts([string[]]$Dirs, [bool]$Add, [bool]$DryRun) {
    $out = @()
    $sh = $null
    foreach ($d in $Dirs) {
        if (-not $d -or -not (Test-Path -LiteralPath $d)) { continue }
        foreach ($f in Get-ChildItem -LiteralPath $d -Recurse -Filter '*.lnk' -File -Force -ErrorAction SilentlyContinue) {
            if (-not (Test-LnkMentionsBrowser $f.FullName)) { continue }
            if (-not $sh) { $sh = New-Object -ComObject WScript.Shell }
            try {
                $s = $sh.CreateShortcut($f.FullName)
                if (-not (Test-BrowserExe $s.TargetPath)) { continue }
                $cur = [string]$s.Arguments
                if ($Add) { $new = Add-FlagToArgs $cur } else { $new = Remove-FlagFromText $cur }
                if ($new -ceq $cur) { continue }
                if (-not $DryRun) {
                    $ro = $f.IsReadOnly
                    if ($ro) { $f.IsReadOnly = $false }
                    $s.Arguments = $new
                    $s.Save()
                    if ($ro) { $f.IsReadOnly = $true }
                }
                $out += New-Change 'shortcut' $f.FullName $cur $new
            } catch {
                $out += New-Change 'error' $f.FullName $null $_.Exception.Message
            }
        }
    }
    if ($sh) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($sh) }
    return $out
}

# ---------------------------------------------------------------------------
# The gate
# ---------------------------------------------------------------------------

function Get-DeviceProblemCode([string]$InstanceId) {
    $id = $InstanceId -replace '\\', '\\'
    $d = Get-CimInstance -ClassName Win32_PnPEntity -Filter "DeviceID='$id'" -ErrorAction Stop
    if (-not $d) { return -1 }
    return [int]$d.ConfigManagerErrorCode
}

# Yes, No or Unknown, and why. $ProblemCode maps an instance ID to its
# Device Manager problem code (0 = working); the tests pass their own.
function Get-GpuGate([string]$EnumKey = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402',
                     [scriptblock]$ProblemCode = { param($i) Get-DeviceProblemCode $i },
                     [string]$InstancePrefix = 'ACPI\RKCP7402') {
    $root = Open-RegKey $EnumKey
    if (-not $root) { return [pscustomobject]@{ State = 'No'; Reason = 'no ACPI\RKCP7402 device: not an RK3576 board with the Mali-G52 in ACPI' } }
    $insts = $root.GetSubKeyNames()
    $root.Close()
    $reasons = @()
    $unknown = $false
    foreach ($i in $insts) {
        $k = Open-RegKey "$EnumKey\$i"
        if (-not $k) { continue }
        $svc = [string]$k.GetValue('Service', '')
        $k.Close()
        if ($svc -ne 'maliwddm') { $reasons += "$InstancePrefix\$i is driven by '$svc', not maliwddm"; continue }
        try { $code = & $ProblemCode "$InstancePrefix\$i" } catch { $unknown = $true; $reasons += "$InstancePrefix\$i state unreadable: $($_.Exception.Message)"; continue }
        if ($code -ne 0) { $reasons += "$InstancePrefix\$i has problem code $code (maliwddm not running)"; continue }
        $p = Open-RegKey "$EnumKey\$i\Device Parameters\maliwddm"
        $ver = $null
        if ($p) { $ver = $p.GetValue('WddmVersion', $null); $p.Close() }
        if ($ver -isnot [int]) { $unknown = $true; $reasons += "$InstancePrefix\$i has no WddmVersion yet"; continue }
        if ($ver -ge 0x2000) { return [pscustomobject]@{ State = 'Yes'; Reason = ('maliwddm runs {0} at WDDM {1}.{2}' -f "$InstancePrefix\$i", (($ver -shr 12) -band 0xF), (($ver -shr 8) -band 0xF)) } }
        $reasons += ('{0} runs WDDM {1}.{2} (Edge and Chrome need no switch there)' -f "$InstancePrefix\$i", (($ver -shr 12) -band 0xF), (($ver -shr 8) -band 0xF))
    }
    if ($unknown) { return [pscustomobject]@{ State = 'Unknown'; Reason = ($reasons -join '; ') } }
    if (-not $reasons) { $reasons = @('no ACPI\RKCP7402 instance') }
    return [pscustomobject]@{ State = 'No'; Reason = ($reasons -join '; ') }
}

# ---------------------------------------------------------------------------
# What to touch: the context
# ---------------------------------------------------------------------------

function Get-RegDword([string]$KeyPath, [string]$Name, [int]$Default) {
    $k = Open-RegKey $KeyPath
    if (-not $k) { return $Default }
    $v = $k.GetValue($Name, $null)
    $k.Close()
    if ($v -is [int]) { return $v }
    return $Default
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# A user's folders with shortcuts, from the profile path and, for a loaded
# hive, the Desktop the user's shell folders name (it may be redirected).
function Get-UserShortcutDirs([string]$ProfilePath, [string]$UserRoot) {
    $appdata = Join-Path $ProfilePath 'AppData\Roaming'
    $dirs = @(
        (Join-Path $appdata 'Microsoft\Windows\Start Menu\Programs'),
        (Join-Path $appdata 'Microsoft\Internet Explorer\Quick Launch'),
        (Join-Path $ProfilePath 'Desktop')
    )
    if ($UserRoot) {
        $k = Open-RegKey "$UserRoot\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
        if ($k) {
            $desk = Get-RegValueRaw $k 'Desktop'
            $k.Close()
            if ($desk -is [string] -and $desk) {
                $desk = $desk -replace '(?i)%USERPROFILE%', $ProfilePath
                if ($desk -notmatch '%') { $dirs += $desk }
            }
        }
    }
    return ($dirs | Select-Object -Unique)
}

# The real system: the machine, and every user with a profile. A user
# whose hive is not loaded gets the shortcuts only; the registry part
# follows at that user's next logon (the task's logon trigger), or, for
# Uninstall, through a temporarily loaded hive.
function Get-SystemContext([bool]$LoadUnloadedHives = $false) {
    $admin = Test-IsAdmin
    $users = @()
    if ($admin) {
        $loaded = @{}
        foreach ($n in [Microsoft.Win32.Registry]::Users.GetSubKeyNames()) { if ($n -match '^S-1-5-21-[\d-]+$') { $loaded[$n] = $true } }
        $plist = Open-RegKey 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
        $sids = @()
        if ($plist) { $sids = $plist.GetSubKeyNames(); $plist.Close() }
        foreach ($sid in $sids) {
            if ($sid -notmatch '^S-1-5-21-[\d-]+$') { continue }
            $pk = Open-RegKey "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid"
            if (-not $pk) { continue }
            $pp = [Environment]::ExpandEnvironmentVariables([string]$pk.GetValue('ProfileImagePath', ''))
            $pk.Close()
            if (-not $pp -or -not (Test-Path -LiteralPath $pp)) { continue }
            $u = [pscustomobject]@{ Name = $sid; Profile = $pp; Root = $null; Classes = $null; ShortcutDirs = @(); Temp = @() }
            if ($loaded[$sid]) {
                $u.Root = "Registry::HKEY_USERS\$sid"
                $u.Classes = "Registry::HKEY_USERS\${sid}_Classes"
            } elseif ($LoadUnloadedHives) {
                $tag = 'RK3576BG_' + ($sid -split '-')[-1]
                $nt = Join-Path $pp 'NTUSER.DAT'
                $uc = Join-Path $pp 'AppData\Local\Microsoft\Windows\UsrClass.dat'
                if (Test-Path -LiteralPath $nt) {
                    & reg.exe load "HKU\$tag" "$nt" 2>&1 | Out-Null
                    if ($LASTEXITCODE -eq 0) { $u.Root = "Registry::HKEY_USERS\$tag"; $u.Temp += "HKU\$tag" }
                }
                if (Test-Path -LiteralPath $uc) {
                    & reg.exe load "HKU\${tag}_C" "$uc" 2>&1 | Out-Null
                    if ($LASTEXITCODE -eq 0) { $u.Classes = "Registry::HKEY_USERS\${tag}_C"; $u.Temp += "HKU\${tag}_C" }
                }
            }
            $u.ShortcutDirs = Get-UserShortcutDirs $pp $u.Root
            $users += $u
        }
    } else {
        $users += [pscustomobject]@{ Name = $env:USERNAME; Profile = $env:USERPROFILE; Root = 'Registry::HKEY_CURRENT_USER'; Classes = 'Registry::HKEY_CURRENT_USER\Software\Classes'
            ShortcutDirs = (Get-UserShortcutDirs $env:USERPROFILE 'Registry::HKEY_CURRENT_USER'); Temp = @() }
    }
    $machine = $null
    $public = $env:PUBLIC
    if (-not $public) { $public = Join-Path $env:SystemDrive 'Users\Public' }
    if ($admin) {
        $machine = [pscustomobject]@{
            Classes = @('Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes')
            Clients = @('Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Clients\StartMenuInternet', 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Clients\StartMenuInternet')
            Run = @('Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Run', 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run')
            ShortcutDirs = @((Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'), (Join-Path $public 'Desktop'))
            PolicyRoot = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Policies'
            EnvKey = 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
            RealEnvironment = $true
        }
    }
    return [pscustomobject]@{ Machine = $machine; Users = $users; StateKey = $BgStateKey }
}

function Close-Context($Context) {
    foreach ($u in $Context.Users) {
        foreach ($h in $u.Temp) {
            [GC]::Collect()
            [GC]::WaitForPendingFinalizers()
            & reg.exe unload $h 2>&1 | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# The work
# ---------------------------------------------------------------------------

# $Add: put the switch in (true) or take it out (false). A user's own
# Mode 0 (HKCU\Software\RK3576\BrowserGpu) takes it out for that user.
function Invoke-BrowserGpuSync($Context, [bool]$Add, [bool]$DryRun, [bool]$Policies = $true, [bool]$WebView2 = $true) {
    $changes = @()
    $m = $Context.Machine
    if ($m) {
        foreach ($c in $m.Classes) { $changes += Sync-ClassCommands $c $Add $DryRun }
        foreach ($c in $m.Clients) { $changes += Sync-StartMenuInternet $c $Add $DryRun }
        foreach ($r in $m.Run) { $changes += Sync-RunValues $r $Add $DryRun }
        $changes += Sync-Shortcuts $m.ShortcutDirs $Add $DryRun
        $polAdd = $Add -and $Policies
        $changes += Sync-PolicyDword "$($m.PolicyRoot)\Microsoft\Edge" 'StartupBoostEnabled' 0 $Context.StateKey $polAdd $DryRun
        $changes += Sync-PolicyDword "$($m.PolicyRoot)\Microsoft\Edge" 'BackgroundModeEnabled' 0 $Context.StateKey $polAdd $DryRun
        $changes += Sync-PolicyDword "$($m.PolicyRoot)\Google\Chrome" 'BackgroundModeEnabled' 0 $Context.StateKey $polAdd $DryRun
        $wvAdd = $Add -and $WebView2
        $changes += Sync-ListValue $m.EnvKey $BgEnvName $wvAdd $DryRun 'environment' ([bool]$m.RealEnvironment)
        $changes += Sync-ListValue "$($m.PolicyRoot)\Microsoft\Edge\WebView2\AdditionalBrowserArguments" '*' $wvAdd $DryRun 'webview2' $false
    }
    foreach ($u in $Context.Users) {
        $ua = $Add
        if ($ua -and $u.Root -and (Get-RegDword "$($u.Root)\$BgUserStateSub" 'Mode' 1) -eq 0) { $ua = $false }
        if ($u.Classes) { $changes += Sync-ClassCommands $u.Classes $ua $DryRun }
        if ($u.Root) {
            $changes += Sync-StartMenuInternet "$($u.Root)\Software\Clients\StartMenuInternet" $ua $DryRun
            $changes += Sync-RunValues "$($u.Root)\Software\Microsoft\Windows\CurrentVersion\Run" $ua $DryRun
        }
        $changes += Sync-Shortcuts $u.ShortcutDirs $ua $DryRun
    }
    return , @($changes | Where-Object { $_ })
}

# What the settings and the gate say: $true add, $false take out, $null leave alone.
function Get-Decision([int]$Mode, $Gate) {
    if ($Mode -eq 0) { return $false }
    if ($Mode -eq 2) { return $true }
    if ($Gate.State -eq 'Yes') { return $true }
    if ($Gate.State -eq 'No') { return $false }
    return $null
}

function Get-TaskXml([string]$ScriptPath) {
    $esc = [Security.SecurityElement]::Escape($ScriptPath)
    $q = "<QueryList><Query Id=`"0`" Path=`"Application`"><Select Path=`"Application`">*[System[Provider[@Name='MsiInstaller'] and (EventID=1033 or EventID=1034 or EventID=11707 or EventID=11724 or EventID=11728)]]</Select>" +
         "<Select Path=`"Application`">*[System[Provider[@Name='edgeupdate' or @Name='gupdate' or @Name='GoogleUpdater']]]</Select></Query></QueryList>"
    $qe = [Security.SecurityElement]::Escape($q)
    return @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Author>woa-rk3576</Author>
    <Description>Keeps --disable-direct-composition on the Edge, Chrome and WebView2 launch paths while the RK3576 Mali-G52 runs maliwddm at WDDM 2.x, so they render on the GPU. Settings: HKLM\SOFTWARE\RK3576\BrowserGpu (Mode 0 turns it off).</Description>
  </RegistrationInfo>
  <Triggers>
    <BootTrigger><Enabled>true</Enabled><Delay>PT1M</Delay></BootTrigger>
    <LogonTrigger><Enabled>true</Enabled><Delay>PT30S</Delay></LogonTrigger>
    <TimeTrigger>
      <Repetition><Interval>PT1H</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition>
      <StartBoundary>2026-01-01T00:17:00</StartBoundary>
      <Enabled>true</Enabled>
    </TimeTrigger>
    <EventTrigger><Enabled>true</Enabled><Delay>PT2M</Delay><Subscription>$qe</Subscription></EventTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>true</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>PT10M</ExecutionTimeLimit>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>powershell.exe</Command>
      <Arguments>-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "$esc" -Action Apply -Quiet</Arguments>
    </Exec>
  </Actions>
</Task>
"@
}

function Get-ScriptVersion([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return -1 }
    $m = Select-String -LiteralPath $Path -Pattern '^\$BgVersion = (\d+)' -List
    if ($m) { return [int]$m.Matches[0].Groups[1].Value }
    return 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

$script:BgQuiet = $false
$script:BgLog = $null
function Say([string]$m) {
    if (-not $script:BgQuiet) { Write-Host $m }
    if ($script:BgLog) {
        try {
            $f = Get-Item -LiteralPath $script:BgLog -ErrorAction SilentlyContinue
            if ($f -and $f.Length -gt 262144) { Move-Item -LiteralPath $script:BgLog -Destination "$($script:BgLog).old" -Force }
            Add-Content -LiteralPath $script:BgLog -Value "$(Get-Date -Format s) $m" -Encoding ascii
        } catch { }
    }
}

function Show-Changes($Changes, [bool]$DryRun) {
    $verb = 'changed'
    if ($DryRun) { $verb = 'would change' }
    foreach ($c in $Changes) {
        if ($c.Kind -eq 'error') { Say "  error  $($c.Where): $($c.After)"; continue }
        Say "  $verb $($c.Kind)  $($c.Where)"
        Say "      was: $($c.Before)"
        Say "      now: $($c.After)"
    }
}

function Set-State([string]$Name, $Value) {
    $k = Open-RegKey $BgStateKey $true $true
    if (-not $k) { return }
    if ($Value -is [int]) { $k.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::DWord) } else { $k.SetValue($Name, [string]$Value) }
    $k.Close()
}

function Invoke-Main {
    $script:BgQuiet = [bool]$Quiet
    $admin = Test-IsAdmin
    if ($admin -and -not $DryRun) {
        $dir = Join-Path $env:ProgramData 'RK3576'
        try { New-Item -ItemType Directory -Force -Path $dir | Out-Null; $script:BgLog = Join-Path $dir 'browser-gpu.log' } catch { }
    }
    if (($Action -in 'Install', 'Uninstall', 'Enable', 'Disable') -and -not $admin) {
        Say "$Action needs an administrator (or SYSTEM)."
        return 1
    }
    $installDir = Join-Path $env:ProgramFiles 'RK3576\browser-gpu'
    $installed = Join-Path $installDir 'rk3576-browser-gpu.ps1'
    $self = $PSCommandPath

    switch ($Action) {
        'Enable' {
            if (-not $DryRun) { $k = Open-RegKey $BgStateKey $true $true; $k.DeleteValue('Mode', $false); $k.Close() }
            Say 'mode: auto'
        }
        'Disable' {
            if (-not $DryRun) { Set-State 'Mode' 0 }
            Say 'mode: off'
        }
        'Install' {
            $have = Get-ScriptVersion $installed
            if ($self -and ((Resolve-Path -LiteralPath $self).Path -ne $installed) -and $have -le $BgVersion) {
                if ($DryRun) { Say "would copy $self to $installed" } else {
                    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
                    Copy-Item -LiteralPath $self -Destination $installed -Force
                    Say "copied to $installed (version $BgVersion)"
                }
            } elseif ($have -gt $BgVersion) {
                Say "kept $installed (version $have, newer than this $BgVersion)"
            }
            if ($DryRun) { Say "would register the task $BgTaskPath$BgTaskName" } else {
                Register-ScheduledTask -TaskPath $BgTaskPath -TaskName $BgTaskName -Xml (Get-TaskXml $installed) -Force | Out-Null
                Set-State 'Version' $BgVersion
                Say "task: $BgTaskPath$BgTaskName"
            }
            if ($have -gt $BgVersion -and -not $DryRun) {
                $qa = @()
                if ($Quiet) { $qa = @('-Quiet') }
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installed -Action Apply @qa | Out-Host
                return 0
            }
        }
    }

    $mode = Get-RegDword $BgStateKey 'Mode' 1
    $policies = (Get-RegDword $BgStateKey 'Policies' 1) -ne 0
    $webview2 = (Get-RegDword $BgStateKey 'WebView2' 1) -ne 0
    $gate = Get-GpuGate
    $modeName = @{ 0 = 'off'; 1 = 'auto'; 2 = 'always' }[$mode]
    if (-not $modeName) { $modeName = "unknown ($mode), treated as auto"; $mode = 1 }
    if ($Action -eq 'Status') {
        Say "rk3576-browser-gpu $BgVersion, mode $modeName, gate $($gate.State): $($gate.Reason)"
        $ctx = Get-SystemContext
        if (-not $ctx.Machine) { Say '(not an administrator: only your own shortcuts and classes are looked at)' }
        $missing = Invoke-BrowserGpuSync $ctx $true $true $policies $webview2
        $present = Invoke-BrowserGpuSync $ctx $false $true $policies $webview2
        Say "entries with the switch: $(@($present | Where-Object { $_.Kind -ne 'policy' }).Count); entries without it: $(@($missing | Where-Object { $_.Kind -ne 'policy' }).Count)"
        foreach ($c in $present) { if ($c.Kind -ne 'error') { Say "  has  $($c.Kind)  $($c.Where)" } }
        foreach ($c in $missing) { if ($c.Kind -ne 'error') { Say "  lacks $($c.Kind)  $($c.Where)" } }
        return 0
    }

    $add = $null
    switch ($Action) {
        'Apply' { $add = Get-Decision $mode $gate }
        'Enable' { $add = Get-Decision 1 $gate }
        'Install' { $add = Get-Decision $mode $gate }
        default { $add = $false }   # Revert, Disable, Uninstall
    }
    $last = $null
    $k = Open-RegKey $BgStateKey
    if ($k) { $last = $k.GetValue('LastGate', $null); $k.Close() }
    $gateLine = "mode $modeName, gate $($gate.State): $($gate.Reason)"
    if ($null -eq $add) {
        if ($last -ne $gateLine) { Say "$Action`: $gateLine; nothing changed" }
        if (-not $DryRun) { Set-State 'LastGate' $gateLine; Set-State 'LastRun' (Get-Date -Format s) }
        return 0
    }
    $ctx = Get-SystemContext ($Action -eq 'Uninstall')
    try {
        $changes = Invoke-BrowserGpuSync $ctx $add ([bool]$DryRun) $policies $webview2
    } finally { Close-Context $ctx }
    $what = 'take out'
    if ($add) { $what = 'add' }
    if ($changes.Count -gt 0 -or $last -ne $gateLine -or $Action -ne 'Apply') {
        Say "$Action ($what $BgFlag): $gateLine; $($changes.Count) change(s)"
        Show-Changes $changes ([bool]$DryRun)
    }
    if (-not $DryRun -and $Action -ne 'Uninstall') {
        Set-State 'LastGate' $gateLine
        Set-State 'LastRun' (Get-Date -Format s)
        Set-State 'Applied' ([int][bool]$add)
    }
    if ($Action -eq 'Uninstall') {
        if ($DryRun) { Say "would remove the task $BgTaskPath$BgTaskName, $installDir and $BgStateKey" } else {
            Unregister-ScheduledTask -TaskPath $BgTaskPath -TaskName $BgTaskName -Confirm:$false -ErrorAction SilentlyContinue
            try {
                $svc = New-Object -ComObject Schedule.Service
                $svc.Connect()
                $f = $svc.GetFolder($BgTaskPath.TrimEnd('\'))
                if ($f.GetTasks(1).Count -eq 0 -and $f.GetFolders(0).Count -eq 0) { $svc.GetFolder('\').DeleteFolder('RK3576', 0) }
            } catch { }
            if (Test-Path -LiteralPath $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
            $p = Open-RegKey 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RK3576' $true
            if ($p) { $p.DeleteSubKeyTree('BrowserGpu', $false); $p.Close() }
            Say 'uninstalled'
        }
    }
    return 0
}

if ($MyInvocation.InvocationName -ne '.') {
    $rc = Invoke-Main
    exit $rc
}
