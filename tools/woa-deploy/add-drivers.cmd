@echo off
rem ---------------------------------------------------------------------------
rem add-drivers.cmd - inject this stick's drivers into a Windows that
rem deploy-windows.cmd has already put on the eMMC, without erasing anything.
rem
rem Written for INACCESSIBLE_BOOT_DEVICE on the first boot: dwcsdhc used to be
rem demand-start, so DISM only staged it, and the installed system had no
rem driver for its own boot disk. The driver on this stick is boot-start, which
rem DISM installs properly (binary, service, device binding).
rem
rem It then reads the installed system's registry and prints the start type of
rem the two drivers the boot path needs: dwcsdhc (ours, the eMMC controller)
rem and sdstor (inbox, the disk on it). Both must be 0, boot start.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set DRIVERS=%~d0\woa-drivers
set LOG=%~d0\woa-deploy\add-drivers.log
echo woa-rk3576 add-drivers, %DATE% %TIME% > "%LOG%"

set WIN=
set N=0
for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
  if exist %%d:\Windows\System32\config\SYSTEM (
    set WIN=%%d:
    set /a N+=1
  )
)
if not "%N%"=="1" (
  echo Expected exactly one installed Windows, found %N%. Stopping.
  goto :fail
)
echo Installed Windows: %WIN%
echo win=%WIN% >> "%LOG%"

echo Adding the drivers.
dism /English /Image:%WIN%\ /Add-Driver /Driver:"%DRIVERS%" /Recurse /ForceUnsigned >> "%LOG%" 2>&1 || goto :faillog

reg load HKLM\WOASYS %WIN%\Windows\System32\config\SYSTEM >> "%LOG%" 2>&1 || goto :faillog
set CS=1
for /f "tokens=3" %%v in ('reg query HKLM\WOASYS\Select /v Current ^| find "Current"') do set /a CS=%%v
set SVC=HKLM\WOASYS\ControlSet00%CS%\Services

rem If DISM left dwcsdhc demand-start (for instance because it judged the
rem package already present), the service key exists and can be corrected.
set START=
for /f "tokens=3" %%v in ('reg query %SVC%\dwcsdhc /v Start 2^>nul ^| find "Start"') do set START=%%v
if defined START if not "%START%"=="0x0" (
  echo dwcsdhc Start was %START%; setting it to 0.
  reg add %SVC%\dwcsdhc /v Start /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
)

echo.
echo Start type in the installed system ^(0x0 = boot start^):
for %%s in (dwcsdhc sdstor sdport) do (
  set V=missing
  for /f "tokens=3" %%v in ('reg query %SVC%\%%s /v Start 2^>nul ^| find "Start"') do set V=%%v
  echo   %%s  !V!
  echo %%s !V! >> "%LOG%"
)
reg unload HKLM\WOASYS >> "%LOG%" 2>&1

echo.
echo Done. dwcsdhc and sdstor should both read 0x0. Take the sticks out and
echo reboot into Windows. Log: %LOG%
goto :end

:faillog
reg unload HKLM\WOASYS >nul 2>&1
echo Something failed. Full log follows; it is also in %LOG%.
type "%LOG%"
:fail
echo.
echo Nothing more was changed.
:end
endlocal
