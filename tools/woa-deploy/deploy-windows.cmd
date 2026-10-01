@echo off
rem ---------------------------------------------------------------------------
rem deploy-windows.cmd - put Windows onto the CM5-IO eMMC from this WinPE.
rem
rem Why not Windows Setup: Windows 11 23H2 Setup refuses a 29 GB disk and a
rem board with no TPM 2.0 or Secure Boot, and Setup would give the installed
rem system a BCD without testsigning -- which it needs from the very first
rem boot, because the eMMC driver it boots through (dwcsdhc) is test-signed.
rem Applying the image with DISM from here has none of those problems, and
rem this WinPE already runs the eMMC driver, so the disk is visible.
rem
rem What it does, in order, stopping at the first thing that is not as
rem expected:
rem   1. find the eMMC: exactly one disk whose PNP id starts SD\, 28-33 GB,
rem      whose first partition is the firmware region: starts at sector 64,
rem      64 MiB long (the layout CM5IO-emmc.img ships). That partition holds
rem      the idblock and the UEFI firmware and is never touched.
rem   2. find the Windows image: \install.wim or \sources\install.wim (or
rem      .esd) on any volume, and ask which edition.
rem   3. ask for a typed confirmation, then delete every partition after the
rem      first and create ESP + MSR + Windows in the space they leave.
rem   4. dism /apply-image (compact), dism /add-driver with the drivers on
rem      this stick, bcdboot, testsigning on in the new BCD, and BypassNRO
rem      so OOBE does not insist on a network.
rem
rem Usage: from the WinPE command prompt, run this file from the stick.
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set HERE=%~dp0
set DRIVERS=%~d0\woa-drivers
set LOG=%~d0\woa-deploy\deploy.log
echo woa-rk3576 deploy, %DATE% %TIME% > "%LOG%"

for %%L in (S W) do (
  if exist %%L:\ (
    echo Drive letter %%L: is already in use. Unassign it or reboot, then retry.
    goto :fail
  )
)
if not exist "%DRIVERS%\dwcsdhc\dwcsdhc.inf" (
  echo No driver packages at %DRIVERS%. This stick was not built with them.
  goto :fail
)

rem --- 1. the eMMC ----------------------------------------------------------
set DISK=
set NDISK=0
for /f "tokens=2 delims==" %%a in ('wmic diskdrive where "PNPDeviceID like 'SD\\%%'" get Index /value 2^>nul ^| find "="') do (
  for %%b in (%%a) do (
    set DISK=%%b
    set /a NDISK+=1
  )
)
if not "%NDISK%"=="1" (
  echo Expected exactly one SD/eMMC disk, found %NDISK%. Stopping.
  echo Is the SD card still in the slot? Take it out and retry.
  goto :fail
)

set SIZE=
for /f "tokens=2 delims==" %%a in ('wmic diskdrive where "Index=%DISK%" get Size /value ^| find "="') do for %%b in (%%a) do set SIZE=%%b
rem 11 digits, 28..33 GB. set /a is 32-bit, so compare the leading digits.
if "!SIZE:~10,1!"=="" goto :badsize
if not "!SIZE:~11,1!"=="" goto :badsize
set /a GB=1!SIZE:~0,2!-100
if %GB% LSS 28 goto :badsize
if %GB% GTR 33 goto :badsize

set P0OFF=
set P0SIZE=
for /f "tokens=1,2 delims==" %%a in ('wmic partition where "DiskIndex=%DISK% and Index=0" get StartingOffset^,Size /value ^| find "="') do (
  for %%c in (%%b) do (
    if /i "%%a"=="StartingOffset" set P0OFF=%%c
    if /i "%%a"=="Size" set P0SIZE=%%c
  )
)
if not "%P0OFF%"=="32768" goto :badfw
if not "%P0SIZE%"=="67076096" goto :badfw

echo.
echo eMMC: disk %DISK%, about %GB% GB. Firmware partition found at sector 64,
echo 64 MiB. It will be kept.
echo disk=%DISK% size=%SIZE% p0=%P0OFF%/%P0SIZE% >> "%LOG%"

rem --- 2. the Windows image --------------------------------------------------
set WIM=
for %%d in (C D E F G H I J K L M N O P Q R T U V Y Z) do (
  if not defined WIM (
    for %%f in ("%%d:\install.wim" "%%d:\sources\install.wim" "%%d:\install.esd" "%%d:\sources\install.esd") do (
      if not defined WIM if exist %%f set WIM=%%~f
    )
  )
)
if not defined WIM (
  echo No install.wim or install.esd found on any volume.
  echo Copy it from the Windows 11 23H2 ARM64 ISO ^(sources\install.wim^)
  echo to a partition on this stick that is exFAT or NTFS, then retry.
  goto :fail
)
echo.
echo Image: %WIM%
dism /English /Get-ImageInfo /ImageFile:"%WIM%"
echo.
set /p IDX=Which index to install (for example the one named Windows 11 Pro)?
if "%IDX%"=="" goto :fail
dism /English /Get-ImageInfo /ImageFile:"%WIM%" /Index:%IDX% > "%TEMP%\img.txt" 2>&1 || (
  echo Index %IDX% is not in this image.
  goto :fail
)
rem find is case-sensitive unless told otherwise, and on 2026-10-01 a check
rem for "ARM64" missed a genuine ARM64 image. Read the value, compare it /i.
set ARCH=
for /f "tokens=2 delims=:" %%a in ('type "%TEMP%\img.txt" ^| find /i "Architecture"') do (
  for %%b in (%%a) do set ARCH=%%b
)
if /i not "%ARCH%"=="arm64" (
  echo Index %IDX% reports architecture "%ARCH%", not arm64. What DISM said:
  type "%TEMP%\img.txt"
  goto :fail
)
rem "Version : 10.0.22621" -> the fifth token, split on dots and spaces.
rem This WinPE has find.exe but no findstr.exe (seen 2026-10-01), and sort.exe
rem is not something to count on either; use only find.
set BUILD=
for /f "tokens=5 delims=. " %%v in ('type "%TEMP%\img.txt" ^| find /i "Version :"') do (
  if not defined BUILD set BUILD=%%v
)
if not defined BUILD (
  echo Could not read the image's build number.
  goto :fail
)
set /a BNUM=%BUILD% 2>nul
if %BNUM% LSS 10000 (
  echo Could not read the image's build number ^(got "%BUILD%"^). What DISM said:
  type "%TEMP%\img.txt"
  goto :fail
)
if %BNUM% GTR 25999 (
  echo This image is build %BNUM%. RK3576 is ARMv8.0: Windows 11 24H2
  echo ^(26100^) and later cannot run on it. Use 23H2 ^(22631^) or Windows 10.
  goto :fail
)
echo Build %BNUM%, %ARCH%.
type "%TEMP%\img.txt" >> "%LOG%"

