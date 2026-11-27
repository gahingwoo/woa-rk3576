# browser-gpu.tests.ps1 - tests of setup\rk3576-browser-gpu.ps1
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File browser-gpu.tests.ps1 [-Script <path>]
#
# Windows PowerShell 5.1 (and pwsh). The script only takes out what its
# version 1 added. The string functions are tested everywhere; on Windows
# also the registry and shortcut parts: a made-up machine and user under
# HKCU\Software\RK3576BgTest and .lnk files in a temporary folder are set
# up the way version 1 left them (nothing of the real system is touched),
# the cleanup must give back every earlier value, and nothing may ever get
# the switch. Exit 0 when everything passed, 1 otherwise.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param([string]$Script = (Join-Path $PSScriptRoot '..\setup\rk3576-browser-gpu.ps1'))

$ErrorActionPreference = 'Stop'
. $Script

$script:Failed = 0
$script:Passed = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { $script:Passed++; Write-Host "ok    $Name" }
    else { $script:Failed++; Write-Host "FAIL  $Name $Detail" }
}
function Eq([string]$Name, $Got, $Want) {
    Check $Name ($Got -ceq $Want) "`n      got:  [$Got]`n      want: [$Want]"
}

$SW = '--disable-direct-composition'
$edge = '"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"'
$chrome = '"C:\Program Files\Google\Chrome\Application\chrome.exe"'

# --- nothing adds the switch ------------------------------------------------
# version 1's functions that added the switch, gated it or made the task
$v1 = 'Add-FlagToArgs', 'Add-FlagToCommand', 'Add-FlagToList', 'Sync-ClassCommands', 'Sync-StartMenuInternet', 'Sync-ShellCommands',
      'Sync-RunValues', 'Sync-ListValue', 'Sync-PolicyDword', 'Sync-Shortcuts', 'Invoke-BrowserGpuSync', 'Get-GpuGate', 'Get-Decision', 'Get-TaskXml'
$adders = @($v1 | Where-Object { Test-Path "function:\$_" })
Check 'no function that adds the switch, gates it or makes the task' ($adders.Count -eq 0) ($adders -join ', ')
$text = Get-Content -Raw -LiteralPath $Script
Check 'the script registers no task' ($text -notmatch '(?<!Un)Register-ScheduledTask')
# $BgFlag only in its definition, in regexes that find it, and in messages
$uses = @($text -split "`n" | Where-Object { $_ -match '\$BgFlag' -and $_ -notmatch '^\$BgFlag = ' -and $_ -notmatch 'Escape\(\$BgFlag\)' -and $_ -notmatch '^\s*Say "' })
Check 'the script never writes the switch anywhere' ($uses.Count -eq 0) ($uses -join ' | ')

