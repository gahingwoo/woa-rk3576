@echo off
rem install.cmd - install the RK3576 Windows driver package for one board.
rem
rem   install.cmd W:\ <board> [/optional]       into a Windows that is not
rem                                             running (applied from WinPE,
rem                                             or a mounted install.wim): DISM
rem   install.cmd /online <board> [/optional]   into this running Windows:
rem                                             install-online.ps1 trusts
rem                                             certs\*.cer, stages the drivers
rem                                             (pnputil /add-driver, never
rem                                             /install) and marks the devices
rem                                             to take them at the next boot;
rem                                             a task checks after that boot
rem                                             that they did (verify-bound.ps1)
rem
rem Which drivers go in depends on the target's Windows build:
rem boards\<board>-builds.txt (and -builds-optional.txt) give each driver the
rem builds it is for (drivers.tsv's os column), and the ones that cover the
rem build are installed: a Windows 10 build of a driver (dwc_eqos10) on
rem Windows 10, its Windows 11 build (dwc_eqos) from 22000. boards\skipped.txt
rem names what has no build for the target yet, and this says so.
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
set OPTARG=%~3
set VER=unknown
if exist "%HERE%VERSION.txt" set /p VER=<"%HERE%VERSION.txt"
echo RK3576 driver package %VER%, board %B%.

if /i "%T%"=="/online" goto :online

if "%T:~-1%"=="\" set T=%T:~0,-1%
if not exist "%T%\Windows\System32\config\SYSTEM" (
  echo No Windows at %T%\ ^(no %T%\Windows\System32\config\SYSTEM^).
  exit /b 2
)
rem Which Windows: the build picks the drivers (:lists).
set BUILD=
reg load HKLM\RKDRVSW "%T%\Windows\System32\config\SOFTWARE" >nul 2>&1
if not errorlevel 1 (
  for /f "tokens=3" %%v in ('reg query "HKLM\RKDRVSW\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber 2^>nul ^| find "CurrentBuildNumber"') do set BUILD=%%v
  reg unload HKLM\RKDRVSW >nul 2>&1
)
if "%BUILD%"=="" (
  echo Could not read the Windows build of %T%\ ^(its SOFTWARE hive^); taking the Windows 11 ^(22000^) drivers.
  set BUILD=22000
)
call :lists %BUILD%
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
if exist "%SEL%" del "%SEL%" >nul 2>&1
if not "%E%"=="0" exit /b 1
echo RK3576 drivers %VER% added to %T%\.
exit /b 0

:lists
rem %1 = the build. Sets LISTS to the drivers for that build (a list file
rem written from boards\<board>-builds*.txt) and names what has no build for it.
set SEL=
if not exist "%HERE%boards\%B%-builds.txt" goto :lists_old
set SEL=%TEMP%\rk3576-drivers-%B%-%RANDOM%.txt
type nul > "%SEL%"
set BL="%HERE%boards\%B%-builds.txt"
if /i "%OPTARG%"=="/optional" set BL=%BL% "%HERE%boards\%B%-builds-optional.txt"
for %%L in (%BL%) do (
  if exist %%L for /f "usebackq eol=# tokens=1-3 delims=	" %%a in (%%L) do (
    if %1 GEQ %%b if %1 LEQ %%c >>"%SEL%" echo %%a
  )
)
set LISTS="%SEL%"
if %1 LSS 22000 (echo Windows build %1: the Windows 10 drivers.) else (echo Windows build %1: the Windows 11 drivers.)
if exist "%HERE%boards\skipped.txt" (
  for /f "usebackq eol=# tokens=1-4,* delims=	" %%a in ("%HERE%boards\skipped.txt") do (
    if %1 GEQ %%b if %1 LEQ %%c (
      echo ,%%d, | find ",%B%," >nul && echo Skipped %%a ^(%%e^): it has no driver for build %1 yet.
    )
  )
)
exit /b 0

:lists_old
rem A package from before boards\<board>-builds.txt: the two lists.
set OSK=
if %1 LSS 22000 set OSK=-win10
if not exist "%HERE%boards\%B%%OSK%.txt" set OSK=
set LISTS="%HERE%boards\%B%%OSK%.txt"
if /i "%OPTARG%"=="/optional" set LISTS=%LISTS% "%HERE%boards\%B%%OSK%-optional.txt"
echo Windows build %1.
exit /b 0

:online
net session >nul 2>&1 || (
  echo Run this from an administrator command prompt.
  exit /b 2
)
set OPT=
if /i "%~3"=="/optional" set OPT=-Optional
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install-online.ps1" -Board %B% %OPT%
exit /b %errorlevel%

:usage
echo Usage: install.cmd W:\ ^<board^> [/optional]
echo        install.cmd /online ^<board^> [/optional]
echo ^<board^>: tspi3m, cm5io or rock4d
exit /b 2
