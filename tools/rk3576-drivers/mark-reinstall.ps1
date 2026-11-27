# mark-reinstall.ps1 - run as SYSTEM by install-online.ps1 (a one-shot
# scheduled task): sets bit 0x20 (CONFIGFLAG_REINSTALL) in ConfigFlags of
# each device instance listed in reinstall.txt next to this script, one
# instance ID per line (for example ACPI\RKCP3004\0). At the next boot PnP
# installs those devices from the driver store. Nothing else is changed.
# Log: mark-reinstall.log next to this script.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $here 'mark-reinstall.log'
$out = @("mark-reinstall $(Get-Date -Format s) as $([Environment]::UserName)")
try {
    $ids = Get-Content -LiteralPath (Join-Path $here 'reinstall.txt') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
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
