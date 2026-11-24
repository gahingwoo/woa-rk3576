@echo off
rem firstboot.cmd - finish an offline install (offline-install.cmd) on the
rem first start. Run once as SYSTEM, from %WINDIR%\Setup\Scripts\SetupComplete.cmd:
rem
rem   call "%ProgramFiles%\RK3576\setup\firstboot.cmd"
rem
rem It adds the Start menu shortcuts and the "RK3576\Performance counters"
rem task and registers the GPU and NPU counters. It returns 0 even when that
rem fails, so it never holds up Windows setup; the log is
rem %ProgramData%\RK3576\setup.log, and install.cmd repairs it later.
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
if not exist "%ProgramData%\RK3576" mkdir "%ProgramData%\RK3576"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0rk3576-setup.ps1" -Action FirstBoot >> "%ProgramData%\RK3576\firstboot.log" 2>&1
exit /b 0
