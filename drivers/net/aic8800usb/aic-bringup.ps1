# aic-bringup.ps1 - settings and readout for the aic8800usb board session.
#
#   aic-bringup.ps1 -Status
#       every AIC8800 USB device (loader PID_8D80, runtime PID_8D81 and its
#       Bluetooth interfaces), its PnP state, and what the driver recorded
#       under its Device Parameters (the stored PMK is not printed)
#   aic-bringup.ps1 -Stage 3 -Ssid home -Passphrase 'secret'
#       write the settings (service Parameters key, read by both device
#       instances). The driver replaces Passphrase by Pmk the first time it
#       uses it. Restart Windows afterwards: the stages run at device start.
#
# Run as administrator.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [int] $Stage = -1,
    [string] $Ssid = '',
    [string] $Passphrase = '',
    [switch] $Status
)

$ErrorActionPreference = 'Stop'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\aic8800usb\Parameters'

if ($Stage -ge 0 -or $Ssid -ne '' -or $Passphrase -ne '') {
    New-Item -Path $params -Force | Out-Null
    if ($Stage -ge 0) { Set-ItemProperty -Path $params -Name Stage -Type DWord -Value $Stage }
    if ($Ssid -ne '') { Set-ItemProperty -Path $params -Name Ssid -Type String -Value $Ssid }
    if ($Passphrase -ne '') {
        if ($Passphrase.Length -lt 8 -or $Passphrase.Length -gt 64) { throw 'a WPA2 passphrase has 8 to 63 characters (or 64 hex digits)' }
        Set-ItemProperty -Path $params -Name Passphrase -Type String -Value $Passphrase
        Remove-ItemProperty -Path $params -Name Pmk -ErrorAction SilentlyContinue
    }
    Write-Host 'Settings written. Restart Windows for them to take effect.'
}

if ($Status -or ($Stage -lt 0 -and $Ssid -eq '' -and $Passphrase -eq '')) {
    if (Test-Path $params) {
        $p = Get-ItemProperty -Path $params
        Write-Host ("Settings: Stage={0} Ssid='{1}' Passphrase={2} Pmk={3}" -f $p.Stage, $p.Ssid,
            $(if ($p.Passphrase) { 'set' } else { 'none' }), $(if ($p.Pmk) { 'set' } else { 'none' }))
    } else {
        Write-Host 'Settings: driver not installed (no service key)'
    }
    $devs = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_A69C*' }
    if (-not $devs) { Write-Host 'No AIC8800 USB device present (VID_A69C).' }
    foreach ($d in $devs) {
        Write-Host ''
        Write-Host ("{0}  [{1}] {2}" -f $d.InstanceId, $d.Status, $d.FriendlyName)
        $prob = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
        if ($prob) { Write-Host "   problem code $prob" }
        $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
        if (Test-Path $key) {
            $v = Get-ItemProperty -Path $key
            foreach ($n in $v.PSObject.Properties.Name) {
                if ($n -like 'PS*' -or $n -eq 'Pmk') { continue }
                $val = $v.$n
                if ($val -is [int] -or $val -is [long]) { $val = '{0} (0x{0:X})' -f $val }
                if ($n -eq 'ScanResults') { Write-Host "   ScanResults:"; ($v.$n -split "`n") | ForEach-Object { if ($_) { Write-Host "      $_" } }; continue }
                Write-Host ("   {0,-20} {1}" -f $n, $val)
            }
        }
    }
    Write-Host ''
    Get-PnpDevice -Class Bluetooth -PresentOnly -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("Bluetooth: {0}  [{1}] {2}" -f $_.InstanceId, $_.Status, $_.FriendlyName) }
}
