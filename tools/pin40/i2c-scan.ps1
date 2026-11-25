<#
.SYNOPSIS
  List the I2C and SPI buses applications can open, and scan an I2C bus
  for devices, through Windows.Devices.I2c.

.DESCRIPTION
  Needs the firmware option "40-pin GPIO/I2C/SPI for apps" on, the
  woa-rk3576 I2C driver (rk3xi2c) and Windows' rhproxy. Bus names are the
  ones in docs/pin40.md (I2C7, I2C8, ...). The scan reads one byte from
  each 7-bit address from 0x08 to 0x77 and lists the ones that answer.
  A read is harmless for most devices; do not scan a bus with a device that
  acts on reads alone.

  Run it in Windows PowerShell 5.1 (powershell.exe).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File i2c-scan.ps1
  powershell -ExecutionPolicy Bypass -File i2c-scan.ps1 -Bus I2C7

  Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
  SPDX-License-Identifier: GPL-2.0-only
#>
param(
    [string]$Bus = ''
)

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -eq 'Core') {
    throw 'Run this in Windows PowerShell 5.1 (powershell.exe); PowerShell 7 cannot load WinRT types.'
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.I2c.I2cDevice, Windows.Devices, ContentType = WindowsRuntime]
$null = [Windows.Devices.Spi.SpiDevice, Windows.Devices, ContentType = WindowsRuntime]
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices, ContentType = WindowsRuntime]

$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1

function Await($op, [Type]$type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $null = $t.Wait(-1)
    $t.Result
}

function Buses([string]$selector) {
    Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) `
          ([Windows.Devices.Enumeration.DeviceInformationCollection])
}

$i2c = Buses ([Windows.Devices.I2c.I2cDevice]::GetDeviceSelector())
$spi = Buses ([Windows.Devices.Spi.SpiDevice]::GetDeviceSelector())
"I2C buses: $(@($i2c | ForEach-Object { $_.Name }) -join ', ')"
"SPI buses: $(@($spi | ForEach-Object { $_.Name }) -join ', ')"
if (@($i2c).Count -eq 0) {
    throw 'No I2C bus for applications. See docs/pin40.md.'
}

$target = if ($Bus) { $i2c | Where-Object { $_.Name -eq $Bus } | Select-Object -First 1 } else { $i2c | Select-Object -First 1 }
if ($null -eq $target) {
    throw "No I2C bus named $Bus."
}

"scanning $($target.Name) at 100 kHz"
$found = @()
for ($addr = 0x08; $addr -le 0x77; $addr++) {
    $settings = New-Object Windows.Devices.I2c.I2cConnectionSettings($addr)
    $settings.BusSpeed = [Windows.Devices.I2c.I2cBusSpeed]::StandardMode
    $dev = Await ([Windows.Devices.I2c.I2cDevice]::FromIdAsync($target.Id, $settings)) ([Windows.Devices.I2c.I2cDevice])
    if ($null -eq $dev) {
        throw "Could not open $($target.Name) (in use by another program?)"
    }
    try {
        $buf = New-Object byte[] 1
        $r = $dev.ReadPartial($buf)
        if ($r.Status -eq [Windows.Devices.I2c.I2cTransferStatus]::FullTransfer) {
            $found += ('0x{0:x2}' -f $addr)
        }
    } finally {
        $dev.Dispose()
    }
}
if ($found.Count) { "answered: $($found -join ' ')" } else { 'nothing answered' }
