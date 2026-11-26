@echo off
rem install.cmd - install the RK3576 Windows driver package for one board.
rem
rem   install.cmd W:\ <board> [/optional]       into a Windows that is not
rem                                             running (applied from WinPE,
rem                                             or a mounted install.wim): DISM
rem   install.cmd /online <board> [/optional]   into this running Windows:
rem                                             trusts certs\*.cer, then pnputil
rem
rem <board> is tspi3m, cm5io or rock4d. boards\<board>.txt lists the driver
rem folders installed by default, boards\<board>-optional.txt the ones added
rem with /optional (untested on that board). manifest.json says what each is.
rem
rem The packages are test-signed: the Windows needs test signing on
rem (bcdedit /set testsigning on). Offline, the certificates are not trusted
rem by this script (Maskrom Studio writes them into the image); Windows then
rem still loads the drivers, Plug and Play may ask about the publisher once.
rem
rem Copyright (c) 2026 Ga Hing Woo ^<gahing@gahingwoo.com^>
rem SPDX-License-Identifier: GPL-2.0-only
setlocal enableextensions enabledelayedexpansion
set HERE=%~dp0
set T=%~1
set B=%~2
if "%T%"=="" goto :usage
if "%B%"=="" goto :usage
if not exist "%HERE%boards\%B%.txt" (
  echo There is no board %B% in this package ^(boards\%B%.txt^).
  exit /b 2
)
set LISTS="%HERE%boards\%B%.txt"
if /i "%~3"=="/optional" set LISTS=%LISTS% "%HERE%boards\%B%-optional.txt"
set VER=unknown
if exist "%HERE%VERSION.txt" set /p VER=<"%HERE%VERSION.txt"
echo RK3576 driver package %VER%, board %B%.

if /i "%T%"=="/online" goto :online

if "%T:~-1%"=="\" set T=%T:~0,-1%
if not exist "%T%\Windows\System32\config\SYSTEM" (
  echo No Windows at %T%\ ^(no %T%\Windows\System32\config\SYSTEM^).
  exit /b 2
)
set E=0
for %%L in (%LISTS%) do (
  for /f "usebackq eol=# delims=" %%d in (%%L) do (
    echo Adding %%d.
    dism /English /Image:%T%\ /Add-Driver /Driver:"%HERE%drivers\%%d" /Recurse /ForceUnsigned >nul || (
      echo DISM could not add %%d ^(exit !errorlevel!^).
      set E=1
    )
  )
)
if not "%E%"=="0" exit /b 1
echo RK3576 drivers %VER% added to %T%\.
exit /b 0

:online
net session >nul 2>&1 || (
  echo Run this from an administrator command prompt.
  exit /b 2
)
for %%c in ("%HERE%certs\*.cer") do (
  certutil -f -addstore Root "%%~c" >nul || echo Could not trust %%~nxc.
  certutil -f -addstore TrustedPublisher "%%~c" >nul || echo Could not trust %%~nxc.
)
set E=0
for %%L in (%LISTS%) do (
  for /f "usebackq eol=# delims=" %%d in (%%L) do (
    echo Installing %%d.
    pnputil /add-driver "%HERE%drivers\%%d\*.inf" /install >nul
    rem 259: nothing to bind (no such device, or its firmware option is off); 3010: restart
    if errorlevel 1 if not errorlevel 259 (
      echo pnputil could not install %%d ^(exit !errorlevel!^).
      set E=1
    )
  )
)
if not "%E%"=="0" exit /b 1
echo RK3576 drivers %VER% installed. Restart Windows.
exit /b 0

:usage
echo Usage: install.cmd W:\ ^<board^> [/optional]
echo        install.cmd /online ^<board^> [/optional]
echo ^<board^>: tspi3m, cm5io or rock4d
exit /b 2
