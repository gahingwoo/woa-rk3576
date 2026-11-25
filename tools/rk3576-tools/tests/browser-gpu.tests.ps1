# browser-gpu.tests.ps1 - tests of setup\rk3576-browser-gpu.ps1
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File browser-gpu.tests.ps1 [-Script <path>]
#
# Windows PowerShell 5.1 (and pwsh). The string functions are tested
# everywhere; on Windows also the registry and shortcut parts, against a
# made-up machine and user under HKCU\Software\RK3576BgTest and .lnk files
# in a temporary folder (nothing of the real system is touched), and the
# task XML. Exit 0 when everything passed, 1 otherwise.
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

$F = '--disable-direct-composition'
$edge = '"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"'
$chrome = '"C:\Program Files\Google\Chrome\Application\chrome.exe"'

# --- strings -------------------------------------------------------------
Eq 'edge open command gets the switch before --single-argument' (Add-FlagToCommand "$edge --single-argument %1") "$edge $F --single-argument %1"
Eq 'chrome open command' (Add-FlagToCommand "$chrome --single-argument %1") "$chrome $F --single-argument %1"
Eq 'chrome PDF command with -- ' (Add-FlagToCommand "$chrome -- `"%1`"") "$chrome $F -- `"%1`""
Eq 'command with nothing after the program' (Add-FlagToCommand $edge) "$edge $F"
Eq 'idempotent' (Add-FlagToCommand (Add-FlagToCommand "$edge --single-argument %1")) "$edge $F --single-argument %1"
Eq 'a switch after --single-argument is part of the URL, so it is added in front' (Add-FlagToCommand "$edge --single-argument $F") "$edge $F --single-argument $F"
Eq 'unquoted path with spaces' (Add-FlagToCommand 'C:\Program Files\Google\Chrome\Application\chrome.exe --single-argument %1') "C:\Program Files\Google\Chrome\Application\chrome.exe $F --single-argument %1"
Eq 'proxy (installed web app)' (Add-FlagToCommand '"C:\Program Files (x86)\Microsoft\Edge\Application\msedge_proxy.exe" --profile-directory=Default --app-id=abc') "`"C:\Program Files (x86)\Microsoft\Edge\Application\msedge_proxy.exe`" $F --profile-directory=Default --app-id=abc"
Eq 'other programs are left alone' (Add-FlagToCommand '"C:\Windows\notepad.exe" "%1"') '"C:\Windows\notepad.exe" "%1"'
Eq 'a program named like a browser only in a folder name is left alone' (Add-FlagToCommand '"C:\chrome.exe.d\tool.exe" %1') '"C:\chrome.exe.d\tool.exe" %1'
Eq 'empty command' (Add-FlagToCommand '') ''
Eq 'remove gives back the original' (Remove-FlagFromCommand (Add-FlagToCommand "$edge --single-argument %1")) "$edge --single-argument %1"
Eq 'remove from a command without it' (Remove-FlagFromCommand "$edge --single-argument %1") "$edge --single-argument %1"
Eq 'startup boost Run value' (Add-FlagToCommand "$edge --no-startup-window --win-session-start") "$edge $F --no-startup-window --win-session-start"
Eq 'shortcut arguments, empty' (Add-FlagToArgs '') $F
Eq 'shortcut arguments, a profile' (Add-FlagToArgs '--profile-directory=Default') "$F --profile-directory=Default"
Eq 'shortcut arguments, already there' (Add-FlagToArgs "--x $F") "--x $F"
Eq 'shortcut arguments, removed' (Remove-FlagFromText "$F --profile-directory=Default") '--profile-directory=Default'
Eq 'shortcut arguments, removed to empty' (Remove-FlagFromText $F) ''
Eq 'a longer switch is not ours' (Test-HasFlag '--disable-direct-composition-video-overlays') $false
Eq 'a longer switch gets ours too' (Add-FlagToArgs '--disable-direct-composition-video-overlays') "$F --disable-direct-composition-video-overlays"
Eq 'WebView2 list, empty' (Add-FlagToList '') $F
Eq 'WebView2 list, appended' (Add-FlagToList '--remote-debugging-port=9222') "--remote-debugging-port=9222 $F"
Eq 'WebView2 list, removed' (Remove-FlagFromText "--remote-debugging-port=9222 $F") '--remote-debugging-port=9222'
Eq 'WebView2 list, removed from the middle' (Remove-FlagFromText "--a $F --b") '--a --b'
Check 'browser exe names' ((Test-BrowserExe 'C:\x\MSEDGE.EXE') -and (Test-BrowserExe 'R:\chrome\chrome.exe') -and -not (Test-BrowserExe 'C:\x\msedgewebview2.exe'))

# --- decisions -----------------------------------------------------------
$yes = [pscustomobject]@{ State = 'Yes' }; $no = [pscustomobject]@{ State = 'No' }; $unk = [pscustomobject]@{ State = 'Unknown' }
Eq 'auto + yes = add' (Get-Decision 1 $yes) $true
Eq 'auto + no = take out' (Get-Decision 1 $no) $false
Check 'auto + unknown = leave alone' ($null -eq (Get-Decision 1 $unk))
Eq 'off = take out' (Get-Decision 0 $yes) $false
Eq 'always = add' (Get-Decision 2 $no) $true

$onWindows = ($env:OS -eq 'Windows_NT')
if (-not $onWindows) {
    Write-Host "(not Windows: registry, shortcut and task tests skipped)"
} else {
    # --- the gate, against a made-up Enum key ---------------------------
    $T = 'Registry::HKEY_CURRENT_USER\Software\RK3576BgTest'
    if (Test-Path $T) { Remove-Item $T -Recurse -Force }
    $E = "$T\Enum\RKCP7402"
    $code0 = { param($i) 0 }
    Eq 'gate: no device' (Get-GpuGate "$T\Enum\Nothing" $code0).State 'No'
    New-Item -Path "$E\0\Device Parameters\maliwddm" -Force | Out-Null
    New-ItemProperty -Path "$E\0" -Name Service -Value 'maliwddm' -Force | Out-Null
    Eq 'gate: maliwddm, no WddmVersion yet' (Get-GpuGate $E $code0).State 'Unknown'
    New-ItemProperty -Path "$E\0\Device Parameters\maliwddm" -Name WddmVersion -Value 0x2000 -PropertyType DWord -Force | Out-Null
    Eq 'gate: WDDM 2.0' (Get-GpuGate $E $code0).State 'Yes'
    New-ItemProperty -Path "$E\0\Device Parameters\maliwddm" -Name WddmVersion -Value 0x1300 -PropertyType DWord -Force | Out-Null
    Eq 'gate: WDDM 1.3' (Get-GpuGate $E $code0).State 'No'
    New-ItemProperty -Path "$E\0\Device Parameters\maliwddm" -Name WddmVersion -Value 0x2400 -PropertyType DWord -Force | Out-Null
    Eq 'gate: WDDM 2.4' (Get-GpuGate $E $code0).State 'Yes'
    Eq 'gate: problem code 43' (Get-GpuGate $E { param($i) 43 }).State 'No'
    Eq 'gate: not present (-1)' (Get-GpuGate $E { param($i) -1 }).State 'No'
    Eq 'gate: state unreadable' (Get-GpuGate $E { param($i) throw 'WMI down' }).State 'Unknown'
    New-ItemProperty -Path "$E\0" -Name Service -Value 'malidod' -Force | Out-Null
    Eq 'gate: another driver' (Get-GpuGate $E $code0).State 'No'
    $real = Get-GpuGate
    Check "gate on this machine says something ($($real.State): $($real.Reason))" ($real.State -in 'Yes', 'No', 'Unknown')

    # --- a made-up machine and user --------------------------------------
    $M = "$T\M"
    $U = "$T\U"
    function Set-Cmd([string]$Key, [string]$Cmd, [string]$Kind = 'String') {
        New-Item -Path $Key -Force | Out-Null
        Set-Item -Path $Key -Value $Cmd | Out-Null
        if ($Kind -eq 'ExpandString') {
            $k = Open-RegKey $Key $true; $k.SetValue('', $Cmd, [Microsoft.Win32.RegistryValueKind]::ExpandString); $k.Close()
        }
    }
    function Get-Cmd([string]$Key) { $k = Open-RegKey $Key; $v = Get-RegValueRaw $k ''; $k.Close(); return $v }
    function Get-Kind([string]$Key) { $k = Open-RegKey $Key; $v = $k.GetValueKind(''); $k.Close(); return [string]$v }

    $cmds = [ordered]@{
        "$M\Classes\MSEdgeHTM\shell\open\command"                  = "$edge --single-argument %1"
        "$M\Classes\MSEdgePDF\shell\open\command"                  = "$edge --single-argument %1"
        "$M\Classes\microsoft-edge\shell\open\command"             = "$edge --single-argument %1"
        "$M\Classes\ChromeHTML\shell\open\command"                 = "$chrome --single-argument %1"
        "$M\Classes\ChromePDF\shell\open\command"                  = "$chrome --single-argument %1"
        "$M\Classes\Applications\msedge.exe\shell\open\command"    = "$edge --single-argument %1"
        "$M\Clients\StartMenuInternet\Microsoft Edge\shell\open\command" = $edge
        "$U\Classes\ChromeHTML.ABCDEF\shell\open\command"          = '"C:\Users\u\AppData\Local\Google\Chrome\Application\chrome.exe" --single-argument %1'
    }
    $others = [ordered]@{
        "$M\Classes\txtfile\shell\open\command"                    = '%SystemRoot%\system32\NOTEPAD.EXE %1'
        "$M\Classes\ChromeOther\shell\open\command"                = '"C:\Tools\other.exe" %1'
    }
    foreach ($k in $cmds.Keys) { Set-Cmd $k $cmds[$k] }
    Set-Cmd "$M\Classes\MSEdgeHTM\shell\runas\command" '"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" --do-not-de-elevate --single-argument %1' 'ExpandString'
    $cmds["$M\Classes\MSEdgeHTM\shell\runas\command"] = '"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" --do-not-de-elevate --single-argument %1'
    foreach ($k in $others.Keys) { Set-Cmd $k $others[$k] }
    New-Item -Path "$M\Run" -Force | Out-Null
    New-ItemProperty -Path "$M\Run" -Name 'SecurityHealth' -Value '%windir%\system32\SecurityHealthSystray.exe' -PropertyType ExpandString -Force | Out-Null
    New-Item -Path "$U\Software\Microsoft\Windows\CurrentVersion\Run" -Force | Out-Null
    $boost = "$edge --no-startup-window --win-session-start"
    New-ItemProperty -Path "$U\Software\Microsoft\Windows\CurrentVersion\Run" -Name 'MicrosoftEdgeAutoLaunch_0123' -Value $boost -Force | Out-Null
    # policies: Edge's startup boost explicitly on, the rest absent
    New-Item -Path "$M\Policies\Microsoft\Edge" -Force | New-ItemProperty -Name StartupBoostEnabled -Value 1 -PropertyType DWord -Force | Out-Null
    New-Item -Path "$M\Env" -Force | New-ItemProperty -Name $BgEnvName -Value '--remote-debugging-port=9222' -Force | Out-Null

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
    New-Lnk (Join-Path $mStart 'Microsoft Edge.lnk') $edgeExe ''
    New-Lnk (Join-Path $mStart 'Google Chrome.lnk') 'C:\Program Files\Google\Chrome\Application\chrome.exe' ''
    New-Lnk (Join-Path $mStart 'Notepad.lnk') 'C:\Windows\notepad.exe' '/A'
    New-Lnk (Join-Path $uPinned 'Microsoft Edge.lnk') $edgeExe '--profile-directory=Default'
    New-Lnk (Join-Path $uDesk 'Chrome (portable).lnk') 'R:\chrome-portable\chrome.exe' '--user-data-dir=R:\profile'
    $ro = Join-Path $uDesk 'Read only Edge.lnk'
    New-Lnk $ro $edgeExe ''
    (Get-Item $ro).IsReadOnly = $true
    $lnkBefore = @{}
    foreach ($f in Get-ChildItem $tmp -Recurse -Filter *.lnk) { $lnkBefore[$f.FullName] = Get-LnkArgs $f.FullName }

    $ctx = [pscustomobject]@{
        Machine  = [pscustomobject]@{
            Classes = @("$M\Classes"); Clients = @("$M\Clients\StartMenuInternet", "$M\NoSuchClients")
            Run = @("$M\Run"); ShortcutDirs = @($mStart, (Join-Path $tmp 'nothing-here'))
            PolicyRoot = "$M\Policies"; EnvKey = "$M\Env"; RealEnvironment = $false
        }
        Users    = @([pscustomobject]@{ Name = 'u'; Root = $U; Classes = "$U\Classes"; ShortcutDirs = @((Join-Path $tmp 'u\AppData\Roaming\Microsoft\Internet Explorer\Quick Launch'), $uDesk); Temp = @() })
        StateKey = "$T\State"
    }

    # dry run: everything reported, nothing written
    $dry = Invoke-BrowserGpuSync $ctx $true $true
    $unchanged = $true
    foreach ($k in $cmds.Keys) { if ((Get-Cmd $k) -cne $cmds[$k]) { $unchanged = $false } }
    foreach ($f in $lnkBefore.Keys) { if ((Get-LnkArgs $f) -cne $lnkBefore[$f]) { $unchanged = $false } }
    Check 'dry run writes nothing' ($unchanged -and -not (Test-Path "$T\State"))
    # 9 commands, 1 Run value, 5 shortcuts, 3 policies, env, WebView2 fallback
    Eq 'dry run reports every entry' $dry.Count 20

    $c1 = Invoke-BrowserGpuSync $ctx $true $false
    Eq 'apply changes what the dry run said' $c1.Count $dry.Count
    Check 'apply: no errors' (-not ($c1 | Where-Object { $_.Kind -eq 'error' })) (($c1 | Where-Object { $_.Kind -eq 'error' } | ForEach-Object { "$($_.Where): $($_.After)" }) -join '; ')
    foreach ($k in $cmds.Keys) { Eq "apply: $($k -replace '.*RK3576BgTest\\', '')" (Get-Cmd $k) (Add-FlagToCommand $cmds[$k]) }
    Check 'apply: switch before --single-argument' ((Get-Cmd "$M\Classes\MSEdgeHTM\shell\open\command") -like "*msedge.exe`" $F --single-argument %1")
    Eq 'apply: value kind kept' (Get-Kind "$M\Classes\MSEdgeHTM\shell\runas\command") 'ExpandString'
    foreach ($k in $others.Keys) { Eq "apply leaves alone: $($k -replace '.*RK3576BgTest\\', '')" (Get-Cmd $k) $others[$k] }
    Eq 'apply: startup boost Run value' ((Get-ItemProperty "$U\Software\Microsoft\Windows\CurrentVersion\Run").'MicrosoftEdgeAutoLaunch_0123') (Add-FlagToCommand $boost)
    Eq 'apply: other Run values alone' ((Get-Item "$M\Run").GetValue('SecurityHealth', $null, 'DoNotExpandEnvironmentNames')) '%windir%\system32\SecurityHealthSystray.exe'
    Eq 'apply: Edge pinned taskbar shortcut' (Get-LnkArgs (Join-Path $uPinned 'Microsoft Edge.lnk')) "$F --profile-directory=Default"
    Eq 'apply: Edge Start menu shortcut' (Get-LnkArgs (Join-Path $mStart 'Microsoft Edge.lnk')) $F
    Eq 'apply: Chrome Start menu shortcut' (Get-LnkArgs (Join-Path $mStart 'Google Chrome.lnk')) $F
    Eq 'apply: portable Chrome shortcut' (Get-LnkArgs (Join-Path $uDesk 'Chrome (portable).lnk')) "$F --user-data-dir=R:\profile"
    Eq 'apply: read-only shortcut' (Get-LnkArgs $ro) $F
    Check 'apply: read-only shortcut stays read-only' ((Get-Item $ro).IsReadOnly)
    Eq 'apply: Notepad shortcut alone' (Get-LnkArgs (Join-Path $mStart 'Notepad.lnk')) '/A'
    Check 'apply: shortcut target kept' ($wsh.CreateShortcut((Join-Path $uPinned 'Microsoft Edge.lnk')).TargetPath -eq $edgeExe)
    Eq 'apply: Edge StartupBoostEnabled' ((Get-ItemProperty "$M\Policies\Microsoft\Edge").StartupBoostEnabled) 0
    Eq 'apply: Edge BackgroundModeEnabled' ((Get-ItemProperty "$M\Policies\Microsoft\Edge").BackgroundModeEnabled) 0
    Eq 'apply: Chrome BackgroundModeEnabled' ((Get-ItemProperty "$M\Policies\Google\Chrome").BackgroundModeEnabled) 0
    Eq 'apply: WebView2 variable appended' ((Get-ItemProperty "$M\Env").$BgEnvName) "--remote-debugging-port=9222 $F"
    Eq 'apply: WebView2 registry fallback' ((Get-ItemProperty "$M\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments").'*') $F

    $c2 = Invoke-BrowserGpuSync $ctx $true $false
    Eq 'apply again changes nothing' $c2.Count 0

    # a browser update rewrote one command and one shortcut
    Set-Cmd "$M\Classes\MSEdgeHTM\shell\open\command" "$edge --single-argument %1"
    New-Lnk (Join-Path $mStart 'Microsoft Edge.lnk') $edgeExe ''
    $c3 = Invoke-BrowserGpuSync $ctx $true $false
    Eq 'after an update only the rewritten entries change' $c3.Count 2

    # a user who turned it off for themselves
    New-Item -Path "$U\$BgUserStateSub" -Force | New-ItemProperty -Name Mode -Value 0 -PropertyType DWord -Force | Out-Null
    $c4 = Invoke-BrowserGpuSync $ctx $true $false
    Eq "a user's Mode 0 takes their entries out (class, Run, 3 shortcuts)" $c4.Count 5
    Eq "a user's Mode 0: pinned shortcut back" (Get-LnkArgs (Join-Path $uPinned 'Microsoft Edge.lnk')) '--profile-directory=Default'
    Eq "a user's Mode 0: machine entries keep it" (Get-LnkArgs (Join-Path $mStart 'Microsoft Edge.lnk')) $F
    Remove-Item -Path "$U\Software\RK3576" -Recurse -Force

    # an administrator changed a policy after us: revert leaves it
    Set-ItemProperty -Path "$M\Policies\Google\Chrome" -Name BackgroundModeEnabled -Value 1

    $r = Invoke-BrowserGpuSync $ctx $false $false
    Check 'revert: no errors' (-not ($r | Where-Object { $_.Kind -eq 'error' }))
    foreach ($k in $cmds.Keys) { Eq "revert: $($k -replace '.*RK3576BgTest\\', '')" (Get-Cmd $k) $cmds[$k] }
    Eq 'revert: kind kept' (Get-Kind "$M\Classes\MSEdgeHTM\shell\runas\command") 'ExpandString'
    foreach ($f in $lnkBefore.Keys) { Eq "revert: $(Split-Path $f -Leaf)" (Get-LnkArgs $f) $lnkBefore[$f] }
    Eq 'revert: Run value' ((Get-ItemProperty "$U\Software\Microsoft\Windows\CurrentVersion\Run").'MicrosoftEdgeAutoLaunch_0123') $boost
    Eq 'revert: StartupBoostEnabled back to 1' ((Get-ItemProperty "$M\Policies\Microsoft\Edge").StartupBoostEnabled) 1
    Check 'revert: BackgroundModeEnabled absent again' ($null -eq (Get-ItemProperty "$M\Policies\Microsoft\Edge").BackgroundModeEnabled)
    Eq "revert: an administrator's later value is kept" ((Get-ItemProperty "$M\Policies\Google\Chrome").BackgroundModeEnabled) 1
    Eq 'revert: WebView2 variable as it was' ((Get-ItemProperty "$M\Env").$BgEnvName) '--remote-debugging-port=9222'
    Check 'revert: WebView2 fallback value gone' ($null -eq (Get-ItemProperty "$M\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments" -ErrorAction SilentlyContinue).'*')
    $saved = Get-Item "$T\State\Saved" -ErrorAction SilentlyContinue
    Check 'revert: nothing left in the saved state' (-not $saved -or $saved.ValueCount -eq 0)
    $r2 = Invoke-BrowserGpuSync $ctx $false $false
    Eq 'revert again changes nothing' $r2.Count 0

    # a variable we made from nothing is deleted on revert
    Remove-ItemProperty -Path "$M\Env" -Name $BgEnvName
    [void](Invoke-BrowserGpuSync $ctx $true $false)
    Eq 'variable made from nothing' ((Get-ItemProperty "$M\Env").$BgEnvName) $F
    [void](Invoke-BrowserGpuSync $ctx $false $false)
    Check 'variable made from nothing is deleted' ($null -eq (Get-ItemProperty "$M\Env").$BgEnvName)

    # Policies 0 and WebView2 0
    $c5 = Invoke-BrowserGpuSync $ctx $true $false $false $false
    Check 'Policies 0, WebView2 0: no policy, variable or fallback' (-not ($c5 | Where-Object { $_.Kind -in 'policy', 'environment', 'webview2' }))
    [void](Invoke-BrowserGpuSync $ctx $false $false)

    # nothing installed at all (Maskrom Studio's default: no Edge)
    $empty = [pscustomobject]@{
        Machine  = [pscustomobject]@{ Classes = @("$T\Empty\Classes"); Clients = @("$T\Empty\Clients"); Run = @("$T\Empty\Run"); ShortcutDirs = @((Join-Path $tmp 'none'))
                                      PolicyRoot = "$T\Empty\Policies"; EnvKey = "$T\Empty\Env"; RealEnvironment = $false }
        Users    = @([pscustomobject]@{ Name = 'none'; Root = "$T\Empty\User"; Classes = $null; ShortcutDirs = @(); Temp = @() })
        StateKey = "$T\Empty\State"
    }
    $e1 = Invoke-BrowserGpuSync $empty $true $false
    Check 'no browser installed: only policies and WebView2, no errors' ((-not ($e1 | Where-Object { $_.Kind -notin 'policy', 'environment', 'webview2' })) -and $e1.Count -eq 5)
    [void](Invoke-BrowserGpuSync $empty $false $false)
    Check 'no browser installed: revert leaves nothing' (-not (Get-Item "$T\Empty\Env" -ErrorAction SilentlyContinue).ValueCount)

    # --- the task XML -----------------------------------------------------
    $xml = [xml](Get-TaskXml 'C:\Program Files\RK3576\browser-gpu\rk3576-browser-gpu.ps1')
    $ns = New-Object Xml.XmlNamespaceManager $xml.NameTable
    $ns.AddNamespace('t', 'http://schemas.microsoft.com/windows/2004/02/mit/task')
    Eq 'task runs as SYSTEM' $xml.SelectSingleNode('//t:Principal/t:UserId', $ns).InnerText 'S-1-5-18'
    Check 'task: boot, logon, hourly and event triggers' ((($xml.SelectNodes('//t:Triggers/*', $ns) | ForEach-Object { $_.LocalName }) -join ',') -eq 'BootTrigger,LogonTrigger,TimeTrigger,EventTrigger')
    Check 'task: the subscription is a query list' (([xml]$xml.SelectSingleNode('//t:Subscription', $ns).InnerText).QueryList.Query.Select.Count -eq 2)
    Check 'task: runs Apply quietly' ($xml.SelectSingleNode('//t:Arguments', $ns).InnerText -like '*-File "C:\Program Files\RK3576\browser-gpu\rk3576-browser-gpu.ps1" -Action Apply -Quiet')

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
