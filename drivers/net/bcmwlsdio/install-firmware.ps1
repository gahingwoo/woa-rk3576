# install-firmware.ps1 - fetch the Wi-Fi firmware for bcmwlsdio and put it
# where the driver looks for it: the BCM43456 (TaishanPi 3M, default) or,
# with -Chip 43752, the BCM43752 (ArmSoM CM5 on the CM5-IO).
#
# The three files are Broadcom/Cypress/Synaptics firmware and AMPAK board
# data. None of them is part of this repository or its driver packages. This
# script downloads them on the machine it runs on, from pinned commits, and
# refuses any file whose SHA-256 is not the one written here.
#
#   -Source armbian  (default) all three from armbian/firmware: the AP6256
#                    build Linux loads on the TaishanPi 3M under Armbian
#                    (firmware 7.45.96.0, AMPAK NVRAM "AP6256_NVRAM_V1.1").
#                    Armbian's firmware repository gives no licence for them.
#   -Source rpi      firmware and CLM blob from Raspberry Pi's
#                    firmware-nonfree (7.45.96.252, Synaptics licence:
#                    redistribution in object code, for use with Synaptics
#                    chips), with Armbian's AP6256 NVRAM. Raspberry Pi's own
#                    NVRAM is for the Pi 400's RF layout and is not used.
#   -Dir PATH        use brcmfmac<chip>-sdio.bin/.txt/.clm_blob you already
#                    have (not hash-checked here; the driver checks their form)
#
#   -Chip 43752      the CM5-IO's files, all three from armbian/firmware at
#                    the same commit: AMPAK's AP6275S (BCM43752 rev 2) build,
#                    firmware 18.35.387.23.7 ("4362a2-roml", 2020-07-13),
#                    NVRAM "AP6275S_NVRAM_V1.7_20210726". The CM5's module is
#                    a BW3752-50B1, not an AP6275S: this is the set brcmfmac
#                    ran on the CM5-IO under Fedora, not one tuned for its
#                    RF layout. No licence given. -Source
#                    rpi does not exist for this chip.
#
# Run as administrator, then restart Windows:
#   powershell -ExecutionPolicy Bypass -File install-firmware.ps1
#   powershell -ExecutionPolicy Bypass -File install-firmware.ps1 -Chip 43752
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [ValidateSet('armbian', 'rpi')] [string] $Source = 'armbian',
    [ValidateSet('43456', '43752')] [string] $Chip = '43456',
    [string] $Dir = ''
)

$ErrorActionPreference = 'Stop'

$armbian = 'https://raw.githubusercontent.com/armbian/firmware/db8cc456dad5ad511b04f8a5130acda392cad20b/brcm'
$rpi = 'https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/b1833f18a816cf8fd73ca86e27f9eecea11bf991/debian/added-firmware/brcm'

$files = @{
    armbian = @(
        @{ Name = 'brcmfmac43456-sdio.bin'; Url = "$armbian/brcmfmac43456-sdio.bin"; Size = 482927
           Sha256 = '3167956A7B2CFFC4CFCAF6A282B95728C529EEBEF18A5D9E6D9FF32DE32CC67C' },
        @{ Name = 'brcmfmac43456-sdio.clm_blob'; Url = "$armbian/brcmfmac43456-sdio.clm_blob"; Size = 7163
           Sha256 = '2DBD7D22FC9AF0EB560CEAB45B19646D211BC7B34A1DD00C6BFAC5DD6BA25E8A' },
        @{ Name = 'brcmfmac43456-sdio.txt'; Url = "$armbian/brcmfmac43456-sdio.txt"; Size = 2099
           Sha256 = '66C71EB53B47C49D42386B66735134578640B0946F3F46E44F384FEF5AACFD9E' }
    )
    rpi = @(
        @{ Name = 'brcmfmac43456-sdio.bin'; Url = "$rpi/brcmfmac43456-sdio.bin"; Size = 593869
           Sha256 = 'DC4429F13B59F8C271E940C28B2A197455A8E02270FCC7A2F9036AA1E4C739FB' },
        @{ Name = 'brcmfmac43456-sdio.clm_blob'; Url = "$rpi/brcmfmac43456-sdio.clm_blob"; Size = 7163
           Sha256 = '2DBD7D22FC9AF0EB560CEAB45B19646D211BC7B34A1DD00C6BFAC5DD6BA25E8A' },
        @{ Name = 'brcmfmac43456-sdio.txt'; Url = "$armbian/brcmfmac43456-sdio.txt"; Size = 2099
           Sha256 = '66C71EB53B47C49D42386B66735134578640B0946F3F46E44F384FEF5AACFD9E' }
    )
    armbian43752 = @(
        @{ Name = 'brcmfmac43752-sdio.bin'; Url = "$armbian/brcmfmac43752-sdio.bin"; Size = 743607
           Sha256 = '46F62076768E50938D0E29B306B24D4663DE20B07B474C4759D5801FCBF0BDDE' },
        @{ Name = 'brcmfmac43752-sdio.clm_blob'; Url = "$armbian/brcmfmac43752-sdio.clm_blob"; Size = 29225
           Sha256 = '5143146E1923F87F7AAB8DF043ABCF89A657FA9FDC3B22A38806399730D9A97A' },
        @{ Name = 'brcmfmac43752-sdio.txt'; Url = "$armbian/brcmfmac43752-sdio.txt"; Size = 8355
           Sha256 = '2D2723101FE9C66C853DDB1E2D715851BA100A4390F8AC72FC84DD35736CC66F' }
    )
}

if ($Chip -eq '43752') {
    if ($Source -ne 'armbian') { throw "-Source $Source has no files for the BCM43752; use armbian" }
    $set = 'armbian43752'
} else {
    $set = $Source
}

$destDir = Join-Path $env:SystemRoot 'System32\drivers\brcm'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null

if ($Dir -ne '') {
    foreach ($n in "brcmfmac$Chip-sdio.bin", "brcmfmac$Chip-sdio.clm_blob", "brcmfmac$Chip-sdio.txt") {
        $src = Join-Path $Dir $n
        if (-not (Test-Path -LiteralPath $src)) { throw "missing $src" }
        Copy-Item -LiteralPath $src -Destination (Join-Path $destDir $n) -Force
        Write-Host "Installed $n from $Dir"
    }
} else {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    foreach ($f in $files[$set]) {
        $tmp = Join-Path $env:TEMP "$($f.Name).download"
        Write-Host "Downloading $($f.Url)"
        Invoke-WebRequest -UseBasicParsing -Uri $f.Url -OutFile $tmp
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

# The driver reads the files when it starts. Restart Windows rather than
# disabling and enabling the device: a second start on a chip that is
# already running firmware has not been tried on the board.
Write-Host 'Done. Restart Windows for bcmwlsdio to load the files.'
