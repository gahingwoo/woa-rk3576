# null-devices-reinstall.ps1 - mark the RK3576 no-driver devices for
# reinstall at the next boot
#
# Sets bit 0x20 (CONFIGFLAG_REINSTALL) in ConfigFlags of every present or
# remembered instance of the hardware IDs rk3576-null.inf names, so that
# after a restart PnP installs them from the newest staged package. This
# replaces `pnputil /install` on a running system. The Enum key is writable
# by SYSTEM only: run it as SYSTEM (README.md shows a scheduled task).
# It changes nothing else, and prints each value before and after to
# %SystemRoot%\Temp\null-devices-reinstall.log.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

$ErrorActionPreference = 'Stop'
$log = Join-Path $env:SystemRoot 'Temp\null-devices-reinstall.log'
$ids = 'HYMB0001', 'RKCP0806', 'RKCP3005', 'HISI0031', 'AMZN0001'
$out = @("null-devices-reinstall $(Get-Date -Format s)")
$n = 0
foreach ($id in $ids) {
    $base = "HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\$id"
    if (-not (Test-Path $base)) { $out += "ACPI\$id  not present (nothing to do)"; continue }
    foreach ($inst in Get-ChildItem $base) {
        $p = Get-ItemProperty -Path $inst.PSPath
        $old = 0
        if ($p.PSObject.Properties['ConfigFlags']) { $old = [int]$p.ConfigFlags }
        Set-ItemProperty -Path $inst.PSPath -Name ConfigFlags -Value ($old -bor 0x20) -Type DWord
        $new = (Get-ItemProperty -Path $inst.PSPath -Name ConfigFlags).ConfigFlags
        $out += ("ACPI\{0}\{1}  ConfigFlags 0x{2:x8} -> 0x{3:x8}" -f $id, $inst.PSChildName, $old, $new)
        $n++
    }
}
$out += "$n instance(s) marked; restart Windows to install them."
$out | Tee-Object -FilePath $log