# --- strings: every shape version 1 wrote -----------------------------------
Eq 'edge open command' (Remove-FlagFromCommand "$edge $SW --single-argument %1") "$edge --single-argument %1"
Eq 'chrome open command' (Remove-FlagFromCommand "$chrome $SW --single-argument %1") "$chrome --single-argument %1"
Eq 'chrome PDF command with --' (Remove-FlagFromCommand "$chrome $SW -- `"%1`"") "$chrome -- `"%1`""
Eq 'command with nothing after the program' (Remove-FlagFromCommand "$edge $SW") $edge
Eq 'unquoted path with spaces' (Remove-FlagFromCommand "C:\Program Files\Google\Chrome\Application\chrome.exe $SW --single-argument %1") 'C:\Program Files\Google\Chrome\Application\chrome.exe --single-argument %1'
Eq 'proxy (installed web app)' (Remove-FlagFromCommand "`"C:\x\msedge_proxy.exe`" $SW --profile-directory=Default --app-id=abc") '"C:\x\msedge_proxy.exe" --profile-directory=Default --app-id=abc'
Eq 'startup boost Run value' (Remove-FlagFromCommand "$edge $SW --no-startup-window --win-session-start") "$edge --no-startup-window --win-session-start"
Eq 'a switch after --single-argument is part of the URL and stays' (Remove-FlagFromCommand "$edge --single-argument $SW") "$edge --single-argument $SW"
Eq 'other programs are left alone' (Remove-FlagFromCommand "`"C:\Tools\other.exe`" $SW %1") "`"C:\Tools\other.exe`" $SW %1"
Eq 'command without it' (Remove-FlagFromCommand "$edge --single-argument %1") "$edge --single-argument %1"
Eq 'empty command' (Remove-FlagFromCommand '') ''
Eq 'shortcut arguments, a profile' (Remove-FlagFromText "$SW --profile-directory=Default") '--profile-directory=Default'
Eq 'shortcut arguments, to empty' (Remove-FlagFromText $SW) ''
Eq 'WebView2 list, at the end' (Remove-FlagFromText "--remote-debugging-port=9222 $SW") '--remote-debugging-port=9222'
Eq 'WebView2 list, in the middle' (Remove-FlagFromText "--a $SW --b") '--a --b'
Eq 'a longer switch is not ours' (Remove-FlagFromText '--disable-direct-composition-video-overlays') '--disable-direct-composition-video-overlays'
Check 'browser exe names' ((Test-BrowserExe 'C:\x\MSEDGE.EXE') -and (Test-BrowserExe 'R:\chrome\chrome.exe') -and -not (Test-BrowserExe 'C:\x\msedgewebview2.exe'))
foreach ($s in "$edge --single-argument %1", '--profile-directory=Default', '', '"C:\Windows\notepad.exe" "%1"') {
    Check "nothing gets the switch: [$s]" (-not (Test-HasFlag (Remove-FlagFromCommand $s)) -and -not (Test-HasFlag (Remove-FlagFromText $s)))
}

$onWindows = ($env:OS -eq 'Windows_NT')
if (-not $onWindows) {
    Write-Host "(not Windows: registry and shortcut tests skipped)"
} else {
    $T = 'Registry::HKEY_CURRENT_USER\Software\RK3576BgTest'
    if (Test-Path $T) { Remove-Item $T -Recurse -Force }
    $M = "$T\M"
    $U = "$T\U"
    function Set-Cmd([string]$Key, [string]$Cmd, [string]$Kind = 'String') {
        New-Item -Path $Key -Force | Out-Null
        $k = Open-RegKey $Key $true; $k.SetValue('', $Cmd, [Microsoft.Win32.RegistryValueKind]$Kind); $k.Close()
    }
    function Get-Cmd([string]$Key) { $k = Open-RegKey $Key; $v = Get-RegValueRaw $k ''; $k.Close(); return $v }
    function Get-Kind([string]$Key) { $k = Open-RegKey $Key; $v = $k.GetValueKind(''); $k.Close(); return [string]$v }
    # What version 1 made of a command: the switch right after the program.
    function V1([string]$Cmd) { $c = Split-Command $Cmd; return $c.Head + ' ' + $SW + $c.Rest }
    # Every value under the test key and every test shortcut that has the switch.
    function Get-WithFlag {
        $hits = @()
        foreach ($k in Get-ChildItem $T -Recurse) {
            foreach ($n in $k.GetValueNames()) {
                $v = $k.GetValue($n, $null, 'DoNotExpandEnvironmentNames')
                if ($v -is [string] -and (Test-HasFlag $v)) { $hits += "$($k.Name)\$n" }
            }
        }
        foreach ($f in Get-ChildItem $tmp -Recurse -Filter *.lnk) { if (Test-HasFlag (Get-LnkArgs $f.FullName)) { $hits += $f.FullName } }
        return $hits
    }

    # --- the clean state, as a browser installs it ------------------------
    $cmds = [ordered]@{
        "$M\Classes\MSEdgeHTM\shell\open\command"                  = "$edge --single-argument %1"
        "$M\Classes\MSEdgePDF\shell\open\command"                  = "$edge --single-argument %1"
        "$M\Classes\microsoft-edge\shell\open\command"             = "$edge --single-argument %1"
        "$M\Classes\ChromeHTML\shell\open\command"                 = "$chrome --single-argument %1"
        "$M\Classes\ChromePDF\shell\open\command"                  = "$chrome -- `"%1`""
        "$M\Classes\Applications\msedge.exe\shell\open\command"    = "$edge --single-argument %1"
        "$M\Clients\StartMenuInternet\Microsoft Edge\shell\open\command" = $edge
        "$U\Classes\ChromeHTML.ABCDEF\shell\open\command"          = '"C:\Users\u\AppData\Local\Google\Chrome\Application\chrome.exe" --single-argument %1'
        "$U\Software\Clients\StartMenuInternet\Google Chrome.ABCDEF\shell\open\command" = '"C:\Users\u\AppData\Local\Google\Chrome\Application\chrome.exe"'
    }
    $runas = "$M\Classes\MSEdgeHTM\shell\runas\command"
    $runasCmd = '"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" --do-not-de-elevate --single-argument %1'
    # never ours: other programs, a browser that never had it, a URL that names the switch
    $others = [ordered]@{
        "$M\Classes\txtfile\shell\open\command"                    = '%SystemRoot%\system32\NOTEPAD.EXE %1'
        "$M\Classes\ChromeOther\shell\open\command"                = "`"C:\Tools\other.exe`" $SW %1"
        "$M\Classes\MSEdgeMHT\shell\open\command"                  = "$edge --single-argument %1"
        "$M\Classes\ChromeURL\shell\open\command"                  = "$chrome --single-argument $SW"
    }
    $boost = "$edge --no-startup-window --win-session-start"
    $machineBoost = "$chrome --no-startup-window"

    # shortcuts
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("rk3576bg-" + [guid]::NewGuid().ToString('n').Substring(0, 8))
    $mStart = Join-Path $tmp 'ProgramData\Start Menu\Programs'
    $uPinned = Join-Path $tmp 'u\AppData\Roaming\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'
    $uDesk = Join-Path $tmp 'u\Desktop'
    New-Item -ItemType Directory -Force -Path $mStart, $uPinned, $uDesk | Out-Null
    $wsh = New-Object -ComObject WScript.Shell
    function New-Lnk([string]$Path, [string]$Target, [string]$Arguments) {
        $s = $wsh.CreateShortcut($Path); $s.TargetPath = $Target; $s.Arguments = $Arguments; $s.Description = 'test'; $s.Save()
    }
    function Get-LnkArgs([string]$Path) { return [string]$wsh.CreateShortcut($Path).Arguments }
    $edgeExe = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
    $lnks = [ordered]@{
        (Join-Path $mStart 'Microsoft Edge.lnk')        = @($edgeExe, '')
        (Join-Path $mStart 'Google Chrome.lnk')         = @('C:\Program Files\Google\Chrome\Application\chrome.exe', '')
        (Join-Path $uPinned 'Microsoft Edge.lnk')       = @($edgeExe, '--profile-directory=Default')
        (Join-Path $uDesk 'Chrome (portable).lnk')      = @('R:\chrome-portable\chrome.exe', '--user-data-dir=R:\profile')
        (Join-Path $uDesk 'Read only Edge.lnk')         = @($edgeExe, '')
    }
    $otherLnk = Join-Path $mStart 'Notepad.lnk'
    $ro = Join-Path $uDesk 'Read only Edge.lnk'

    $ctx = [pscustomobject]@{
        Machine  = [pscustomobject]@{
            Classes = @("$M\Classes"); Clients = @("$M\Clients\StartMenuInternet", "$M\NoSuchClients")
            Run = @("$M\Run"); ShortcutDirs = @($mStart, (Join-Path $tmp 'nothing-here'))
            PolicyRoot = "$M\Policies"; EnvKey = "$M\Env"; RealEnvironment = $false
        }
        Users    = @([pscustomobject]@{ Name = 'u'; Root = $U; Classes = "$U\Classes"; ShortcutDirs = @((Join-Path $tmp 'u\AppData\Roaming\Microsoft\Internet Explorer\Quick Launch'), $uDesk); Temp = @() })
        StateKey = "$T\State"
    }
    $saved = "$T\State\Saved"
    $pol = ($M -replace '^Registry::', '') + '\Policies'

    function Set-Clean {
        foreach ($k in $cmds.Keys) { Set-Cmd $k $cmds[$k] }
        Set-Cmd $runas $runasCmd 'ExpandString'
        foreach ($k in $others.Keys) { Set-Cmd $k $others[$k] }
        New-Item -Path "$M\Run" -Force | Out-Null
        New-ItemProperty -Path "$M\Run" -Name 'SecurityHealth' -Value '%windir%\system32\SecurityHealthSystray.exe' -PropertyType ExpandString -Force | Out-Null
        New-ItemProperty -Path "$M\Run" -Name 'GoogleChromeAutoLaunch' -Value $machineBoost -Force | Out-Null
        New-Item -Path "$U\Software\Microsoft\Windows\CurrentVersion\Run" -Force | Out-Null
        New-ItemProperty -Path "$U\Software\Microsoft\Windows\CurrentVersion\Run" -Name 'MicrosoftEdgeAutoLaunch_0123' -Value $boost -Force | Out-Null
        # policies: Edge's startup boost explicitly on, an administrator's own Chrome value
        New-Item -Path "$M\Policies\Microsoft\Edge" -Force | New-ItemProperty -Name StartupBoostEnabled -Value 1 -PropertyType DWord -Force | Out-Null
        New-Item -Path "$M\Policies\Google\Chrome" -Force | New-ItemProperty -Name BackgroundModeEnabled -Value 0 -PropertyType DWord -Force | Out-Null
        New-Item -Path "$M\Env" -Force | New-ItemProperty -Name $BgEnvName -Value '--remote-debugging-port=9222' -Force | Out-Null
        foreach ($p in $lnks.Keys) {
            if (Test-Path $p) { (Get-Item $p).IsReadOnly = $false }
            New-Lnk $p $lnks[$p][0] $lnks[$p][1]
        }
        (Get-Item $ro).IsReadOnly = $true
        New-Lnk $otherLnk 'C:\Windows\notepad.exe' "/A $SW"
    }

    # What version 1's Install left: the switch everywhere, the policies at 0
    # with their earlier state saved, the switch appended to WebView2's arguments.
    function Set-V1 {
        foreach ($k in $cmds.Keys) { Set-Cmd $k (V1 $cmds[$k]) }
        Set-Cmd $runas (V1 $runasCmd) 'ExpandString'
        Set-ItemProperty -Path "$M\Run" -Name 'GoogleChromeAutoLaunch' -Value (V1 $machineBoost)
        Set-ItemProperty -Path "$U\Software\Microsoft\Windows\CurrentVersion\Run" -Name 'MicrosoftEdgeAutoLaunch_0123' -Value (V1 $boost)
        $k = Open-RegKey $saved $true $true
        $k.SetValue("$pol\Microsoft\Edge|StartupBoostEnabled", '1')
        $k.SetValue("$pol\Microsoft\Edge|BackgroundModeEnabled", $BgAbsent)
        $k.Close()
        Set-ItemProperty -Path "$M\Policies\Microsoft\Edge" -Name StartupBoostEnabled -Value 0
        New-ItemProperty -Path "$M\Policies\Microsoft\Edge" -Name BackgroundModeEnabled -Value 0 -PropertyType DWord -Force | Out-Null
        Set-ItemProperty -Path "$M\Env" -Name $BgEnvName -Value "--remote-debugging-port=9222 $SW"
        New-Item -Path "$M\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments" -Force | New-ItemProperty -Name '*' -Value $SW -Force | Out-Null
        foreach ($p in $lnks.Keys) {
            $ro0 = (Get-Item $p).IsReadOnly
            (Get-Item $p).IsReadOnly = $false
            $a = $lnks[$p][1]
            if ($a) { $a = "$SW $a" } else { $a = $SW }
            New-Lnk $p $lnks[$p][0] $a
            (Get-Item $p).IsReadOnly = $ro0
        }
    }

    function Test-Clean([string]$Tag) {
        foreach ($k in $cmds.Keys) { Eq "${Tag}: $($k -replace '.*RK3576BgTest\\', '')" (Get-Cmd $k) $cmds[$k] }
        Eq "${Tag}: runas command" (Get-Cmd $runas) $runasCmd
        Eq "${Tag}: runas value kind kept" (Get-Kind $runas) 'ExpandString'
        foreach ($k in $others.Keys) { Eq "$Tag leaves alone: $($k -replace '.*RK3576BgTest\\', '')" (Get-Cmd $k) $others[$k] }
        Eq "${Tag}: machine Run value" ((Get-ItemProperty "$M\Run").GoogleChromeAutoLaunch) $machineBoost
        Eq "${Tag}: user Run value" ((Get-ItemProperty "$U\Software\Microsoft\Windows\CurrentVersion\Run").'MicrosoftEdgeAutoLaunch_0123') $boost
        Eq "${Tag}: other Run values alone" ((Get-Item "$M\Run").GetValue('SecurityHealth', $null, 'DoNotExpandEnvironmentNames')) '%windir%\system32\SecurityHealthSystray.exe'
        foreach ($p in $lnks.Keys) { Eq "${Tag}: $(Split-Path $p -Leaf)" (Get-LnkArgs $p) $lnks[$p][1] }
        Check "${Tag}: shortcut target kept" ($wsh.CreateShortcut((Join-Path $uPinned 'Microsoft Edge.lnk')).TargetPath -eq $edgeExe)
        Check "${Tag}: read-only shortcut stays read-only" ((Get-Item $ro).IsReadOnly)
        Eq "${Tag}: Notepad shortcut alone" (Get-LnkArgs $otherLnk) "/A $SW"
        Eq "${Tag}: StartupBoostEnabled back to 1" ((Get-ItemProperty "$M\Policies\Microsoft\Edge").StartupBoostEnabled) 1
        Check "${Tag}: BackgroundModeEnabled absent again" ($null -eq (Get-ItemProperty "$M\Policies\Microsoft\Edge").BackgroundModeEnabled)
        Eq "${Tag}: an administrator's own Chrome policy is kept" ((Get-ItemProperty "$M\Policies\Google\Chrome").BackgroundModeEnabled) 0
        Eq "${Tag}: WebView2 variable as it was" ((Get-ItemProperty "$M\Env").$BgEnvName) '--remote-debugging-port=9222'
        Check "${Tag}: WebView2 fallback value gone" ($null -eq (Get-ItemProperty "$M\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments" -ErrorAction SilentlyContinue).'*')
        $s = Get-Item $saved -ErrorAction SilentlyContinue
        Check "${Tag}: nothing left in the saved state" (-not $s -or $s.ValueCount -eq 0)
    }

    # --- a clean machine: nothing to do, nothing added ----------------------
    Set-Clean
    $flagBefore = @(Get-WithFlag)
    $c0 = Invoke-BrowserGpuCleanup $ctx $false
    Eq 'clean machine: no change' $c0.Count 0
    $flagAfter = @(Get-WithFlag)
    Eq 'clean machine: no entry got the switch' ($flagAfter -join '|') ($flagBefore -join '|')

    # --- version 1's leftovers ---------------------------------------------
    Set-V1
    Check 'set-up: the switch is in place' ((Get-WithFlag).Count -gt $flagBefore.Count)
    $dry = Invoke-BrowserGpuCleanup $ctx $true
    # 10 commands, 2 Run values, 5 shortcuts, 2 policies, env, WebView2 fallback
    Eq 'dry run reports every entry' $dry.Count 21
    Eq 'dry run writes nothing' (Get-Cmd "$M\Classes\MSEdgeHTM\shell\open\command") (V1 $cmds["$M\Classes\MSEdgeHTM\shell\open\command"])
    Check 'dry run keeps the saved state' ((Get-Item $saved).ValueCount -eq 2)

    $r = Invoke-BrowserGpuCleanup $ctx $false
    Eq 'cleanup changes what the dry run said' $r.Count $dry.Count
    Check 'cleanup: no errors' (-not ($r | Where-Object { $_.Kind -eq 'error' })) (($r | Where-Object { $_.Kind -eq 'error' } | ForEach-Object { "$($_.Where): $($_.After)" }) -join '; ')
    $kinds = ($r | ForEach-Object { $_.Kind } | Sort-Object -Unique) -join ','
    Eq 'cleanup: every entry kind' $kinds 'command,environment,policy,run,shortcut,webview2'
    Check 'cleanup: no change puts the switch in' (-not ($r | Where-Object { $_.After -is [string] -and (Test-HasFlag $_.After) }))
    Test-Clean 'cleanup'
    Eq 'cleanup: only the entries that were never ours keep the switch' ((Get-WithFlag) -join '|') ($flagBefore -join '|')
    $r2 = Invoke-BrowserGpuCleanup $ctx $false
    Eq 'cleanup again changes nothing' $r2.Count 0

    # an administrator changed a policy after version 1: it is left, its saved state dropped
    Set-V1
    Set-ItemProperty -Path "$M\Policies\Microsoft\Edge" -Name StartupBoostEnabled -Value 2
    [void](Invoke-BrowserGpuCleanup $ctx $false)
    Eq "an administrator's later policy value is kept" ((Get-ItemProperty "$M\Policies\Microsoft\Edge").StartupBoostEnabled) 2
    Check 'its saved state is dropped' ((Get-Item $saved).ValueCount -eq 0)
    Set-ItemProperty -Path "$M\Policies\Microsoft\Edge" -Name StartupBoostEnabled -Value 1

    # a WebView2 variable that held only the switch is deleted
    Set-ItemProperty -Path "$M\Env" -Name $BgEnvName -Value $SW
    $e = Invoke-BrowserGpuCleanup $ctx $false
    Eq 'variable with only the switch: one change' $e.Count 1
    Check 'variable with only the switch is deleted' ($null -eq (Get-ItemProperty "$M\Env").$BgEnvName)

    # nothing installed at all (Maskrom Studio's default: no Edge)
    $empty = [pscustomobject]@{
        Machine  = [pscustomobject]@{ Classes = @("$T\Empty\Classes"); Clients = @("$T\Empty\Clients"); Run = @("$T\Empty\Run"); ShortcutDirs = @((Join-Path $tmp 'none'))
                                      PolicyRoot = "$T\Empty\Policies"; EnvKey = "$T\Empty\Env"; RealEnvironment = $false }
        Users    = @([pscustomobject]@{ Name = 'none'; Root = "$T\Empty\User"; Classes = $null; ShortcutDirs = @(); Temp = @() })
        StateKey = "$T\Empty\State"
    }
    $e1 = Invoke-BrowserGpuCleanup $empty $false
    Eq 'no browser installed: no change' $e1.Count 0
    Check 'no browser installed: nothing created' (-not (Test-Path "$T\Empty"))

    # cleanup
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wsh)
    (Get-Item $ro).IsReadOnly = $false
    Remove-Item $tmp -Recurse -Force
    Remove-Item $T -Recurse -Force
}

Write-Host ""
Write-Host "$script:Passed passed, $script:Failed failed"
if ($script:Failed) { exit 1 }
exit 0
