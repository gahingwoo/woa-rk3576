@echo off
rem net-update: install the dwc_eqos package from this stick into the running
rem Windows, then run net-debug. Run from an ADMINISTRATOR prompt.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
set PKG=%~d0\woa-drivers\dwc_eqos\dwc_eqos.inf
if not exist "%PKG%" (echo %PKG% not found & exit /b 1)
echo == installed dwc_eqos before:
pnputil /enum-drivers | find /i "dwc_eqos" 
echo == installing %PKG%
echo    (if Windows asks about the publisher, choose "Install this driver software anyway")
pnputil /add-driver "%PKG%" /install
echo == restarting the adapter with the new driver
powershell -NoProfile -Command "Get-PnpDevice -PresentOnly | ? InstanceId -like 'ACPI\RKCP6543*' | ForEach-Object { pnputil /restart-device $_.InstanceId }"
timeout /t 10 /nobreak >nul
call "%~dp0net-debug.cmd"
