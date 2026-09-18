@echo off
setlocal enabledelayedexpansion
rem ---------------------------------------------------------------------------
rem collect.cmd - everything we want to know from inside WinPE, in one pass.
rem
rem Called by autounattend.xml with the stick's drive letter as %1. Writes to
rem <stick>\woa-debug\out\, which is readable from Linux afterwards - mount the
rem stick and read the files. Nothing here writes outside that directory and
rem nothing here touches a disk.
rem
rem To add a probe: append a PROBE block. Keep each one on its own so a command
rem that does not exist in this WinPE build cannot take the rest down with it.
rem ---------------------------------------------------------------------------

set STICK=%~1
if "%STICK%"=="" set STICK=%~d0
set OUT=%STICK%\woa-debug\out
if not exist "%OUT%" mkdir "%OUT%" 2>nul

echo woa-rk3576 WinPE collection > "%OUT%\00-index.txt"
echo date: %DATE% %TIME% >> "%OUT%\00-index.txt"

rem --- PROBE: every device Windows enumerated, and whether a driver bound ----
rem This is the one that matters most right now. A device present with no
rem driver is a different problem from a device that never appeared at all.
pnputil /enum-devices > "%OUT%\10-devices.txt" 2>&1

rem --- PROBE: the ACPI branch of the device tree ------------------------------
rem Shows what came out of the DSDT by _HID, including devices pnputil may
rem summarise away. RKCP0D40 is the eMMC, RKCPFE2C the SD slot, RKCP6543 the
rem GMAC, RKCP300x the GPIO/I2C/SPI blocks.
reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI /s > "%OUT%\11-enum-acpi.txt" 2>&1

rem --- PROBE: the PCI branch -------------------------------------------------
rem Empty here means the root bridge enumerated nothing behind it, which is a
rem firmware/ECAM question, not a driver one.
reg query HKLM\SYSTEM\CurrentControlSet\Enum\PCI /s > "%OUT%\12-enum-pci.txt" 2>&1

rem --- PROBE: storage as Setup sees it ---------------------------------------
echo list disk > "%OUT%\dp.txt"
echo list volume >> "%OUT%\dp.txt"
diskpart /s "%OUT%\dp.txt" > "%OUT%\20-diskpart.txt" 2>&1
del "%OUT%\dp.txt" 2>nul

rem --- PROBE: Setup's own logs -----------------------------------------------
rem setuperr.log is short and names what Setup itself objected to.
if exist X:\Windows\Panther\setupact.log copy /y X:\Windows\Panther\setupact.log "%OUT%\30-setupact.log" >nul 2>&1
if exist X:\Windows\Panther\setuperr.log copy /y X:\Windows\Panther\setuperr.log "%OUT%\31-setuperr.log" >nul 2>&1

rem --- PROBE: drivers already staged in this WinPE ---------------------------
pnputil /enum-drivers > "%OUT%\40-drivers.txt" 2>&1

rem --- OPTIONAL: load our own drivers ----------------------------------------
rem Only runs if the packages are on the stick. Unsigned kernel drivers need
rem test signing enabled in the stick's BCD (bcdedit /store ... /set testsigning
rem on) and Secure Boot off, or drvload fails with a signature error - that
rem failure is itself a useful result, so it is recorded either way.
rem
rem Note what WinPE does NOT have: GpioClx, SpbCx and NetAdapterCx are absent,
rem so gpio, i2c, spi and gmac cannot load here no matter how they are signed.
rem sdport IS present (WinPE boots from storage), so rkdwmmc is the one with a
rem real chance, and the SD card is the reason to care.
if exist "%STICK%\woa-debug\drivers" (
  echo drvload results > "%OUT%\50-drvload.txt"
  for /d %%D in ("%STICK%\woa-debug\drivers\*") do (
    for %%I in ("%%D\*.inf") do (
      echo ---- %%~nxI >> "%OUT%\50-drvload.txt"
      drvload "%%I" >> "%OUT%\50-drvload.txt" 2>&1
    )
  )
  rem re-run the device list so the before/after is visible in one place
  pnputil /enum-devices > "%OUT%\51-devices-after-drvload.txt" 2>&1
)

echo done >> "%OUT%\00-index.txt"
endlocal
