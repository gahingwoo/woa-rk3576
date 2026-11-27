# charsiu-cli.tests.ps1 - tests of the charsiu command the RK3576 tools install
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File charsiu-cli.tests.ps1
#       [-Setup <rk3576-setup.ps1>] [-Npu <a bundle's RK3576\npu>] [-Installed]
#
# Windows PowerShell 5.1 (and pwsh). Always: the machine Path edits of
# rk3576-setup.ps1, as strings. On Windows also: the NPU's UserAccess on a
# made-up Enum tree under HKCU\Software\RK3576NpuTest (nothing of the real
# system is touched); with -Npu, npu\bin\charsiu.cmd as a console runs it:
# arguments with spaces reach charsiu_run's command line whole, the NPU
# variables are set, one already in the environment wins, the config can
# turn the NPU off (`charsiu ... --show` prints what it would run, so no
# NPU and no arm64 program is needed); with -Installed, the machine Path
# of an installed system has npu\bin once and a new console finds
# `charsiu`. Exit 0 when everything passed, 1 otherwise.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string]$Setup = (Join-Path $PSScriptRoot '..\setup\rk3576-setup.ps1'),
    [string]$Npu = '',
    [switch]$Installed
)

$ErrorActionPreference = 'Stop'
. $Setup
$ErrorActionPreference = 'Stop'

$script:Failed = 0
$script:Passed = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { $script:Passed++; Write-Host "ok    $Name" }
    else { $script:Failed++; Write-Host "FAIL  $Name $Detail" }
}
function Eq([string]$Name, $Got, $Want) {
    Check $Name ($Got -ceq $Want) "`n      got:  [$Got]`n      want: [$Want]"
}

