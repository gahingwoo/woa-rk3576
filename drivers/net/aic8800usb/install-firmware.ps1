# install-firmware.ps1 - fetch the AIC8800D80 firmware for aic8800usb and put
# it where the driver looks for it.
#
# The files are AICSemi firmware: the Wi-Fi firmware (fmacfw), the ROM
# patches and the patch table (fw_patch*, fw_adid) that also bring up the
# Bluetooth half. None of them is part of this repository or its driver
# packages: AICSemi publishes no licence for them. This script downloads
# them on the machine it runs on, from Radxa's radxa-pkg/aic8800 at a pinned
# commit (the files Radxa's own images ship), and refuses any file whose
# SHA-256 or size is not the one written here.
#
#   -Dir PATH   use files you already have instead (not hash-checked here)
#
# Run as administrator, then restart Windows:
#   powershell -ExecutionPolicy Bypass -File install-firmware.ps1
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string] $Dir = ''
)

$ErrorActionPreference = 'Stop'

$radxa = 'https://raw.githubusercontent.com/radxa-pkg/aic8800/5562c254b0231be34588dc608ec28c4f1d613285/src/USB/driver_fw/fw/aic8800D80'

# fmacfw_8800d80_h_u02.bin is for the "H" chips (chip revision bits 0xC0);
# the driver records ChipIsH and loads the one it needs.
$files = @(
    @{ Name = 'fw_patch_table_8800d80_u02.bin'; Size = 1384
       Sha256 = '9DECB77435B7E9713E33E32DA483D683B7329ED93B672B2D1B134031D7DA5F67' },
    @{ Name = 'fw_adid_8800d80_u02.bin'; Size = 1708
       Sha256 = 'A526CBD02FCDC495F049F3AD6B5933CB08CD984B16790C716A060D582FEE1A56' },
    @{ Name = 'fw_patch_8800d80_u02.bin'; Size = 32700
       Sha256 = 'F0E2F5BBC17BC327CA7F1574FF55370DFD863D931514347BB4ABC18A74F6218F' },
    @{ Name = 'fw_patch_8800d80_u02_ext0.bin'; Size = 16136
       Sha256 = 'CA738F5EA5ACA6021C4F48B60FF35E835DFDAF072A8490E3BA8BA0A42440E3E5' },
    @{ Name = 'fmacfw_8800d80_u02.bin'; Size = 358072
       Sha256 = '23F2BED2AC0E8F7BDA8F36DF7DEFD30D9B928C97867F6F0F01F28D89DD78D670' },
    @{ Name = 'fmacfw_8800d80_h_u02.bin'; Size = 358640
       Sha256 = 'C5C8C774DFAE93AED71F8DA7489E35159192135E4929591EA8A945B76DEBA6BD' }
)

$destDir = Join-Path $env:SystemRoot 'System32\drivers\aic8800'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null

if ($Dir -ne '') {
    foreach ($f in $files) {
        $src = Join-Path $Dir $f.Name
        if (-not (Test-Path -LiteralPath $src)) { throw "missing $src" }
        Copy-Item -LiteralPath $src -Destination (Join-Path $destDir $f.Name) -Force
        Write-Host "Installed $($f.Name) from $Dir"
    }
} else {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    foreach ($f in $files) {
        $url = "$radxa/$($f.Name)"
        $tmp = Join-Path $env:TEMP "$($f.Name).download"
        Write-Host "Downloading $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tmp
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash
        $size = (Get-Item -LiteralPath $tmp).Length
        if ($hash -ne $f.Sha256 -or $size -ne $f.Size) {
            Remove-Item -LiteralPath $tmp -Force
            throw "$($f.Name) does not match: SHA-256 $hash, $size bytes"
        }
        Move-Item -LiteralPath $tmp -Destination (Join-Path $destDir $f.Name) -Force
        Write-Host "Installed $($f.Name) (SHA-256 ok)"
    }
}

# The loader instance reads the files when it starts, which is at boot.
Write-Host 'Done. Restart Windows for aic8800usb to load the files.'
