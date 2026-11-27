<#
.SYNOPSIS
  Blink (or read) a 40-pin header GPIO through Windows.Devices.Gpio.

.DESCRIPTION
  Needs the firmware option "40-pin GPIO/I2C/SPI for apps" on, the
  woa-rk3576 GPIO driver (rk3xgpio) and Windows' rhproxy. Pin numbers are
  the Windows pin numbers in docs/pin40.md (0 upwards, in the order the
  firmware lists the header's GPIO pins), not header pin numbers and not
  SoC pin names.

  Run it in Windows PowerShell 5.1 (powershell.exe): PowerShell 7 cannot
  load WinRT types.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -List
  powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 0 -Count 10
  powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 3 -Read

  Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
  SPDX-License-Identifier: GPL-2.0-only
#>
param(
    [int]$Pin = 0,
    [int]$Count = 10,
    [int]$PeriodMs = 500,
    [switch]$Read,
    [switch]$List
)

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -eq 'Core') {
    throw 'Run this in Windows PowerShell 5.1 (powershell.exe); PowerShell 7 cannot load WinRT types.'
}

$null = [Windows.Devices.Gpio.GpioController, Windows.Devices, ContentType = WindowsRuntime]
$ctl = [Windows.Devices.Gpio.GpioController]::GetDefault()
if ($null -eq $ctl) {
    throw ('No GPIO controller for applications. Is "40-pin GPIO/I2C/SPI for apps" on in the firmware, ' +
           'and does Device Manager show "Resource Hub proxy device" started? See docs/pin40.md.')
}

if ($List) {
    "GPIO pins for applications: $($ctl.PinCount)"
    for ($i = 0; $i -lt $ctl.PinCount; $i++) {
        $p = $null
        $status = [Windows.Devices.Gpio.GpioOpenStatus]::PinOpened
        if ($ctl.TryOpenPin($i, [Windows.Devices.Gpio.GpioSharingMode]::SharedReadOnly, [ref]$p, [ref]$status)) {
            '{0,3}  {1,-5}  {2}' -f $i, $p.Read(), $p.GetDriveMode()
            $p.Dispose()
        } else {
            '{0,3}  not available ({1})' -f $i, $status
        }
    }
    return
}

$gpio = $ctl.OpenPin($Pin)
try {
    if ($Read) {
        $gpio.SetDriveMode([Windows.Devices.Gpio.GpioPinDriveMode]::Input)
        "pin $Pin reads $($gpio.Read())"
        return
    }
    $gpio.Write([Windows.Devices.Gpio.GpioPinValue]::Low)
    $gpio.SetDriveMode([Windows.Devices.Gpio.GpioPinDriveMode]::Output)
    for ($i = 0; $i -lt $Count; $i++) {
        $gpio.Write([Windows.Devices.Gpio.GpioPinValue]::High)
        Start-Sleep -Milliseconds ($PeriodMs / 2)
        $gpio.Write([Windows.Devices.Gpio.GpioPinValue]::Low)
        Start-Sleep -Milliseconds ($PeriodMs / 2)
    }
    "pin $Pin toggled $Count times"
} finally {
    # back to an input, so the pin does not keep driving whatever is wired to it
    $gpio.SetDriveMode([Windows.Devices.Gpio.GpioPinDriveMode]::Input)
    $gpio.Dispose()
}
