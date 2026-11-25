# aic-bringup.ps1 - settings and readout for the aic8800usb board session.
#
#   aic-bringup.ps1 -Status
#       every AIC8800 USB device (loader PID_8D80, runtime PID_8D81 and its
#       Bluetooth interfaces), its PnP state, and what the driver recorded
#       under its Device Parameters (the stored PMK is not printed)
#   aic-bringup.ps1 -Connect -Ssid home
#       join "home" with all stages on (Stage 4: the adapter carries
#       traffic). The passphrase is asked for without echo; it is never
#       printed, logged or kept in PowerShell's history. The driver replaces
#       it by the PMK the first time it uses it.
#   aic-bringup.ps1 -Stage 3 -Ssid home
#       a single stage, for the step-by-step board session
#   aic-bringup.ps1 -Mode WiFiCx
#       switch the WLAN interface to Windows' own Wi-Fi (aic8800wifi.inf):
#       the other package (aic8800usb.inf) is removed from the device and
#       the driver store, aic8800wifi.inf is staged, Stage is raised to 4;
#       restart Windows, then connect from the taskbar. -Mode Ethernet goes
#       back. Both INFs are taken from -PackageDir (default: the aic8800usb
#       folder next to this script). docs/dev/wificx.md.
#
# Settings go to HKLM\SYSTEM\CurrentControlSet\Services\aic8800usb\Parameters,
# read by both services (aic8800ldr and aic8800usb); -Connect also limits that
# key to SYSTEM and Administrators, since it holds the PMK. Restart Windows
# afterwards: the stages run at device start.
#
# Passing -Passphrase on the command line works too, but then it is in the
# console history; prefer the prompt.
#
# Run as administrator.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [ValidateSet('WiFiCx', 'Ethernet')]
    [string] $Mode = '',
    [string] $PackageDir = '',
    [int] $Stage = -1,
    [string] $Ssid = '',
    [string] $Passphrase = '',
    [switch] $Connect,
    [switch] $Status
)

$ErrorActionPreference = 'Stop'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\aic8800usb\Parameters'

function Protect-Key([string] $Path) {
    # Only SYSTEM and Administrators: the key holds the PMK.
    $acl = New-Object System.Security.AccessControl.RegistrySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in 'S-1-5-18', 'S-1-5-32-544') {     # SYSTEM, Administrators (any language)
        $who = New-Object System.Security.Principal.SecurityIdentifier($sid)
        $acl.AddAccessRule((New-Object System.Security.AccessControl.RegistryAccessRule($who, 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow')))
    }
    Set-Acl -Path $Path -AclObject $acl
}

if ($Mode -ne '') {
    if ($PackageDir -eq '') { $PackageDir = Join-Path $PSScriptRoot 'aic8800usb' }
    $want  = if ($Mode -eq 'WiFiCx') { 'aic8800wifi.inf' } else { 'aic8800usb.inf' }
    $other = if ($Mode -eq 'WiFiCx') { 'aic8800usb.inf' } else { 'aic8800wifi.inf' }
    $inf = Join-Path $PackageDir $want
    if (-not (Test-Path $inf)) { throw "$inf not found (-PackageDir)" }
    # Both bind USB\VID_A69C&PID_8D81&MI_02 at the same rank: only one may be
    # in the driver store. Taking the other off the device leaves it without
    # a driver until the wanted one is bound (now or at the restart).
    foreach ($d in Get-WindowsDriver -Online | Where-Object { (Split-Path $_.OriginalFileName -Leaf) -eq $other }) {
        Write-Host "removing $other ($($d.Driver))"
        pnputil /delete-driver $d.Driver /uninstall /force | Out-Host
    }
    pnputil /add-driver $inf | Out-Host
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) { throw "pnputil /add-driver $want failed: $LASTEXITCODE" }
    New-Item -Path $params -Force | Out-Null
    $cur = (Get-ItemProperty -Path $params -Name Stage -ErrorAction SilentlyContinue).Stage
    if ($Mode -eq 'WiFiCx' -and ($null -eq $cur -or $cur -lt 4)) {
        Set-ItemProperty -Path $params -Name Stage -Type DWord -Value 4
    }
    Write-Host "Mode $Mode staged. Restart Windows; then -Status shows ActiveMode."
    if ($Stage -lt 0 -and $Ssid -eq '' -and -not $Connect -and -not $Status) { exit 0 }
}

if ($Connect) {
    if ($Ssid -eq '') { throw '-Connect needs -Ssid' }
    if ($Stage -lt 0) { $Stage = 4 }
    if ($Passphrase -eq '') {
        $sec = Read-Host -AsSecureString "Passphrase for '$Ssid'"
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
        try { $Passphrase = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }
}

if ($Stage -ge 0 -or $Ssid -ne '' -or $Passphrase -ne '') {
    New-Item -Path $params -Force | Out-Null
    if ($Connect) { Protect-Key $params }
    if ($Stage -ge 0) { Set-ItemProperty -Path $params -Name Stage -Type DWord -Value $Stage }
    if ($Ssid -ne '') {
        Set-ItemProperty -Path $params -Name Ssid -Type String -Value $Ssid
        # a different network: the old PMK (which is per SSID) no longer fits
        Remove-ItemProperty -Path $params -Name Pmk -ErrorAction SilentlyContinue
    }
    if ($Passphrase -ne '') {
        if ($Passphrase.Length -lt 8 -or $Passphrase.Length -gt 64) {
            $Passphrase = $null
            throw 'a WPA2 passphrase has 8 to 63 characters (or 64 hex digits)'
        }
        Set-ItemProperty -Path $params -Name Passphrase -Type String -Value $Passphrase
        Remove-ItemProperty -Path $params -Name Pmk -ErrorAction SilentlyContinue
        $Passphrase = $null
    }
    Write-Host 'Settings written (passphrase not shown). Restart Windows for them to take effect.'
}

if ($Status -or ($Stage -lt 0 -and $Ssid -eq '' -and -not $Connect)) {
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
                if ($n -eq 'WdiTrace') { Write-Host ("   {0,-20} {1}" -f $n, $v.$n); continue }
                if ($n -eq 'ScanResults') { Write-Host "   ScanResults:"; ($v.$n -split "`n") | ForEach-Object { if ($_) { Write-Host "      $_" } }; continue }
                Write-Host ("   {0,-20} {1}" -f $n, $val)
            }
        }
    }
    Write-Host ''
    $nic = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like 'AIC8800*' }
    if ($nic) {
        # WiFiCx mode: what Windows' WLAN service sees (no networks listed
        # while location services are off: rtw89-win SCAN-7)
        netsh wlan show interfaces 2>$null | Out-Host
    }
    foreach ($n in $nic) {
        Write-Host ("Adapter: {0}  {1}  {2}  {3}" -f $n.Name, $n.Status, $n.MacAddress, $n.LinkSpeed)
        Get-NetIPAddress -InterfaceIndex $n.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("   IPv4 {0}/{1} ({2})" -f $_.IPAddress, $_.PrefixLength, $_.PrefixOrigin) }
        Get-NetRoute -InterfaceIndex $n.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("   gateway {0}" -f $_.NextHop) }
    }
    Get-PnpDevice -Class Bluetooth -PresentOnly -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("Bluetooth: {0}  [{1}] {2}" -f $_.InstanceId, $_.Status, $_.FriendlyName) }
}
