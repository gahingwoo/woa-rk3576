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

rem Every probe stamps the index with the time it finished.
rem
rem "the spinner turns very slowly" was the observation that finally
rem explained a whole day of intermittent failures -- the system was not
rem hung, it was starved, and it ended in a DRIVER_PNP_WATCHDOG bugcheck.
rem Slowness has to be measured, not felt: a probe that normally takes a
rem second and takes forty says where the time goes, and which one it is
rem narrows the cause. pnputil enumerating devices is the one to watch.
echo woa-rk3576 WinPE collection > "%OUT%\00-index.txt"
echo date: %DATE% %TIME% >> "%OUT%\00-index.txt"

rem --- PROBE: every device Windows enumerated, and whether a driver bound ----
rem This is the one that matters most right now. A device present with no
rem driver is a different problem from a device that never appeared at all.
pnputil /enum-devices > "%OUT%\10-devices.txt" 2>&1
echo   %TIME%  10-devices.txt >> "%OUT%\00-index.txt"

rem --- PROBE: the ACPI branch of the device tree ------------------------------
rem Shows what came out of the DSDT by _HID, including devices pnputil may
rem summarise away. RKCP0D40 is the eMMC, RKCPFE2C the SD slot, RKCP6543 the
rem GMAC, RKCP300x the GPIO/I2C/SPI blocks.
reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI /s > "%OUT%\11-enum-acpi.txt" 2>&1
echo   %TIME%  11-enum-acpi.txt >> "%OUT%\00-index.txt"

rem --- PROBE: the PCI branch -------------------------------------------------
rem Empty here means the root bridge enumerated nothing behind it, which is a
rem firmware/ECAM question, not a driver one.
reg query HKLM\SYSTEM\CurrentControlSet\Enum\PCI /s > "%OUT%\12-enum-pci.txt" 2>&1
echo   %TIME%  12-enum-pci.txt >> "%OUT%\00-index.txt"

rem --- PROBE: storage as Setup sees it ---------------------------------------
echo list disk > "%OUT%\dp.txt"
echo list volume >> "%OUT%\dp.txt"
diskpart /s "%OUT%\dp.txt" > "%OUT%\20-diskpart.txt" 2>&1
echo   %TIME%  20-diskpart.txt >> "%OUT%\00-index.txt"
del "%OUT%\dp.txt" 2>nul

rem --- PROBE: Setup's own logs -----------------------------------------------
rem setuperr.log is short and names what Setup itself objected to.
if exist X:\Windows\Panther\setupact.log copy /y X:\Windows\Panther\setupact.log "%OUT%\30-setupact.log" >nul 2>&1
if exist X:\Windows\Panther\setuperr.log copy /y X:\Windows\Panther\setuperr.log "%OUT%\31-setuperr.log" >nul 2>&1

rem --- PROBE: drivers already staged in this WinPE ---------------------------
pnputil /enum-drivers > "%OUT%\40-drivers.txt" 2>&1
echo   %TIME%  40-drivers.txt >> "%OUT%\00-index.txt"

rem --- PROBE: what the PnP arbiter actually handed out ------------------------
rem HKLM\HARDWARE\RESOURCEMAP is the arbiter's output, not a device's wish
rem list: every allocated interrupt vector, memory range and port, by owner.
rem The Enum dumps above say what each device *asks* for; this says what the
rem machine actually granted, which is the only way to see why an allocation
rem could not be made. Added after a session where every requirement looked
rem satisfiable in isolation and the device still got CM_PROB_NORMAL_CONFLICT.
reg query HKLM\HARDWARE\RESOURCEMAP /s > "%OUT%\60-resourcemap.txt" 2>&1
echo   %TIME%  60-resourcemap.txt >> "%OUT%\00-index.txt"

rem --- PROBE: the firmware-described hardware tree ----------------------------
rem What Windows built from the ACPI tables before any driver ran.
reg query "HKLM\HARDWARE\DESCRIPTION\System" /s > "%OUT%\61-hw-description.txt" 2>&1
echo   %TIME%  61-hw-description.txt >> "%OUT%\00-index.txt"

rem --- PROBE: which ACPI tables Windows loaded, by signature ------------------
rem Confirms from the OS side that the tables the firmware installed are the
rem ones in use -- cheaper than inferring it from a build stamp.
reg query HKLM\HARDWARE\ACPI > "%OUT%\62-acpi-tables.txt" 2>&1

