# build-woa.ps1 - build and test-sign this repository's drivers for the
# driver package, from the sources of this checkout.
#
#   build-woa.ps1 -Table drivers.tsv -Out DIR -Build N
#
# Builds every driver of drivers.tsv whose source is "woa", and writes
# DIR\woa-drivers\<dir>\ (the .inf, .sys and .cat of each) and
# DIR\woa-drivers\signer.cer, the layout assemble.py --woa reads. A <dir> is
# found, in this order:
#   - in build\RockchipDrivers.sln's output (build\ARM64\Release\Output\<dir>)
#   - as drivers\**\<dir>.vcxproj, built on its own
#   - as drivers\**\<dir>.inf with no project: an INF-only (null driver)
#     package, whose DriverVer is stamped here
# N goes into DriverVer, so a package outranks the one installed before it.
# Every package is signed with one certificate made for this run.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Table,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][int]$Build,
    [string]$Wdk = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repo

$dirs = @()
foreach ($line in Get-Content -LiteralPath $Table) {
    $s = $line.Trim()
    if (-not $s -or $s.StartsWith('#')) { continue }
    $f = $s -split '\s+', 5
    if ($f.Count -ne 5) { throw "drivers.tsv: want 5 columns: $s" }
    if ($f[3] -eq 'woa') { $dirs += $f[0] }
}
if (-not $dirs) { throw 'drivers.tsv names no woa driver' }

$stage = Join-Path $Out 'woa-drivers'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Copy-Package([string]$from, [string]$to) {
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Get-ChildItem $from -Recurse -File -Include *.sys, *.inf, *.cat |
        Copy-Item -Destination $to -Force
}

# The symbols of each driver go to DIR\pdb\<dir>, outside the package, so a
# disassembly of the shipped .sys can be read with function names (the
# alignment audit of the code the compiler actually emitted).
$pdbOut = Join-Path $Out 'pdb'
if (Test-Path $pdbOut) { Remove-Item -Recurse -Force $pdbOut }
function Copy-Pdb([string]$d, [string[]]$from) {
    $to = Join-Path $pdbOut $d
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    foreach ($f in $from) {
        if (Test-Path $f) {
            Get-ChildItem $f -Recurse -File -Filter "$d.pdb" |
                Sort-Object LastWriteTime | Copy-Item -Destination $to -Force
        }
    }
}

Write-Host '== build\RockchipDrivers.sln'
msbuild build\RockchipDrivers.sln /p:Configuration=Release /p:Platform=ARM64 `
        /p:SignMode=Off /p:WoaDriverBuild=$Build /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'msbuild failed for RockchipDrivers.sln' }

$infOnly = @()
foreach ($d in $dirs) {
    $dst = Join-Path $stage $d
    $sol = Join-Path 'build\ARM64\Release\Output' $d
    if (Test-Path (Join-Path $sol "$d.sys")) {
        Write-Host "== $d (solution)"
        Copy-Package $sol $dst
        Copy-Pdb $d @('build', 'drivers')
        continue
    }
    $proj = @(Get-ChildItem drivers -Recurse -File -Filter "$d.vcxproj")
    if ($proj.Count -gt 1) { throw "${d}: more than one ${d}.vcxproj" }
    if ($proj.Count -eq 1) {
        Write-Host "== $d ($($proj[0].FullName.Substring($repo.Length + 1)))"
        msbuild $proj[0].FullName /p:Configuration=Release /p:Platform=ARM64 `
                /p:WindowsTargetPlatformVersion=$Wdk /p:SignMode=Off /m /nologo /v:minimal
        if ($LASTEXITCODE -ne 0) { throw "msbuild failed for $d" }
        Copy-Package (Join-Path $proj[0].DirectoryName 'ARM64\Release') $dst
        Copy-Pdb $d @($proj[0].DirectoryName)
        continue
    }
    $inf = @(Get-ChildItem drivers -Recurse -File -Filter "$d.inf")
    if ($inf.Count -ne 1) { throw "${d}: no project and no single ${d}.inf under drivers" }
    Write-Host "== $d (INF only, $($inf[0].FullName.Substring($repo.Length + 1)))"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    $to = Join-Path $dst "$d.inf"
    $dv = 'DriverVer   = {0},1.0.{1}.0' -f (Get-Date -Format 'MM/dd/yyyy'), $Build
    (Get-Content -LiteralPath $inf[0].FullName) -replace '^DriverVer\s*=.*$', $dv |
        Set-Content -Encoding ascii $to
    if (-not (Select-String -LiteralPath $to -Pattern '^DriverVer\s*=\s*\d')) { throw "${d}: no DriverVer line to stamp" }
    $infOnly += $d
}

$kits = 'C:\Program Files (x86)\Windows Kits\10'
$st   = Join-Path $kits "bin\$Wdk\x86\signtool.exe"
$i2c  = Join-Path $kits "bin\$Wdk\x86\inf2cat.exe"
foreach ($t in $st, $i2c) { if (-not (Test-Path $t)) { throw "missing $t" } }

$pw = [guid]::NewGuid().ToString()
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=woa-rk3576 test signing' `
          -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
$pfx = Join-Path $env:RUNNER_TEMP 'woa-pkg.pfx'
if (-not $env:RUNNER_TEMP) { $pfx = Join-Path $env:TEMP 'woa-pkg.pfx' }
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (ConvertTo-SecureString $pw -Force -AsPlainText) | Out-Null
Export-Certificate -Cert $cert -FilePath (Join-Path $stage 'signer.cer') | Out-Null

foreach ($d in $dirs) {
    $p = Join-Path $stage $d
    Get-ChildItem $p -Filter *.cat | Remove-Item -Force
    # 10_RS3_ARM64 is build 16299, the floor of these INFs (as winpe.yml).
    # The null INF is catalogued for every ARM64 release up to 23H2 (10_NI),
    # as ci.yml's null-devices job does.
    $os = if ($infOnly -contains $d) { '10_RS3_ARM64,10_RS4_ARM64,10_RS5_ARM64,10_19H1_ARM64,10_VB_ARM64,10_CO_ARM64,10_NI_ARM64' } else { '10_RS3_ARM64' }
    & $i2c /driver:"$p" /os:$os
    if ($LASTEXITCODE) { throw "inf2cat failed for ${d}: $LASTEXITCODE" }
    $files = @(Get-ChildItem $p -File -Include *.cat, *.sys -Recurse | Select-Object -ExpandProperty FullName)
    & $st sign /fd sha256 /f $pfx /p $pw $files
    if ($LASTEXITCODE) { throw "signtool failed for ${d}: $LASTEXITCODE" }
    foreach ($sig in Get-AuthenticodeSignature $files) {
        Write-Host ("   {0,-14} {1}" -f $sig.Status, (Split-Path $sig.Path -Leaf))
        if ($sig.Status -eq 'NotSigned') { throw "$($sig.Path) came out unsigned" }
    }
    if (-not (Get-ChildItem $p -Filter *.inf)) { throw "${d}: no .inf" }
    if (-not (Get-ChildItem $p -Filter *.cat)) { throw "${d}: no .cat" }
    if (($infOnly -notcontains $d) -and -not (Get-ChildItem $p -Filter *.sys)) { throw "${d}: no .sys" }
}
Remove-Item $pfx -Force
Get-ChildItem $stage -Recurse -File | Select-Object FullName, Length | Format-Table -AutoSize | Out-String | Write-Host
