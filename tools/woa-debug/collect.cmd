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

rem --- PROBE: what the PnP arbiter actually handed out ------------------------
rem HKLM\HARDWARE\RESOURCEMAP is the arbiter's output, not a device's wish
rem list: every allocated interrupt vector, memory range and port, by owner.
rem The Enum dumps above say what each device *asks* for; this says what the
rem machine actually granted, which is the only way to see why an allocation
rem could not be made. Added after a session where every requirement looked
rem satisfiable in isolation and the device still got CM_PROB_NORMAL_CONFLICT.
reg query HKLM\HARDWARE\RESOURCEMAP /s > "%OUT%\60-resourcemap.txt" 2>&1

rem --- PROBE: the firmware-described hardware tree ----------------------------
rem What Windows built from the ACPI tables before any driver ran.
reg query "HKLM\HARDWARE\DESCRIPTION\System" /s > "%OUT%\61-hw-description.txt" 2>&1

rem --- PROBE: which ACPI tables Windows loaded, by signature ------------------
rem Confirms from the OS side that the tables the firmware installed are the
rem ones in use -- cheaper than inferring it from a build stamp.
reg query HKLM\HARDWARE\ACPI > "%OUT%\62-acpi-tables.txt" 2>&1

rem --- PROBE: devices with a problem, listed on their own ---------------------
pnputil /enum-devices /problem > "%OUT%\63-problem-devices.txt" 2>&1

rem --- PROBE: Setup's own PnP complaints --------------------------------------
rem setupact.log is copied whole above; this pulls the lines worth reading
rem first, so a 24 KB log does not have to be moved over a serial console.
rem WinPE ships find, not findstr -- findstr is not in boot.wim at all, and
rem the 2026-09-18 run produced nothing but "is not recognized".  find takes
rem one string per call, so loop over them.
type nul > "%OUT%\64-setupact-pnp.txt"
for %%S in ("pci" "resource" "arbit" "conflict" "nvme") do (
    echo. >> "%OUT%\64-setupact-pnp.txt"
    echo === %%~S === >> "%OUT%\64-setupact-pnp.txt"
    find /i %%S X:\Windows\Panther\setupact.log >> "%OUT%\64-setupact-pnp.txt" 2>&1
)

rem --- OPTIONAL: load our own drivers ----------------------------------------
rem Only runs if the packages are on the stick. Unsigned kernel drivers need
rem test signing enabled in the stick's BCD (bcdedit /store ... /set testsigning
rem on) and Secure Boot off, or drvload fails with a signature error - that
rem failure is itself a useful result, so it is recorded either way.
rem
rem What WinPE actually has, checked against the ADK 22621 boot.wim with
rem `wimlib-imagex dir` rather than assumed: msgpioclx.sys, SpbCx.sys,
rem sdport.sys, sdbus.sys and sdstor.sys are all present. An earlier note
rem here said GpioClx and SpbCx were absent and that gpio/i2c/spi therefore
rem could not load; that was wrong.
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

rem --- PROBE: how many CPUs the kernel actually started ---------------------
rem One subkey per running processor. This read 1 for a long time; it should
rem now be 0..7 (MADT GICC CPU interface numbers).
reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor" > "%OUT%\70-cpus.txt" 2>&1

rem --- PROBE: the storage controllers, on their own -------------------------
rem 10-devices.txt has everything, but these are the two the whole storage
rem story turns on and they are worth having in a file of their own:
rem   ACPI\RKCP0D40  eMMC  (DWCMSHC, SDHCI-compatible, inbox sdbus binds)
rem   ACPI\RKCPFE2C  SD    (dw_mmc, needs rkdwmmc from this repo)
rem Three outcomes to tell apart: no driver bound at all (INF/hardware-id or
rem signing), bound but failed to start (the error code says why), or started
rem with no child (the controller is up and the card is not).
pnputil /enum-devices /class SDHost > "%OUT%\71-sdhost.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\RKCPFE2C" >> "%OUT%\71-sdhost.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\RKCP0D40" >> "%OUT%\71-sdhost.txt" 2>&1

rem --- PROBE: which third-party drivers are staged and loadable -------------
rem /enum-drivers lists the driver store; 40-drivers.txt has said "no published
rem driver packages" on every stock image. If the injected ones are missing
rem here, the question is the image, not the driver.
pnputil /enum-drivers > "%OUT%\72-drivers-staged.txt" 2>&1

rem --- PROBE: disks, again, after everything above --------------------------
echo list disk > "%OUT%\dp2.txt"
echo list volume >> "%OUT%\dp2.txt"
diskpart /s "%OUT%\dp2.txt" > "%OUT%\73-diskpart-late.txt" 2>&1
del "%OUT%\dp2.txt" 2>nul

echo done >> "%OUT%\00-index.txt"
endlocal
