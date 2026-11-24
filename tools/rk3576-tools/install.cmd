@echo off
rem install.cmd - put the RK3576 tools into %ProgramFiles%\RK3576
rem
rem   install.cmd            install (asks for administrator rights), start
rem                          rk-monitor for you, show the result
rem   install.cmd /quiet     no pause at the end
rem   install.cmd /nostart   do not start rk-monitor now (it starts at logon)
rem
rem It copies the RK3576 folder next to this file, starts rk-monitor at every
rem logon for all users with the desktop widget on, adds Start menu
rem shortcuts and the Settings, Apps entry, and registers the GPU and NPU
rem performance counters, with a task that registers them again after a
rem driver update. Running it again upgrades in place. README.md has the
rem details; uninstall.cmd removes all of it.
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
setlocal enableextensions
set QUIET=
set NOSTART=
for %%a in (%*) do (
  if /i "%%~a"=="/quiet" set QUIET=1
  if /i "%%~a"=="/nostart" set NOSTART=-NoStart
)
fltmc >nul 2>&1
if errorlevel 1 (
  echo Asking for administrator rights...
  if "%~1"=="" (powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs") else (powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs")
  exit /b
)
if not exist "%~dp0RK3576\rk-monitor.exe" (
  echo No RK3576\rk-monitor.exe next to %~nx0: unpack the whole bundle first.
  if not defined QUIET pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RK3576\setup\rk3576-setup.ps1" -Action Install -Source "%~dp0RK3576" %NOSTART%
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (echo RK3576 tools installed in "%ProgramFiles%\RK3576".) else (echo The install failed, exit %RC%. Log: "%ProgramData%\RK3576\setup.log")
if not defined QUIET pause
exit /b %RC%
