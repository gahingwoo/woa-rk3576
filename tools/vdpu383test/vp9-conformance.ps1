# vp9-conformance.ps1 - every stream of vp9-conformance.txt through
# vdpu383test on the board; MATCH when the output MD5 is FFmpeg's.
#
#   powershell -ExecutionPolicy Bypass -File vp9-conformance.ps1 -Dir D:\vp9\streams [-DxvaOnly] [-Only vp90-2-00] [-Extra '--vp9-mpp']
#
# Writes NAME.txt (vdpu383test's output) and vp9-results.txt into -Out,
# then info-after.txt (`vdpu383test info`), and prints one line per stream.
# -Extra passes more options to every decode (for example --vp9-mpp, or
# --stride mpp) so that a second run can be compared with the first.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string]$Dir = ".",
    [string]$Exe = "vdpu383test.exe",
    [string]$Out = ".\vp9-results",
    [string]$Only = "",
    [string]$Extra = "",
    [switch]$DxvaOnly
)

$list = Join-Path $PSScriptRoot "vp9-conformance.txt"
New-Item -ItemType Directory -Force $Out | Out-Null
$results = @()
$bad = 0
foreach ($line in Get-Content $list) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split '\s+'
    $name = $f[0]; $frames = [int]$f[1]; $md5 = $f[2]; $size = $f[3]; $fmt = $f[4]
    if ($Only -and -not $name.StartsWith($Only)) { continue }
    $ivf = Join-Path $Dir "$name.ivf"
    if (-not (Test-Path $ivf)) { Write-Host "$name : missing $ivf"; $bad++; continue }
    $a = @('decode', $ivf)
    if ($DxvaOnly) { $a += '--dxva-only' }
    if ($Extra) { $a += ($Extra -split '\s+') }
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
    $r = '{0,-36} {1,-8} frames {2,4}/{3,-4} hw errors {4,3} exit {5} {6,6} ms {7} {8}' -f `
        $name, ($(if ($ok) { 'MATCH' } else { 'MISMATCH' })), $gotFrames, $frames, $hwErr, $code, $ms, $size, $fmt
    Write-Host $r
    $results += $r
}
& $Exe info > (Join-Path $Out "info-after.txt") 2>&1
$results += "streams not matching: $bad"
$results | Set-Content (Join-Path $Out "vp9-results.txt")
Write-Host "streams not matching: $bad"
exit $bad
