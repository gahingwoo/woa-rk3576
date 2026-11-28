@echo off
rem win10-nvme.cmd W:\ - after the drivers went into a Windows 10 image that
rem is not running (DISM /Add-Driver from WinPE: install.cmd W:\, Maskrom
rem Studio), make its NVMe controller take rk3xnvme instead of stornvme.
rem
rem stornvme.inf matches PCI\CC_010802 like rk3xnvme.inf does, and an inbox
rem driver outranks a test-signed one whatever the ID, so Plug and Play would
rem pick stornvme, which faults on the RK3576 under Windows 10 (bugcheck 0x7E).
rem This deletes the value stornvme.inf under
rem HKLM\DRIVERS\DriverDatabase\DeviceIds\PCI\CC_010802 of the image's DRIVERS
rem hive. The stornvme service is left as it is.
rem
rem Does nothing on Windows 11 (build 22000 and later), where stornvme works
rem and rk3xnvme.inf matches nothing. Exit 0: done or nothing to do; 1: the
rem image would boot its NVMe drive with stornvme; 2: usage.
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
setlocal enableextensions
set T=%~1
if "%T%"=="" (
  echo Usage: win10-nvme.cmd W:\
  exit /b 2
)
if "%T:~-1%"=="\" set T=%T:~0,-1%
if not exist "%T%\Windows\System32\config\DRIVERS" (
  echo No Windows at %T%\ ^(no %T%\Windows\System32\config\DRIVERS^).
  exit /b 2
)
set BUILD=
reg load HKLM\RKNVMESW "%T%\Windows\System32\config\SOFTWARE" >nul 2>&1
if not errorlevel 1 (
  for /f "tokens=3" %%v in ('reg query "HKLM\RKNVMESW\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber 2^>nul ^| find "CurrentBuildNumber"') do set BUILD=%%v
  reg unload HKLM\RKNVMESW >nul 2>&1
)
if "%BUILD%"=="" (
  echo Could not read the Windows build of %T%\.
  exit /b 1
)
if %BUILD% GEQ 22000 (
  echo Windows build %BUILD%: stornvme stays the NVMe driver.
  exit /b 0
)
reg load HKLM\RKNVMEDB "%T%\Windows\System32\config\DRIVERS" >nul 2>&1 || (
  echo Could not load the DRIVERS hive of %T%\.
  exit /b 1
)
set E=0
set K=HKLM\RKNVMEDB\DriverDatabase\DeviceIds\PCI\CC_010802
reg query "%K%" 2>nul | findstr /i "oem" >nul || (
  echo No added driver is registered for PCI\CC_010802: rk3xnvme is not in the image.
  set E=1
)
reg query "%K%" /v stornvme.inf >nul 2>&1
if errorlevel 1 (
  echo stornvme.inf is not in the DriverDatabase entry for PCI\CC_010802.
) else (
  reg delete "%K%" /v stornvme.inf /f >nul 2>&1
  reg query "%K%" /v stornvme.inf >nul 2>&1
  if errorlevel 1 (
    echo Removed stornvme.inf from the DriverDatabase entry for PCI\CC_010802: the NVMe drive takes rk3xnvme.
  ) else (
    echo Could not remove stornvme.inf from the DriverDatabase entry for PCI\CC_010802.
    set E=1
  )
)
reg unload HKLM\RKNVMEDB >nul 2>&1
exit /b %E%
