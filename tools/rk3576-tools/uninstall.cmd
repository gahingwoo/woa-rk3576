@echo off
rem uninstall.cmd - remove the RK3576 tools (the one in %ProgramFiles%\RK3576\setup does it)
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
if not exist "%ProgramFiles%\RK3576\setup\uninstall.cmd" (
  echo The RK3576 tools are not installed.
  exit /b 0
)
call "%ProgramFiles%\RK3576\setup\uninstall.cmd" %*