rem --- PROBE: devices with a problem, listed on their own ---------------------
pnputil /enum-devices /problem > "%OUT%\63-problem-devices.txt" 2>&1
echo   %TIME%  63-problem-devices.txt >> "%OUT%\00-index.txt"

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
echo   %TIME%  70-cpus.txt >> "%OUT%\00-index.txt"

rem --- PROBE: the storage controllers, on their own -------------------------
rem 10-devices.txt has everything, but these are the two the whole storage
rem story turns on and they are worth having in a file of their own:
rem   ACPI\RKCP0D40  eMMC  (DWCMSHC, SDHCI-compatible, inbox sdbus binds)
rem   ACPI\RKCPFE2C  SD    (dw_mmc, needs rkdwmmc from this repo)
rem Three outcomes to tell apart: no driver bound at all (INF/hardware-id or
rem signing), bound but failed to start (the error code says why), or started
rem with no child (the controller is up and the card is not).
pnputil /enum-devices /class SDHost > "%OUT%\71-sdhost.txt" 2>&1
echo   %TIME%  71-sdhost.txt >> "%OUT%\00-index.txt"
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
echo   %TIME%  73-diskpart-late.txt >> "%OUT%\00-index.txt"
del "%OUT%\dp2.txt" 2>nul

rem --- PROBE: what rkdwmmc recorded about itself -------------------------
rem The driver publishes a snapshot here rather than tracing, because the
rem kernel debugger is not a usable instrument on this board: with no
rem listener the target retransmits forever and storage enumeration times
rem out, and with one attached the exchange stalls on RESEND.
rem
rem CardDetectRaw is the first thing to read. ACPI routes card detect
rem through a GpioInt, but the driver reads the controller CDETECT; if bit
rem 0 is set the slot reports empty and sdport never initialises a card.
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkdwmmc\Diag" /s > "%OUT%\74-rkdwmmc-diag.txt" 2>&1
echo   %TIME%  74-rkdwmmc-diag.txt >> "%OUT%\00-index.txt"

rem --- PROBE: rkemmc, the eMMC miniport, same idea -----------------------
rem Read VendorBitsBefore first.  It is EMMC_CTRL as the driver found it
rem straight after an SDHCI RESET_ALL, and it is the premise the whole
rem driver rests on: bits 0 and 2 clear means the reset really does wipe
rem CARD_IS_EMMC and EMMC_RST_N and the restore is needed.  If they are
rem SET, the premise is wrong and docs/STORAGE.md needs rewriting -- that
rem would matter more than the card not enumerating.
rem
rem Then VendorBitsAfter (did the write-back take), ClockStableWaits and
rem ClockStableTimeouts (did the internal clock relock after the CRU rate
rem changed), and RequestCalls/SeenErrStatus (did commands go out at all,
rem and what did the controller say about them).
rem
rem This has to run inside WinPE.  HKLM\SYSTEM\CurrentControlSet lives on
rem the RAM disk there, so the snapshot is gone the moment the board
rem reboots -- the first rkemmc run was lost that way, because the driver
rem shipped before this probe did.
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkemmc\Diag" /s > "%OUT%\74b-rkemmc-diag.txt" 2>&1
echo   %TIME%  74b-rkemmc-diag.txt >> "%OUT%\00-index.txt"

rem Cmd00..CmdNN in that file are the command trace, one DWORD each:
rem   [31:24] sequence  [23:16] command index
rem   [15:8]  ERR_INT_STATUS low byte   [7:0] INT_STATUS low byte
rem   bit 31  set = the command never reached the command register
rem A slot whose two status bytes are both 0 is a command that went out
rem and was never answered -- a different fault from one never issued.

rem --- PROBE: the two storage devices, in full ----------------------------
rem 11-enum-acpi.txt has these, buried in 200 KB. Their own file keeps the
rem fields that matter together: Service, LogConf/BootConfig (what the
rem arbiter granted), Device Parameters, and whether a child node exists.
rem A controller Started with no child under it is the shape both the eMMC
rem and the SD slot have been stuck in.
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0D40" /s > "%OUT%\75-emmc-enum.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCPFE2C" /s > "%OUT%\76-sd-enum.txt" 2>&1
echo   %TIME%  75/76-storage-enum >> "%OUT%\00-index.txt"

