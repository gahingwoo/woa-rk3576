# mark-reinstall.ps1 - run as SYSTEM by install-online.ps1 (a one-shot
# scheduled task): sets bit 0x20 (CONFIGFLAG_REINSTALL) in ConfigFlags of
# each device instance listed in reinstall.txt next to this script, one
# instance ID per line (for example ACPI\RKCP3004\0). At the next boot PnP
# installs those devices from the driver store. With unmap.txt next to this
# script (lines "<device id key> <inf>", for example
# "PCI\CC_010802 stornvme.inf"), it first deletes value <inf> under
# HKLM\DRIVERS\DriverDatabase\DeviceIds\<device id key>, so Plug and Play
# no longer offers that INF for the ID (rk3xnvme on Windows 10, see
# rk3xnvme README). Nothing else is changed.
# Log: mark-reinstall.log next to this script.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $here 'mark-reinstall.log'
$out = @("mark-reinstall $(Get-Date -Format s) as $([Environment]::UserName)")
try {
    $um = Join-Path $here 'unmap.txt'
    if (Test-Path -LiteralPath $um) {
        foreach ($l in Get-Content -LiteralPath $um) {
            $f = @($l.Trim() -split '\s+')
            if ($f.Count -ne 2) { continue }
            $k = "HKLM:\DRIVERS\DriverDatabase\DeviceIds\$($f[0])"
            if ((Test-Path -LiteralPath $k) -and (Get-ItemProperty -LiteralPath $k -Name $f[1] -ErrorAction SilentlyContinue)) {
                Remove-ItemProperty -LiteralPath $k -Name $f[1]
                $out += "$($f[0])  $($f[1]) removed from the DriverDatabase"
            } else {
                $out += "$($f[0])  no $($f[1]) in the DriverDatabase"
            }
        }
    }
    $ids = @()
    $rl = Join-Path $here 'reinstall.txt'
    if (Test-Path -LiteralPath $rl) { $ids = @(Get-Content -LiteralPath $rl | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
    foreach ($id in $ids) {
        $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
        if (-not (Test-Path -LiteralPath $k)) { $out += "$id  not present"; continue }
        $p = Get-ItemProperty -LiteralPath $k
        $old = 0
        if ($p.PSObject.Properties['ConfigFlags']) { $old = [int]$p.ConfigFlags }
        Set-ItemProperty -LiteralPath $k -Name ConfigFlags -Value ($old -bor 0x20) -Type DWord
        $new = (Get-ItemProperty -LiteralPath $k -Name ConfigFlags).ConfigFlags
        $out += ("{0}  ConfigFlags 0x{1:x8} -> 0x{2:x8}" -f $id, $old, $new)
    }
    $out += 'done'
} catch {
    $out += "FAILED: $_"
}
$out | Out-File -FilePath $log -Encoding ascii
