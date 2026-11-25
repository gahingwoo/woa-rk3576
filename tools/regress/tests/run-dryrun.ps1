# run-dryrun.ps1 - the suite's own test, no board needed: every .ps1 under
# tools\regress must parse, and regress.ps1 -DryRun is run against each
# tests\mock-*.json. A mock lists the result it expects for some check ids
# ("expect") and the exit code; any difference is a failure.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-dryrun.ps1
#
# Windows PowerShell 5.1 (CI, windows-latest) and PowerShell 7 both run it.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fail = 0

foreach ($f in Get-ChildItem $root -Recurse -Filter *.ps1) {
    $tokens = $null; $errs = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errs)
    if ($errs) {
        foreach ($e in $errs) { Write-Host "PARSE $($f.Name):$($e.Extent.StartLineNumber): $($e.Message)" }
        $fail++
    } else { Write-Host "parse ok  $($f.Name)" }
    # Non-ASCII text needs a BOM, or Windows PowerShell 5.1 reads it as ANSI
    $b = [System.IO.File]::ReadAllBytes($f.FullName)
    $bom = $b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF
    $nonAscii = $false
    for ($i = 0; $i -lt $b.Length; $i++) { if ($b[$i] -ge 0x80) { $nonAscii = $true; break } }
    if ($nonAscii -and -not $bom) { Write-Host "ENCODING $($f.Name): non-ASCII without a UTF-8 BOM"; $fail++ }
}

