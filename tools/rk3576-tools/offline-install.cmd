@echo off
rem offline-install.cmd - install the RK3576 tools into a Windows that is not
rem running: an image applied to a disk (from WinPE, as Maskrom Studio and
rem woa-deploy do), or a mounted install.wim. cmd and reg.exe only: WinPE
rem has no PowerShell.
rem
rem   offline-install.cmd W:\           W:\ holds the new Windows (W:\Windows)
rem   offline-install.cmd W:\ /hook     also make that Windows' SetupComplete.cmd
rem                                     call the first-boot step (appended, or
rem                                     a new file)
rem
rem What it does (README.md, "Offline install", is the interface):
rem   1. copies the RK3576 folder next to this file to W:\Program Files\RK3576
rem   2. loads W:\Windows\System32\config\SOFTWARE and writes
rem        Microsoft\Windows\CurrentVersion\Run  "RK3576 Monitor"
rem          REG_EXPAND_SZ "%%ProgramFiles%%\RK3576\rk-monitor.exe" --autostart
rem        RK3576\Monitor  Widget = 1
rem        RK3576  InstallDir, Version, FirstBootPending = 1
rem        Microsoft\Windows\CurrentVersion\Uninstall\RK3576Tools
rem   3. with /hook: SetupComplete.cmd gets
rem        call "%%ProgramFiles%%\RK3576\setup\firstboot.cmd"
rem Without /hook, the image's own first-boot script must run that line
rem as SYSTEM; that step adds the Start menu, the two tasks and the
rem counter registrations, which need the running system. Its logon task
rem then replaces the Run entry (which only waits out Explorer's startup
rem delay; the Run entry is the fallback should the first-boot step never run).
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
setlocal enableextensions
set T=%~1
if "%T%"=="" goto :usage
if "%T:~-1%"=="\" set T=%T:~0,-1%
if not exist "%T%\Windows\System32\config\SOFTWARE" (
  echo No Windows at %T%\ ^(no %T%\Windows\System32\config\SOFTWARE^).
  exit /b 2
)
if not exist "%~dp0RK3576\rk-monitor.exe" (
  echo No RK3576\rk-monitor.exe next to %~nx0.
  exit /b 2
)
set VER=unknown
if exist "%~dp0RK3576\VERSION.txt" set /p VER=<"%~dp0RK3576\VERSION.txt"

echo Copying the RK3576 tools to %T%\Program Files\RK3576.
if exist "%T%\Program Files\RK3576" rmdir /s /q "%T%\Program Files\RK3576"
xcopy "%~dp0RK3576" "%T%\Program Files\RK3576\" /e /i /q /y >nul || goto :fail

echo Writing the registry of %T%\Windows.
reg load HKLM\RK3576OFF "%T%\Windows\System32\config\SOFTWARE" >nul || goto :fail
set K=HKLM\RK3576OFF
set U=%K%\Microsoft\Windows\CurrentVersion\Uninstall\RK3576Tools
set E=0
reg add "%K%\Microsoft\Windows\CurrentVersion\Run" /v "RK3576 Monitor" /t REG_EXPAND_SZ /d "\"%%ProgramFiles%%\RK3576\rk-monitor.exe\" --autostart" /f >nul || set E=1
reg add "%K%\RK3576\Monitor" /v Widget /t REG_DWORD /d 1 /f >nul || set E=1
reg add "%K%\RK3576" /v InstallDir /t REG_EXPAND_SZ /d "%%ProgramFiles%%\RK3576" /f >nul || set E=1
reg add "%K%\RK3576" /v Version /t REG_SZ /d "%VER%" /f >nul || set E=1
reg add "%K%\RK3576" /v FirstBootPending /t REG_DWORD /d 1 /f >nul || set E=1
reg add "%U%" /v DisplayName /t REG_SZ /d "RK3576 tools" /f >nul || set E=1
reg add "%U%" /v DisplayVersion /t REG_SZ /d "%VER%" /f >nul || set E=1
reg add "%U%" /v Publisher /t REG_SZ /d "woa-rk3576" /f >nul || set E=1
reg add "%U%" /v InstallLocation /t REG_EXPAND_SZ /d "%%ProgramFiles%%\RK3576" /f >nul || set E=1
reg add "%U%" /v DisplayIcon /t REG_EXPAND_SZ /d "%%ProgramFiles%%\RK3576\rk-monitor.exe" /f >nul || set E=1
reg add "%U%" /v UninstallString /t REG_EXPAND_SZ /d "\"%%ProgramFiles%%\RK3576\setup\uninstall.cmd\"" /f >nul || set E=1
reg add "%U%" /v URLInfoAbout /t REG_SZ /d "https://github.com/gahingwoo/woa-rk3576" /f >nul || set E=1
reg add "%U%" /v NoModify /t REG_DWORD /d 1 /f >nul || set E=1
reg add "%U%" /v NoRepair /t REG_DWORD /d 1 /f >nul || set E=1
reg unload HKLM\RK3576OFF >nul || set E=1
if not "%E%"=="0" goto :fail

if /i "%~2"=="/hook" (
  if not exist "%T%\Windows\Setup\Scripts" mkdir "%T%\Windows\Setup\Scripts"
  if not exist "%T%\Windows\Setup\Scripts\SetupComplete.cmd" echo @echo off> "%T%\Windows\Setup\Scripts\SetupComplete.cmd"
  echo call "%%ProgramFiles%%\RK3576\setup\firstboot.cmd">> "%T%\Windows\Setup\Scripts\SetupComplete.cmd"
  echo SetupComplete.cmd calls the first-boot step.
) else (
  echo First boot: run as SYSTEM  call "%%ProgramFiles%%\RK3576\setup\firstboot.cmd"
)
echo RK3576 tools %VER% put into %T%\.
exit /b 0

:usage
echo usage: %~nx0 ^<windows root, e.g. W:\^> [/hook]
exit /b 1

:fail
reg unload HKLM\RK3576OFF >nul 2>&1
echo offline install failed.
exit /b 3
