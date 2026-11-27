# rk3576-browser-gpu.ps1 - take --disable-direct-composition back out of
# Edge, Chrome and WebView2 on RK3576 boards
#
# Why: version 1 of this script added that switch so Edge and Chrome would
# render on the Mali-G52 at WDDM 2.x. It never helped. With the switch,
# Chromium presents through ANGLE's blt-model swap chain: on every maliwddm
# before 2df1808 those presents never reached the screen (Edge was a white
# window), and on the fixed driver they arrive through an extra copy (32 fps
# at 1440p). Without the switch, DirectComposition runs at 60 fps at 1440p
# on the fixed driver, and an older driver falls back to software rendering,
# which is usable (TaishanPi 3M and CM5-IO, 2026-11-27; wddm-mali-g52
# docs/EDGE-PRESENT.md). So this version only removes what version 1 added,
# and never adds anything. BROWSER-GPU.md has the details.
#
#   -Action Status      list what an earlier version left behind
#   -Action Apply       take it out (what version 1's scheduled task runs)
#   -Action Revert      the same as Apply
#   -Action Enable      the same as Apply (kept for old callers)
#   -Action Disable     the same as Apply (kept for old callers)
#   -Action Install     take it out, also for users who are not signed in,
#                       and remove version 1's task \RK3576\Browser GPU, its
#                       copy in %ProgramFiles%\RK3576\browser-gpu and its
#                       settings HKLM\SOFTWARE\RK3576\BrowserGpu
#   -Action Uninstall   the same as Install
#   -DryRun             say what would change, change nothing
#   -Quiet              no output (the log still gets the changes)
#
# What is taken out (only from entries whose program is msedge.exe,
# chrome.exe or their _proxy.exe):
#   - the switch in shortcuts (.lnk): all users' Start menu and desktop, and
#     each user's Start menu, desktop and Quick Launch (User Pinned\TaskBar,
#     ImplicitAppShortcuts)
#   - the switch in open commands: HKLM\SOFTWARE\Classes and each user's
#     classes, for the keys MSEdge*, microsoft-edge*, Chrome*, Chromium*,
#     Applications\msedge.exe and Applications\chrome.exe, and
#     Clients\StartMenuInternet\*: every shell\<verb>\command
#   - the switch in Run values of the machine and of each user
#   - the policies version 1 set (Edge StartupBoostEnabled and
#     BackgroundModeEnabled, Chrome BackgroundModeEnabled): each gets back
#     the state version 1 saved in HKLM\SOFTWARE\RK3576\BrowserGpu\Saved,
#     unless an administrator changed it since
#   - the switch in WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS (machine) and in
#     HKLM\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments
#     "*" (each value deleted when nothing else is left in it)
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

# Version 1 added the switch; version 2 only takes it out.
$BgVersion = 2
$BgFlag = '--disable-direct-composition'
$BgBrowsers = @('msedge.exe', 'chrome.exe', 'msedge_proxy.exe', 'chrome_proxy.exe')
$BgClassPattern = '^(MSEdge|microsoft-edge|Chrome|Chromium)'
$BgEnvName = 'WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS'
$BgTaskPath = '\RK3576\'
$BgTaskName = 'Browser GPU'
$BgStateKey = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RK3576\BrowserGpu'
$BgAbsent = '<absent>'
# What version 1 set each policy to.
$BgPolicyValue = 0

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

