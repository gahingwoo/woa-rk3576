# install-firmware.ps1 - fetch the Bluetooth patchram for bcm4345bt and put
# it where the driver looks for it: BCM4345C5.hcd (TaishanPi 3M, default)
# or, with -Chip 4362a2, BCM4362A2.hcd (ArmSoM CM5 on the CM5-IO).
#
# The patchram is Broadcom/Cypress firmware and is not part of this
# repository or its driver packages. This script downloads it on the machine
# it is installed on, from a pinned commit, and refuses it unless its SHA-256
# is the one written here.
#
#   -Source ampak  (default) Ampak's BCM4345C5 build for the AP6256 module,
#                  "BCM4345C5 Ampak_CL1 UART 37.4 MHz", the file Armbian
#                  ships and Linux loads on the TaishanPi 3M. Armbian's
#                  firmware repository gives no licence for it.
#   -Source rpi    Raspberry Pi's BCM43456 build ("BCM43456 37.4MHz
#                  Raspberry Pi"), for the Pi 400's CYW43456. Distributed
#                  under Cypress's licence (LICENSE.cypress next to it in
#                  RPi-Distro/bluez-firmware). A different board's RF
#                  tuning: try it only if the Ampak file misbehaves.
#   -File PATH     use a file you already have (checked with the walker's
#                  rules only by the driver, not by hash).
#   -Chip 4362a2   the CM5-IO's BCM43752, whose Bluetooth core Linux names
#                  BCM4362A2: Ampak's "BCM43752A2 UART 37.4MHz Ampak AP6398
#                  sLNA iLNA CL1" build from armbian/firmware, the file
#                  Linux's btbcm asks for on this chip. Not tuned for the
#                  CM5's BW3752-50B1 module. No licence given. Only
#                  -Source ampak exists for it.
#
# Run as administrator, then restart the device (or reboot):
#   powershell -ExecutionPolicy Bypass -File install-firmware.ps1
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [ValidateSet('ampak', 'rpi')] [string] $Source = 'ampak',
    [ValidateSet('4345c5', '4362a2')] [string] $Chip = '4345c5',
    [string] $File = ''
)

$ErrorActionPreference = 'Stop'

$sources = @{
    ampak = @{
        Url    = 'https://raw.githubusercontent.com/armbian/firmware/6b6f053f6089e08dd2a675cda1ec813de2e842e2/brcm/BCM4345C5.hcd'
        Sha256 = 'F67164F0EDA8D4CA96305E177A61542BF8B470F2F1C456B66FE8C660650F1C7A'
        Size   = 45667
    }
    rpi = @{
        Url    = 'https://raw.githubusercontent.com/RPi-Distro/bluez-firmware/cdf61dc691a49ff01a124752bd04194907f0f9cd/debian/firmware/broadcom/BCM4345C5.hcd'
        Sha256 = 'FB9F4EC2DF5301BD35F416384E103C012C5983024C49AA72FBBAF90177512CAA'
        Size   = 55676
    }
    ampak4362a2 = @{
        Url    = 'https://raw.githubusercontent.com/armbian/firmware/db8cc456dad5ad511b04f8a5130acda392cad20b/brcm/BCM4362A2.hcd'
        Sha256 = '18901A5BEF1D418B6895E92D0AFAE36234F4160B237465DFCA3D75E9844E93EF'
        Size   = 80602
    }
}

if ($Chip -eq '4362a2') {
    if ($Source -ne 'ampak') { throw "-Source $Source has no file for the BCM4362A2; use ampak" }
    $key = 'ampak4362a2'
    $fileName = 'BCM4362A2.hcd'
    $hwid = 'ACPI\RKCP4362\*'
} else {
    $key = $Source
    $fileName = 'BCM4345C5.hcd'
    $hwid = 'ACPI\RKCP4345\*'
}

$destDir = Join-Path $env:SystemRoot 'System32\drivers\brcm'
$dest = Join-Path $destDir $fileName
$tmp = Join-Path $env:TEMP "$fileName.download"

if ($File -ne '') {
    Copy-Item -LiteralPath $File -Destination $tmp -Force
} else {
    $s = $sources[$key]
    Write-Host "Downloading $($s.Url)"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $s.Url -OutFile $tmp
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash
    $size = (Get-Item -LiteralPath $tmp).Length
    if ($hash -ne $s.Sha256 -or $size -ne $s.Size) {
        Remove-Item -LiteralPath $tmp -Force
        throw "Downloaded file does not match: SHA-256 $hash, $size bytes"
    }
    Write-Host "SHA-256 ok ($hash)"
}

New-Item -ItemType Directory -Force -Path $destDir | Out-Null
Move-Item -LiteralPath $tmp -Destination $dest -Force
Write-Host "Installed $dest"

$dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
       Where-Object { $_.InstanceId -like $hwid }
if ($dev) {
    Write-Host "Restarting $($dev.FriendlyName) so it loads the patchram"
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
} else {
    Write-Host 'bcm4345bt is not installed yet; the patchram is used from its first start.'
}