# the PowerShell to run the suite with: the same one running this script
$ps = (Get-Process -Id $PID).Path
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('rk3576-regress-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
foreach ($m in Get-ChildItem (Join-Path $PSScriptRoot '.') -Filter 'mock-*.json') {
    $mock = Get-Content -Raw -Encoding UTF8 $m.FullName | ConvertFrom-Json
    $out = Join-Path $tmp $m.BaseName
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'regress.ps1'), '-DryRun', '-Mock', $m.FullName, '-Out', $out)
    if ($mock.args) { $a += @($mock.args) }
    Write-Host ''
    Write-Host "== $($m.Name): $($mock.comment)"
    # run from an empty folder: the suite must write nothing outside -Out
    $cwd = Join-Path $tmp "$($m.BaseName)-cwd"
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null
    Push-Location $cwd
    try { & $ps @a | Out-Host; $code = $LASTEXITCODE } finally { Pop-Location }
    $stray = @(Get-ChildItem -Force $cwd)
    if ($stray.Count) { Write-Host "STRAY $($m.Name): wrote $($stray.Count) item(s) into the working folder: $(($stray | Select-Object -First 3 | ForEach-Object { $_.Name }) -join ', ')"; $fail++ }
    if ($code -ne [int]$mock.exitCode) { Write-Host "EXIT $($m.Name): $code, expected $($mock.exitCode)"; $fail++ }
    $rj = Join-Path $out 'results.json'
    if (-not (Test-Path $rj)) { Write-Host "MISSING $rj"; $fail++; continue }
    if (-not (Test-Path (Join-Path $out 'results.md'))) { Write-Host "MISSING results.md"; $fail++ }
    $res = Get-Content -Raw -Encoding UTF8 $rj | ConvertFrom-Json
    $byId = @{}
    foreach ($r in @($res.results)) {
        if ($byId.ContainsKey($r.id)) { Write-Host "DUPLICATE id $($r.id)"; $fail++ }
        $byId[$r.id] = $r
        if (@('PASS', 'FAIL', 'SKIP', 'N-A') -notcontains $r.status) { Write-Host "STATUS $($r.id): '$($r.status)'"; $fail++ }
    }
    foreach ($p in $mock.expect.PSObject.Properties) {
        if (-not $byId.ContainsKey($p.Name)) { Write-Host "NO ROW $($m.Name): $($p.Name)"; $fail++; continue }
        $got = $byId[$p.Name].status
        if ($got -ne $p.Value) { Write-Host "WRONG $($m.Name): $($p.Name) is $got, expected $($p.Value) ($($byId[$p.Name].reason))"; $fail++ }
    }
    Write-Host "checked $($mock.expect.PSObject.Properties.Name.Count) expectations, $(@($res.results).Count) rows"
}
# Run-Tool for real, outside -DryRun: the exit code of a tool that exits at
# once must come back (Start-Process lost it for rknputest info on the
# 2026-11-25 board run), as must its output, a timeout and the arguments.
Write-Host ''
Write-Host '== Run-Tool, live'
$tokens = $null; $errs = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'regress.ps1'), [ref]$tokens, [ref]$errs)
foreach ($fn in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and @('Run-Tool', 'Join-Arguments') -contains $n.Name }, $true)) {
    . ([scriptblock]::Create($fn.Extent.Text))
}
$script:DryRunMode = $false
$script:OutDir = Join-Path $tmp 'runtool'
New-Item -ItemType Directory -Force -Path $script:OutDir | Out-Null
$onWindows = $env:OS -eq 'Windows_NT'
function Shell-Exe { if ($onWindows) { return (Join-Path $env:SystemRoot 'System32\cmd.exe') } return '/bin/sh' }
function Shell-Args([string]$cmd) { if ($onWindows) { return @('/c', $cmd) } return @('-c', $cmd) }
$cases = @(
    @{ name = 'exit 0 at once'; cmd = 'exit 0'; code = 0 },
    @{ name = 'exit 3 at once'; cmd = 'exit 3'; code = 3 },
    @{ name = 'output and exit 2'; cmd = 'echo RKNPU INFO: OK&& exit 2'; code = 2; out = 'RKNPU INFO: OK' }
)
if (-not $onWindows) { $cases[2].cmd = 'echo RKNPU INFO: OK; exit 2' }
foreach ($c in $cases) {
    # many times: the lost exit code was a race with a tool that exits at once
    for ($i = 0; $i -lt 20; $i++) {
        $r = Run-Tool "t$i" (Shell-Exe) (Shell-Args $c.cmd) 30
        if ($r.ExitCode -isnot [int] -or $r.ExitCode -ne $c.code -or $r.TimedOut) {
            Write-Host "RUN-TOOL $($c.name): exit '$($r.ExitCode)' timedOut $($r.TimedOut), expected $($c.code) (try $i)"; $fail++; break
        }
        if ($c.out -and $r.Output -notmatch [regex]::Escape($c.out)) { Write-Host "RUN-TOOL $($c.name): output '$($r.Output)'"; $fail++; break }
    }
    Write-Host "run-tool  $($c.name)"
}
$slow = @('/c', 'ping -n 30 127.0.0.1 >nul'); if (-not $onWindows) { $slow = @('-c', 'sleep 30') }
$t0 = Get-Date
$r = Run-Tool 'slow' (Shell-Exe) $slow 2
if (-not $r.TimedOut -or $r.ExitCode -ne -1 -or ((Get-Date) - $t0).TotalSeconds -gt 20) { Write-Host "RUN-TOOL timeout: timedOut $($r.TimedOut) exit $($r.ExitCode)"; $fail++ } else { Write-Host 'run-tool  timeout' }
$r = Run-Tool 'missing' (Join-Path $tmp 'no-such-tool.exe') @('info') 5
if ($r.ExitCode -ne -1) { Write-Host "RUN-TOOL missing tool: exit $($r.ExitCode)"; $fail++ } else { Write-Host 'run-tool  missing tool' }
# arguments with spaces, quotes and trailing backslashes arrive as they were
$argv = @('a b', 'say "hi"', 'C:\dir with space\', '')
$line = Join-Arguments $argv
$want = '"a b" "say \"hi\"" "C:\dir with space\\" ""'
if ($line -ne $want) { Write-Host "JOIN-ARGUMENTS: [$line], expected [$want]"; $fail++ } else { Write-Host 'run-tool  argument quoting' }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
Write-Host ''
if ($fail) { Write-Host "run-dryrun: $fail problem(s)"; exit 1 }
Write-Host 'run-dryrun: all good'
exit 0