# A browser's command line without the switch among its switches (a
# "--disable-direct-composition" after --single-argument is part of the URL
# and stays); any other command line unchanged.
function Remove-FlagFromCommand([string]$Command) {
    $c = Split-Command $Command
    if (-not $c -or -not (Test-BrowserExe $c.Exe)) { return $Command }
    $sw = Get-SwitchPart $c.Rest
    if (-not (Test-HasFlag $sw)) { return $Command }
    $tail = $c.Rest.Substring($sw.Length)
    $sw = [regex]::Replace($sw, '\s+' + [regex]::Escape($BgFlag) + '(?=\s|$)', '')
    return $c.Head + $sw + $tail
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
function Clear-ClassCommands([string]$ClassesRoot, [bool]$DryRun) {
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
    foreach ($n in $names) { $out += Clear-ShellCommands "$ClassesRoot\$n" $DryRun }
    return $out
}

# Clients\StartMenuInternet\<browser>\shell\<verb>\command.
function Clear-StartMenuInternet([string]$ClientsRoot, [bool]$DryRun) {
    $out = @()
    $root = Open-RegKey $ClientsRoot
    if (-not $root) { return $out }
    $names = $root.GetSubKeyNames()
    $root.Close()
    foreach ($n in $names) { $out += Clear-ShellCommands "$ClientsRoot\$n" $DryRun }
    return $out
}

function Clear-ShellCommands([string]$KeyPath, [bool]$DryRun) {
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
        $new = Remove-FlagFromCommand $cur
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
function Clear-RunValues([string]$RunKey, [bool]$DryRun) {
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
        $new = Remove-FlagFromCommand $v[1]
        if ($new -ceq $v[1]) { continue }
        if (-not $DryRun) {
            $w = Open-RegKey $RunKey $true
            try { $w.SetValue($v[0], $new, $v[2]) } catch { $out += New-Change 'error' "$RunKey\$($v[0])" $v[1] $_.Exception.Message; continue } finally { if ($w) { $w.Close() } }
        }
        $out += New-Change 'run' "$RunKey\$($v[0])" $v[1] $new
    }
    return $out
}

# A list value (the WebView2 environment variable, its registry fallback)
# without the switch; the value deleted when nothing is left.
function Clear-ListValue([string]$KeyPath, [string]$Name, [bool]$DryRun, [string]$Kind, [bool]$IsEnvironment) {
    $k = Open-RegKey $KeyPath
    $cur = $null
    $vk = [Microsoft.Win32.RegistryValueKind]::String
    if ($k) {
        $cur = Get-RegValueRaw $k $Name
        if ($null -ne $cur) { $vk = $k.GetValueKind($Name) }
        $k.Close()
    }
    if ($cur -isnot [string]) { return @() }
    $new = Remove-FlagFromText $cur
    if ($new -ceq $cur) { return @() }
    if (-not $new.Trim()) { $new = $null }
    if (-not $DryRun) {
        if ($IsEnvironment) {
            # Writes the machine environment and tells running programs (WM_SETTINGCHANGE).
            [Environment]::SetEnvironmentVariable($Name, $new, 'Machine')
        } else {
            $w = Open-RegKey $KeyPath $true
            try { if ($null -eq $new) { $w.DeleteValue($Name, $false) } else { $w.SetValue($Name, $new, $vk) } } finally { $w.Close() }
        }
    }
    return , (New-Change $Kind "$KeyPath\$Name" $cur $new)
}

# A policy DWORD version 1 set: given back to the state version 1 saved in
# $StateKey\Saved, but only while it still holds version 1's value (an
# administrator's later change is left alone). The saved state is dropped
# either way. Nothing saved: never ours, nothing to do.
function Restore-PolicyDword([string]$KeyPath, [string]$Name, [string]$StateKey, [bool]$DryRun) {
    $saveName = ($KeyPath -replace '^Registry::', '') + '|' + $Name
    $sk = Open-RegKey "$StateKey\Saved"
    $saved = $null
    if ($sk) { $saved = $sk.GetValue($saveName, $null); $sk.Close() }
    if ($null -eq $saved) { return @() }
    $k = Open-RegKey $KeyPath
    $cur = $null
    if ($k) { $cur = $k.GetValue($Name, $null); $k.Close() }
    $out = @()
    if ($cur -is [int] -and $cur -eq $BgPolicyValue) {
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

function Clear-Shortcuts([string[]]$Dirs, [bool]$DryRun) {
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
                $new = Remove-FlagFromText $cur
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
# What to look at: the context
# ---------------------------------------------------------------------------

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
# whose hive is not loaded gets the shortcuts only, unless
# $LoadUnloadedHives (Install, Uninstall) loads it for the run.
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

# Takes out everything version 1 added; returns the changes (with -DryRun,
# the changes it would make).
function Invoke-BrowserGpuCleanup($Context, [bool]$DryRun) {
    $changes = @()
    $m = $Context.Machine
    if ($m) {
        foreach ($c in $m.Classes) { $changes += Clear-ClassCommands $c $DryRun }
        foreach ($c in $m.Clients) { $changes += Clear-StartMenuInternet $c $DryRun }
        foreach ($r in $m.Run) { $changes += Clear-RunValues $r $DryRun }
        $changes += Clear-Shortcuts $m.ShortcutDirs $DryRun
        $changes += Restore-PolicyDword "$($m.PolicyRoot)\Microsoft\Edge" 'StartupBoostEnabled' $Context.StateKey $DryRun
        $changes += Restore-PolicyDword "$($m.PolicyRoot)\Microsoft\Edge" 'BackgroundModeEnabled' $Context.StateKey $DryRun
        $changes += Restore-PolicyDword "$($m.PolicyRoot)\Google\Chrome" 'BackgroundModeEnabled' $Context.StateKey $DryRun
        $changes += Clear-ListValue $m.EnvKey $BgEnvName $DryRun 'environment' ([bool]$m.RealEnvironment)
        $changes += Clear-ListValue "$($m.PolicyRoot)\Microsoft\Edge\WebView2\AdditionalBrowserArguments" '*' $DryRun 'webview2' $false
    }
    foreach ($u in $Context.Users) {
        if ($u.Classes) { $changes += Clear-ClassCommands $u.Classes $DryRun }
        if ($u.Root) {
            $changes += Clear-StartMenuInternet "$($u.Root)\Software\Clients\StartMenuInternet" $DryRun
            $changes += Clear-RunValues "$($u.Root)\Software\Microsoft\Windows\CurrentVersion\Run" $DryRun
        }
        $changes += Clear-Shortcuts $u.ShortcutDirs $DryRun
    }
    return , @($changes | Where-Object { $_ })
}

# What version 1's install left besides the entries: its task, its copy,
# its settings key.
function Get-InstallLeftovers([string]$InstallDir) {
    $left = @()
    if (Get-ScheduledTask -TaskPath $BgTaskPath -TaskName $BgTaskName -ErrorAction SilentlyContinue) { $left += "task $BgTaskPath$BgTaskName" }
    if (Test-Path -LiteralPath $InstallDir) { $left += "folder $InstallDir" }
    if (Test-Path -LiteralPath $BgStateKey) { $left += "settings $($BgStateKey -replace '^Registry::', '')" }
    return $left
}

function Remove-Installation([string]$InstallDir) {
    Unregister-ScheduledTask -TaskPath $BgTaskPath -TaskName $BgTaskName -Confirm:$false -ErrorAction SilentlyContinue
    try {
        $svc = New-Object -ComObject Schedule.Service
        $svc.Connect()
        $f = $svc.GetFolder($BgTaskPath.TrimEnd('\'))
        if ($f.GetTasks(1).Count -eq 0 -and $f.GetFolders(0).Count -eq 0) { $svc.GetFolder('\').DeleteFolder('RK3576', 0) }
    } catch { }
    if (Test-Path -LiteralPath $InstallDir) { Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue }
    $p = Open-RegKey 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\RK3576' $true
    if ($p) { $p.DeleteSubKeyTree('BrowserGpu', $false); $p.Close() }
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

function Invoke-Main {
    $script:BgQuiet = [bool]$Quiet
    $admin = Test-IsAdmin
    if ($admin -and -not $DryRun) {
        $dir = Join-Path $env:ProgramData 'RK3576'
        try { New-Item -ItemType Directory -Force -Path $dir | Out-Null; $script:BgLog = Join-Path $dir 'browser-gpu.log' } catch { }
    }
    $full = $Action -in 'Install', 'Uninstall'
    if ($full -and -not $admin) {
        Say "$Action needs an administrator (or SYSTEM)."
        return 1
    }
    $installDir = Join-Path $env:ProgramFiles 'RK3576\browser-gpu'

    if ($Action -eq 'Status') {
        $script:BgLog = $null
        $ctx = Get-SystemContext
        if (-not $ctx.Machine) { Say '(not an administrator: only your own shortcuts and classes are looked at)' }
        $left = Invoke-BrowserGpuCleanup $ctx $true
        $inst = @(Get-InstallLeftovers $installDir)
        Say "rk3576-browser-gpu $BgVersion (removes $BgFlag, never adds it)"
        Say "left by an earlier version: $(@($left | Where-Object { $_.Kind -ne 'error' }).Count) entries, $($inst.Count) install parts"
        foreach ($c in $left) { if ($c.Kind -ne 'error') { Say "  left  $($c.Kind)  $($c.Where)" } }
        foreach ($i in $inst) { Say "  left  $i" }
        if ($inst) { Say '  (Install or Uninstall removes those)' }
        return 0
    }

    $ctx = Get-SystemContext $full
    try {
        $changes = Invoke-BrowserGpuCleanup $ctx ([bool]$DryRun)
    } finally { Close-Context $ctx }
    # The old task runs Apply every hour: it logs only when it changed something.
    if ($changes.Count -gt 0 -or $Action -ne 'Apply') {
        Say "$Action (take out $BgFlag): $($changes.Count) change(s)"
        Show-Changes $changes ([bool]$DryRun)
    }
    if ($full) {
        $inst = @(Get-InstallLeftovers $installDir)
        if ($DryRun) { foreach ($i in $inst) { Say "would remove $i" } }
        elseif ($inst) {
            Remove-Installation $installDir
            Say "removed $($inst -join ', ')"
        }
    }
    return 0
}

if ($MyInvocation.InvocationName -ne '.') {
    $rc = Invoke-Main
    exit $rc
}
