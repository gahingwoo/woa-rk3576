# av1-conformance.ps1 - every stream of av1-conformance.txt through
# vdpu383test on the board; MATCH when the output MD5 is dav1d's, GRAIN-OFF
# when it is dav1d's output without film grain (the decoder did not apply
# the grain, or applied it differently: the frames are right otherwise).
#
#   powershell -ExecutionPolicy Bypass -File av1-conformance.ps1 -Dir D:\av1\streams [-DxvaOnly] [-Synth 1] [-Only av1-1-b8]
#
# -DxvaOnly gives the builder only what a D3D11 driver has (DXVA_PicParams_AV1
# and the tile data, wrapped in OBUs by the builder); -Synth 1 wraps them in
# an OBU_FRAME instead of an OBU_TILE_GROUP. Writes NAME.txt and
# av1-results.txt into -Out, then info-after.txt.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string]$Dir = ".",
    [string]$Exe = "vdpu383test.exe",
    [string]$Out = ".\av1-results",
    [string]$Only = "",
    [string]$Extra = "",
    [string]$Synth = "",
    [switch]$DxvaOnly
)

$list = Join-Path $PSScriptRoot "av1-conformance.txt"
New-Item -ItemType Directory -Force $Out | Out-Null
$results = @()
$bad = 0
foreach ($line in Get-Content $list) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split '\s+'
    $name = $f[0]; $frames = [int]$f[1]; $md5 = $f[2]; $md5ng = $f[3]; $size = $f[4]; $fmt = $f[5]
    if ($Only -and -not $name.StartsWith($Only)) { continue }
    $ivf = Join-Path $Dir "$name.ivf"
    if (-not (Test-Path $ivf)) { Write-Host "$name : missing $ivf"; $bad++; continue }
    $a = @('decode', $ivf)
    if ($DxvaOnly) { $a += '--dxva-only' }
    if ($Synth) { $a += @('--av1-synth', $Synth) }
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
    $ok = ($gotFrames -eq $frames) -and ($hwErr -eq 0)
    $verdict = 'MISMATCH'
    if ($ok -and $gotMd5 -eq $md5) { $verdict = 'MATCH' }
    elseif ($ok -and $gotMd5 -eq $md5ng) { $verdict = 'GRAIN-OFF' }
    if ($verdict -eq 'MISMATCH') { $bad++ }
    $r = '{0,-32} {1,-9} frames {2,4}/{3,-4} hw errors {4,3} exit {5} {6,6} ms {7} {8}' -f `
        $name, $verdict, $gotFrames, $frames, $hwErr, $code, $ms, $size, $fmt
    Write-Host $r
    $results += $r
}
& $Exe info > (Join-Path $Out "info-after.txt") 2>&1
$results += "streams not matching: $bad"
$results | Set-Content (Join-Path $Out "av1-results.txt")
Write-Host "streams not matching: $bad"
exit $bad
