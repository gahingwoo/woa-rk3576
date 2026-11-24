# hevc-conformance.ps1 - every stream of hevc-conformance.txt through
# vdpu383test on the board; MATCH when the output MD5 is FFmpeg's.
#
#   powershell -ExecutionPolicy Bypass -File hevc-conformance.ps1 -Dir D:\hevc\streams [-DxvaOnly] [-Only RPS_]
#
# Writes NAME.txt (vdpu383test's output) next to the script's results file
# hevc-results.txt, and prints one line per stream.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string]$Dir = ".",
    [string]$Exe = "vdpu383test.exe",
    [string]$Out = ".\hevc-results",
    [string]$Only = "",
    [switch]$DxvaOnly
)

$list = Join-Path $PSScriptRoot "hevc-conformance.txt"
New-Item -ItemType Directory -Force $Out | Out-Null
$results = @()
$bad = 0
foreach ($line in Get-Content $list) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split '\s+'
    $name = $f[0]; $frames = [int]$f[1]; $md5 = $f[2]; $size = $f[3]
    if ($Only -and -not $name.StartsWith($Only)) { continue }
    $bit = Join-Path $Dir "$name.bit"
    if (-not (Test-Path $bit)) { Write-Host "$name : missing $bit"; $bad++; continue }
    $a = @('decode', $bit)
    if ($DxvaOnly) { $a += '--dxva-only' }
    $log = Join-Path $Out "$name.txt"
    $t0 = Get-Date
    & $Exe @a > $log 2>&1
    $code = $LASTEXITCODE
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    $text = Get-Content $log -Raw
    $gotMd5 = if ($text -match 'output md5 ([0-9a-f]{32})') { $Matches[1] } else { '' }
    $gotFrames = if ($text -match 'frames out (\d+)') { [int]$Matches[1] } else { -1 }
    $hwErr = if ($text -match 'hardware errors (\d+)') { [int]$Matches[1] } else { -1 }
    $ok = ($gotMd5 -eq $md5) -and ($gotFrames -eq $frames) -and ($hwErr -eq 0)
    if (-not $ok) { $bad++ }
    $r = '{0,-28} {1,-8} frames {2,4}/{3,-4} hw errors {4,3} exit {5} {6,6} ms {7}' -f `
        $name, ($(if ($ok) { 'MATCH' } else { 'MISMATCH' })), $gotFrames, $frames, $hwErr, $code, $ms, $size
    Write-Host $r
    $results += $r
}
$results += "streams not matching: $bad"
$results | Set-Content (Join-Path $Out "hevc-results.txt")
Write-Host "streams not matching: $bad"
exit $bad