# --- the Path, as strings ------------------------------------------------------
$env:RKTEST_PF = 'C:\PF'
$E = '%RKTEST_PF%\RK3576\npu\bin'
Eq 'added at the end' (Add-PathEntry 'C:\a;C:\b' $E) "C:\a;C:\b;$E"
Eq 'added to an empty Path' (Add-PathEntry '' $E) $E
Eq 'a trailing ; does not leave an empty entry' (Add-PathEntry 'C:\a;' $E) "C:\a;$E"
Eq 'there already: unchanged' (Add-PathEntry "C:\a;$E;C:\b" $E) "C:\a;$E;C:\b"
Eq 'there already, expanded, other case, backslash: unchanged' (Add-PathEntry 'C:\a;c:\pf\rk3576\NPU\bin\' $E) 'C:\a;c:\pf\rk3576\NPU\bin\'
Eq 'there already, quoted: unchanged' (Add-PathEntry '"C:\PF\RK3576\npu\bin"' $E) '"C:\PF\RK3576\npu\bin"'
Eq 'other variables stay unexpanded' (Add-PathEntry '%SystemRoot%\system32;%USERPROFILE%\x' $E) "%SystemRoot%\system32;%USERPROFILE%\x;$E"
Eq 'a longer folder is not ours' (Add-PathEntry 'C:\PF\RK3576\npu\bin2' $E) "C:\PF\RK3576\npu\bin2;$E"
Eq 'removed' (Remove-PathEntry "C:\a;$E;C:\b" $E) 'C:\a;C:\b'
Eq 'removed in every spelling' (Remove-PathEntry "C:\PF\RK3576\npu\bin\;C:\a;$E" $E) 'C:\a'
Eq 'removed, the only entry' (Remove-PathEntry $E $E) ''
Eq 'nothing to remove' (Remove-PathEntry '%SystemRoot%\system32;C:\a' $E) '%SystemRoot%\system32;C:\a'
Eq 'add then remove gives the Path back' (Remove-PathEntry (Add-PathEntry '%SystemRoot%;C:\a' $E) $E) '%SystemRoot%;C:\a'
Eq 'the entry the install writes' $NpuBin '%ProgramFiles%\RK3576\npu\bin'

$onWindows = ($env:OS -eq 'Windows_NT')
if (-not $onWindows) {
    Write-Host '(not Windows: registry and console tests skipped)'
} else {
    # --- UserAccess, on a made-up Enum\ACPI\RKCP3576 ----------------------------
    $T = 'HKCU:\Software\RK3576NpuTest'
    if (Test-Path $T) { Remove-Item $T -Recurse -Force }
    Eq 'no device: nothing done' (@(Set-NpuUserAccessIn "$T\Enum").Count) 0
    New-Item -Path "$T\Enum\0" -Force | Out-Null
    New-Item -Path "$T\Enum\1\Device Parameters" -Force | New-ItemProperty -Name UserAccess -PropertyType DWord -Value 0 | Out-Null
    New-Item -Path "$T\Enum\2\Device Parameters" -Force | New-ItemProperty -Name M6Iommu -PropertyType DWord -Value 1 | Out-Null
    $r = @(Set-NpuUserAccessIn "$T\Enum")
    Eq 'set where there was none, a chosen 0 kept' ($r -join ',') 'set,kept 0,set'
    Eq 'instance 0 (no Device Parameters yet)' (Get-ItemProperty "$T\Enum\0\Device Parameters").UserAccess 1
    Eq 'instance 1 keeps its 0' (Get-ItemProperty "$T\Enum\1\Device Parameters").UserAccess 0
    Eq 'instance 2 gets it, its other values stay' ('{0}/{1}' -f (Get-ItemProperty "$T\Enum\2\Device Parameters").UserAccess, (Get-ItemProperty "$T\Enum\2\Device Parameters").M6Iommu) '1/1'
    Eq 'a second run changes nothing' (@(Set-NpuUserAccessIn "$T\Enum") -join ',') 'kept 1,kept 0,kept 1'
    Eq 'remove takes out the 1s' (Remove-NpuUserAccessIn "$T\Enum") 2
    Check 'and leaves the 0' ((Get-ItemProperty "$T\Enum\1\Device Parameters").UserAccess -eq 0 -and
        $null -eq (Get-ItemProperty "$T\Enum\0\Device Parameters" -Name UserAccess -ErrorAction SilentlyContinue))
    Remove-Item $T -Recurse -Force

    # --- the shim, as a console runs it ----------------------------------------
    function Invoke-Cmd([string]$Line, [hashtable]$Env = @{}) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
        $psi.Arguments = "/d /s /c `"$Line`""
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -like 'CHARSIU_*') { $psi.EnvironmentVariables.Remove($k) } }
        foreach ($k in $Env.Keys) { $psi.EnvironmentVariables[$k] = $Env[$k] }
        $p = [Diagnostics.Process]::Start($psi)
        $err = $p.StandardError.ReadToEndAsync()
        $out = $p.StandardOutput.ReadToEnd()
        $p.WaitForExit()
        return [pscustomobject]@{ Code = $p.ExitCode; Out = $out; Err = $err.Result }
    }
    $Want = [ordered]@{ CHARSIU_NPU = '1'; CHARSIU_NPU_QUANT = '1'; CHARSIU_NPU_W4V = '1'; CHARSIU_NPU_KMAX = '1024'
                        CHARSIU_NPU_W4_GROUP = '1024'; CHARSIU_NPU_MAXN = '262144'; CHARSIU_COEF_ELEMS = '65536' }
    function Test-Env([string]$Line, [string]$Name, [string]$Value) {
        return [regex]::IsMatch($Line, "(^| )$([regex]::Escape($Name))=$([regex]::Escape($Value))( |$)")
    }

    if (-not $Npu) {
        Write-Host '(no -Npu: shim tests skipped)'
    } else {
        $Npu = (Resolve-Path $Npu).Path
        $shim = Join-Path $Npu 'bin\charsiu.cmd'
        foreach ($f in 'bin\charsiu.cmd', 'charsiu.ps1', 'charsiu_run.exe', 'charsiu_serve.exe', 'charsiu_check.exe', 'config.ini', 'zoo.txt', 'VERSION') {
            Check "the bundle has npu\$f" (Test-Path (Join-Path $Npu $f))
        }
        # not %TEMP%: on a runner that is an 8.3 name (RUNNER~1)
        $tmp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
        $W = Join-Path $tmp "rk3576 charsiu test $PID"
        if (Test-Path $W) { Remove-Item $W -Recurse -Force }
        New-Item -ItemType Directory -Force -Path "$W\home", "$W\a model dir" | Out-Null
        $model = Join-Path $W 'a model dir\tiny.gguf'
        [IO.File]::WriteAllBytes($model, [byte[]](71, 71, 85, 70))
        $model = (Resolve-Path -LiteralPath $model).Path
        $base = @{ CHARSIU_HOME = "$W\home" }

        $ver = (Get-Content (Join-Path $Npu 'VERSION') -TotalCount 1).Trim()
        $r = Invoke-Cmd "`"$shim`" --version" $base
        Eq 'charsiu --version' ($r.Out.Trim()) "charsiu $ver"
        Eq 'charsiu --version exit code' $r.Code 0

        $r = Invoke-Cmd "`"$shim`" run `"$model`" --show `"two words`"" $base
        $lines = @($r.Out -split "`r?`n" | Where-Object { $_ })
        Eq 'run --show exit code' $r.Code 0
        $envLine = [string]$lines[0]
        Check 'the first line is the environment' ($envLine.StartsWith('env ')) $r.Out
        foreach ($k in $Want.Keys) { Check "sets $k=$($Want[$k])" (Test-Env $envLine $k $Want[$k]) $envLine }
        $cmdLine = [string]$lines[1]
        Check 'runs npu\charsiu_run.exe' ($cmdLine.Contains((Join-Path $Npu 'charsiu_run.exe'))) $cmdLine
        Check 'the model path with spaces arrives whole' ($cmdLine.Contains("`"$model`"")) $cmdLine
        Check 'the question arrives as one argument' ($cmdLine.Contains('-p "two words"')) $cmdLine

        $r = Invoke-Cmd "`"$shim`" run `"$model`" --show hello" ($base + @{ CHARSIU_NPU_KMAX = '512' })
        $lines = @($r.Out -split "`r?`n" | Where-Object { $_ })
        Check 'a variable already set is not overridden' (-not ([string]$lines[0]).Contains('CHARSIU_NPU_KMAX=1024')) $lines[0]
        Check 'and is named as coming from the environment' ($r.Out -match '# from the environment, not the config: CHARSIU_NPU_KMAX') $r.Out

        [IO.File]::WriteAllText("$W\home\config.ini", "[npu]`r`nenable = no`r`n")
        $r = Invoke-Cmd "`"$shim`" run `"$model`" --show hello" $base
        Check '[npu] enable = no: no NPU variables' (-not (Test-Env ([string](@($r.Out -split "`r?`n")[0])) 'CHARSIU_NPU' '1')) $r.Out
        Remove-Item "$W\home\config.ini"

        $r = Invoke-Cmd "`"$shim`" run `"$W\nope.gguf`" --show" $base
        Check 'a missing model: an error and a nonzero exit' ($r.Code -ne 0 -and $r.Err -match 'does not exist') "$($r.Code) $($r.Err)"

        Remove-Item $W -Recurse -Force
    }

    # --- an installed system ----------------------------------------------------
    if ($Installed) {
        $mp = Get-MachinePath
        $k = Get-PathKey $NpuBin
        $n = @(($mp -split ';') | Where-Object { $_ -and (Get-PathKey $_) -eq $k }).Count
        Eq 'the machine Path has npu\bin once' $n 1
        $kind = ([Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($EnvKey)).GetValueKind('Path')
        Eq 'and is still REG_EXPAND_SZ' "$kind" 'ExpandString'
        # a new console: the machine Path and nothing of this process' own
        $path = [Environment]::ExpandEnvironmentVariables($mp)
        $r = Invoke-Cmd 'charsiu --version' @{ PATH = $path; CHARSIU_HOME = (Join-Path ([IO.Path]::GetTempPath()) 'rk3576-charsiu-home') }
        Check 'a new console finds charsiu' ($r.Code -eq 0 -and $r.Out -match '^charsiu ') "$($r.Code) $($r.Out) $($r.Err)"
    }
}

Write-Host ''
Write-Host "$script:Passed passed, $script:Failed failed"
if ($script:Failed) { exit 1 }
exit 0
