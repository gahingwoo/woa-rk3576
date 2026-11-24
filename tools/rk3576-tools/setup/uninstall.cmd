@echo off
rem uninstall.cmd - remove the RK3576 tools: files, shortcuts, autostart,
rem the counter task and the counter registrations, every user's rk-monitor
rem settings (rk3576-setup.ps1 -Action Uninstall lists them).
rem
rem   uninstall.cmd [/quiet]
rem
rem It runs from the folder it deletes, so the work is one block that cmd
rem has read in full before it starts, and the script it calls is a copy.
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
setlocal enableextensions
set QUIET=
if /i "%~1"=="/quiet" set QUIET=1
fltmc >nul 2>&1
if errorlevel 1 (
  echo Asking for administrator rights...
  if "%~1"=="" (powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs") else (powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs")
  exit /b
)
(
  copy /y "%~dp0rk3576-setup.ps1" "%TEMP%\rk3576-setup-uninstall.ps1" >nul
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\rk3576-setup-uninstall.ps1" -Action Uninstall
  del /f /q "%TEMP%\rk3576-setup-uninstall.ps1" >nul 2>&1
  echo.
  echo RK3576 tools removed.
  if not defined QUIET pause
  exit /b 0
)