rem --- 3. confirm, then partition --------------------------------------------
echo.
echo ======================================================================
echo  About to DELETE every partition on disk %DISK% after the firmware one,
echo  and install Windows into that space. Everything in them is lost.
echo  The firmware partition ^(sector 64, 64 MiB^) is not touched.
echo ======================================================================
set /p OK=Type ERASE to continue, anything else to stop:
if not "%OK%"=="ERASE" (
  echo Stopped. Nothing was changed.
  goto :end
)

> "%TEMP%\dp.txt" (
  echo select disk %DISK%
  echo list partition
)
diskpart /s "%TEMP%\dp.txt" > "%TEMP%\parts.txt"
type "%TEMP%\parts.txt" >> "%LOG%"

rem Count the partitions ("Partition ###" is the header), then delete from
rem the highest number down to 2: removing the last one never renumbers the
rem ones before it. Partition 1 stays.
set NPART=0
for /f "tokens=2" %%p in ('type "%TEMP%\parts.txt" ^| find "Partition "') do (
  if not "%%p"=="###" set /a NPART+=1
)
echo partitions before: %NPART% >> "%LOG%"
if %NPART% LSS 1 (
  echo diskpart listed no partitions on disk %DISK%. Stopping.
  goto :faillog
)
> "%TEMP%\dp.txt" echo select disk %DISK%
for /l %%i in (%NPART%,-1,2) do (
  >> "%TEMP%\dp.txt" echo select partition %%i
  >> "%TEMP%\dp.txt" echo delete partition override
)
>> "%TEMP%\dp.txt" (
  echo create partition efi size=260
  echo format quick fs=fat32 label=System
  echo assign letter=S
  echo create partition msr size=16
  echo create partition primary
  echo format quick fs=ntfs label=Windows
  echo assign letter=W
  echo list partition
)
type "%TEMP%\dp.txt" >> "%LOG%"
diskpart /s "%TEMP%\dp.txt" >> "%LOG%" 2>&1 || goto :faillog
if not exist S:\ goto :faillog
if not exist W:\ goto :faillog

rem Re-check that partition 1 is still the firmware region, as diskpart saw it.
set P0OFF=
for /f "tokens=2 delims==" %%a in ('wmic partition where "DiskIndex=%DISK% and Index=0" get StartingOffset /value ^| find "="') do for %%c in (%%a) do set P0OFF=%%c
if not "%P0OFF%"=="32768" (
  echo The firmware partition is not where it was. Stopping before writing.
  goto :faillog
)

rem --- 4. apply, drivers, boot ------------------------------------------------
echo.
echo Applying the image. This takes a while on eMMC.
dism /English /Apply-Image /ImageFile:"%WIM%" /Index:%IDX% /ApplyDir:W:\ /Compact >> "%LOG%" 2>&1 || goto :faillog

echo Adding the RK3576 drivers.
dism /English /Image:W:\ /Add-Driver /Driver:"%DRIVERS%" /Recurse /ForceUnsigned >> "%LOG%" 2>&1 || goto :faillog

echo Writing the boot files.
bcdboot W:\Windows /s S: /f UEFI >> "%LOG%" 2>&1 || goto :faillog
bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} testsigning on >> "%LOG%" 2>&1 || goto :faillog
if not exist S:\EFI\Boot\bootaa64.efi (
  mkdir S:\EFI\Boot 2>nul
  copy /y S:\EFI\Microsoft\Boot\bootmgfw.efi S:\EFI\Boot\bootaa64.efi >> "%LOG%" 2>&1
)

echo Letting OOBE finish without a network.
reg load HKLM\WOAOFF W:\Windows\System32\config\SOFTWARE >> "%LOG%" 2>&1 || goto :faillog
reg add HKLM\WOAOFF\Microsoft\Windows\CurrentVersion\OOBE /v BypassNRO /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg unload HKLM\WOAOFF >> "%LOG%" 2>&1

echo.
echo Done. Log: %LOG%
echo Take the stick out and the SD card out, then reboot. The firmware's boot
echo menu should offer "Windows Boot Manager"; if it does not, pick the eMMC.
goto :end

:badsize
echo Disk %DISK% is %SIZE% bytes, not the 28-33 GB eMMC this is written for.
goto :fail
:badfw
echo Disk %DISK% partition 1 starts at %P0OFF% and is %P0SIZE% bytes long.
echo The firmware region should start at 32768 and be 67076096 bytes. This is
echo not the layout this script expects, so it will not touch the disk.
goto :fail
:faillog
echo Something failed. Full log follows; it is also in %LOG%.
type "%LOG%"
:fail
echo.
echo Nothing was installed.
:end
endlocal