rem --- PROBE: the GPIO controllers -----------------------------------------
rem rk3576gpio drives five of these, and the SD slot's card detect is a
rem GpioInt on \_SB.GPI0 routed through them. It is also the other suspect
rem for the DRIVER_PNP_WATCHDOG: a PnP callback that does not return starves
rem everything behind it, which is what "the spinner turns very slowly" is.
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3002" /s > "%OUT%\77-gpio-enum.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\RKCP3002" >> "%OUT%\77-gpio-enum.txt" 2>&1
echo   %TIME%  77-gpio-enum >> "%OUT%\00-index.txt"

rem --- PROBE: services, and which of ours are running ----------------------
rem sc.exe is not in every WinPE (it is absent from the ADK 22621 image), so
rem read the service keys instead. Start and ErrorControl say how the driver
rem was meant to load; a driver that never loaded has no Enum subkey.
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkdwmmc" /s > "%OUT%\78-services.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkemmc" /s >> "%OUT%\78-services.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rk3576gpio" /s >> "%OUT%\78-services.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\sdbus" /s >> "%OUT%\78-services.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\sdstor" /s >> "%OUT%\78-services.txt" 2>&1
echo   %TIME%  78-services >> "%OUT%\00-index.txt"

rem --- PROBE: the last bugcheck, if the firmware kept it -------------------
rem WinPE has no crash dump, but a bugcheck that happened before a warm
rem reboot can leave its code here. DRIVER_PNP_WATCHDOG is 0x1D5 and names
rem the stuck device object in its parameters.
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /s > "%OUT%\79-crashcontrol.txt" 2>&1
echo   %TIME%  79-crashcontrol >> "%OUT%\00-index.txt"

rem =========================================================================
rem  EXPERIMENT: does an insertion edge wake sdport up?
rem =========================================================================
rem
rem Everything above this line is automatic and is already on disk.  What
rem follows needs a hand on the SD card, so it pauses.  Closing the window
rem here loses nothing.
rem
rem Why: the ACPI card detect in Sdhc.asl is
rem
rem     GpioInt (Edge, ActiveBoth, Shared, PullUp, 0, "\_SB.GPI0") { ... }
rem
rem an EDGE.  A card already in the slot when the machine boots produces no
rem edge, so sdport would be waiting for an event that happened before it was
rem watching.  That fits what the driver reports: on 2026-09-20, with a card
rem physically in the slot, rkdwmmc recorded CardDetectCalls 0, BusOpCalls 0,
rem RequestCalls 0 -- sdport read the capabilities and never came back.
rem
rem If ejecting and reinserting makes the count move, the hypothesis holds and
rem the fix is in the ACPI description, not in the driver.  If it does not,
rem the hypothesis is dead and the next place to look is the GPIO driver.
echo.
echo ==========================================================
echo  SD card experiment.  Close this window to skip it.
echo ==========================================================
echo.
echo  Step 1 of 3: make sure the SD card IS in the slot.
pause
echo list disk > "%OUT%\dp3.txt"
diskpart /s "%OUT%\dp3.txt" > "%OUT%\80-sd-before.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkdwmmc\Diag" /s > "%OUT%\81-diag-before.txt" 2>&1
echo   %TIME%  80/81 sd-before >> "%OUT%\00-index.txt"

echo.
echo  Step 2 of 3: EJECT the card now, wait two seconds, then press a key.
pause
diskpart /s "%OUT%\dp3.txt" > "%OUT%\82-sd-ejected.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkdwmmc\Diag" /s > "%OUT%\83-diag-ejected.txt" 2>&1
echo   %TIME%  82/83 sd-ejected >> "%OUT%\00-index.txt"

echo.
echo  Step 3 of 3: RE-INSERT the card, wait two seconds, then press a key.
pause
diskpart /s "%OUT%\dp3.txt" > "%OUT%\84-sd-reinserted.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkdwmmc\Diag" /s > "%OUT%\85-diag-reinserted.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\rkemmc\Diag" /s > "%OUT%\86-rkemmc-diag-late.txt" 2>&1
echo   %TIME%  84/85/86 sd-reinserted >> "%OUT%\00-index.txt"
del "%OUT%\dp3.txt" 2>nul

echo.
echo  Done.  Read 81 / 83 / 85 side by side: CardDetectCalls and BusOpCalls
echo  are the numbers that answer this.
echo.

echo done >> "%OUT%\00-index.txt"
endlocal
