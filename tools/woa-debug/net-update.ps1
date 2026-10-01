# net-update.ps1 - make the running Windows use the dwc_eqos package on this
# stick, and nothing older. pnputil /add-driver alone is not enough: Windows
# kept the package DISM installed at deploy time ("up-to-date on device").
# Run from an ADMIN prompt via net-update.cmd, then REBOOT.

$ErrorActionPreference = 'Continue'
$stick = Split-Path -Qualifier $PSScriptRoot
$inf = Join-Path $stick 'woa-drivers\dwc_eqos\dwc_eqos.inf'
if (-not (Test-Path $inf)) { Write-Host "$inf not found"; exit 1 }
$want = ((Get-Content $inf | Select-String -Pattern '^\s*DriverVer\s*=') -split ',')[-1].Trim()
Write-Host "stick package version: $want"

Write-Host '== dwc_eqos packages in the driver store, before:'
$pkgs = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\dwc_eqos.inf' }
$pkgs | Format-Table Driver,Version,Date -AutoSize

foreach ($p in $pkgs) {
    if ($p.Version -ne $want) {
        Write-Host "== removing $($p.Driver) ($($p.Version))"
        pnputil /delete-driver $p.Driver /uninstall /force
    }
}
if (-not ($pkgs | Where-Object { $_.Version -eq $want })) {
    Write-Host "== adding $inf"
    pnputil /add-driver $inf /install
}

Write-Host '== dwc_eqos packages in the driver store, after:'
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\dwc_eqos.inf' } | Format-Table Driver,Version,Date -AutoSize
Write-Host ''
Write-Host 'Now REBOOT (Start > Restart), then run net-debug.cmd again.'
Write-Host "After the reboot, adapter.txt should show DriverVersion $want."
