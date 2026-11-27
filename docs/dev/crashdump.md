# Crash dumps from the eMMC

Windows writes a crash dump through a second copy of the boot storage
driver: `dump_dwcsdhc.sys` under `dumpsdport.sys` for the eMMC
(`RKCP0D40`). Crash dumps are off by default. Status: CI only.

## How the dump path works

From the 23H2 binaries (`crashdmp.sys`, `sdport.sys` 10.0.22621.608,
`dumpsdport.sys` 10.0.22621.1) and their public symbols:

- At boot, `crashdmp` asks the disk for its dump pointers; sdport's PDO
  copies its slot and miniport state into a dump context and preallocates a
  64 KB uncached DMA buffer.
- At bugcheck, at HIGH_LEVEL with interrupts off, `crashdmp` calls the
  copy's `DriverEntry`, then `SdhcSlotInitialize(CrashdumpMode = TRUE)`.
  dumpsdport powers the eMMC up from scratch (reset, 3.3 V, 400 kHz, CMD0 to
  CMD7, EXT_CSD, bus width, speed, clock) and polls every request through
  the miniport's interrupt and DPC routines.

What `dwcsdhc` does for this:

- `DriverEntry` skips `ExInitializeDriverRuntime` above PASSIVE_LEVEL (its
  routines bugcheck at HIGH_LEVEL).
- The card clock comes only from the CRU (`CRU_CLKSEL_CON89`; the SDHCI
  divider does not work on this controller), and the dump copy cannot map
  the CRU at HIGH_LEVEL. So the running driver publishes its CRU mapping in
  its own image, and the dump copy follows slot extension, `SD_MINIPORT`,
  `ConfigurationInfo.DeviceObject`, the FDO's driver object and
  `DriverStart` back to it, checks it is the same build (PE timestamp, size,
  checksum), the structure's magic, self pointer and CRU address, and
  borrows the mapping read-only.
- With `Parameters\CrashDump` not 1, or any check failing, the dump copy
  returns `STATUS_NOT_SUPPORTED` from `SdhcSlotInitialize` before touching a
  register: the card is left as it was and Windows records a failed dump.
- In crashdump mode only: DAT and CMD reset at the end of initialise;
  SDR50, DDR50, SDR104 and tuning masked out, so the eMMC runs high speed at
  52 MHz; interrupt signals cleared; `EMMC_RST_N` released if asserted;
  commands that could leave the user area or change the card permanently
  refused (CMD26 to CMD29, CMD35, CMD36, CMD38, CMD42, and CMD6 to EXT_CSD
  136 to 155, 162, 167, 171, 173, 177, 178, 179).

The normal path only reads `CrashDump` once and records one pointer when it
is 1. sdport sees `CrashdumpSupported = TRUE` either way, so the dump stack
loads at boot the same way.

Turning it on (administrator, then restart):

```
reg add HKLM\SYSTEM\CurrentControlSet\Services\dwcsdhc\Parameters /v CrashDump /t REG_DWORD /d 1 /f
```

Turning it off: the same with `/d 0`, or delete the value.

## Checks on Windows

From an administrator PowerShell:

```
Get-CimInstance Win32_OSRecoveryConfiguration | fl DebugInfoType,MiniDumpDirectory,OverwriteExistingDebugFile,WriteToSystemLog
reg query HKLM\SYSTEM\CurrentControlSet\Control\CrashControl
reg query HKLM\SYSTEM\CurrentControlSet\Services\dwcsdhc /s
reg query HKLM\SYSTEM\CurrentControlSet\Services\sdport /s
Get-CimInstance Win32_PageFileUsage | fl Name,AllocatedBaseSize
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='volmgr'} -MaxEvents 20 | fl TimeCreated,Id,Message
Get-WinEvent -FilterHashtable @{LogName='System'; Id=41,1001,6008} -MaxEvents 10 | fl TimeCreated,Id,Message
Get-ChildItem C:\Windows\Minidump, C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue
Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID like 'ACPI\\RKCP0D40%'" | fl DeviceID,DriverVersion,InfName
bcdedit /enum {current}
bcdedit /dbgsettings
```

What they mean:

- `CrashDumpEnabled` 3 is a minidump, 7 an automatic memory dump. The
  pagefile must be on `C:` and large enough, or `DedicatedDumpFile` set.
- volmgr event 45 or 46 at boot: the dump stack did not load or initialise
  (an import or dump pointer problem, before the miniport runs). Event 161:
  a dump was attempted and failed while writing. Neither, and still no dump:
  it died inside the dump copy.
- After a crash that produced a dump: Kernel-Power 41 carries the real
  bugcheck code, and event 1001 (BugCheck) names the dump file.
- `dwcsdhc` must be `Start` 0 (boot) with `BootFlags` 8.
- With `debug on` in the BCD, a bugcheck stops in the debugger first. Keep
  `bcdedit /debug off` for this test. With a kernel debugger attached,
  `lm m dump_*` shows the loaded dump stack.

## Installing the test build

`dwcsdhc` is the boot disk's driver. Never `pnputil /install` it, or swap
it, on the running main system. Install it offline from the rescue Windows,
where the main system is `D:`.

Get the package: `woa-drivers-signed-arm64` from a WinPE (ARM64) run that
includes this driver, folder `woa-drivers\dwcsdhc\` (`dwcsdhc.inf`,
`dwcsdhc.sys`, `dwcsdhc.cat`). Note its `DriverVer` (the last field is the WinPE run number).

On the main system, before leaving it: `bcdedit /debug off` if it is on,
then `bcdedit /bootsequence <rescue guid>` (`C:\drvold\rescue-guid.txt`) and
`shutdown /r /t 0`.

In the rescue Windows, administrator PowerShell, package copied to
`C:\pkg\dwcsdhc`:

1. Find the old package(s):
   `dism /Image:D:\ /Get-Drivers /Format:Table | findstr /i dwcsdhc`
   Each line gives an `oemN.inf`. `dism /Image:D:\ /Get-DriverInfo /Driver:oemN.inf`
   shows its version.
2. Remove each old one, then add the new one:
   `dism /Image:D:\ /Remove-Driver /Driver:oemN.inf`
   `dism /Image:D:\ /Add-Driver /Driver:C:\pkg\dwcsdhc\dwcsdhc.inf`
   Run `/Get-Drivers` again: exactly one `dwcsdhc.inf`, the new version.
3. Check the file the boot loader will load is the new one:
   `Get-FileHash D:\Windows\System32\drivers\dwcsdhc.sys, C:\pkg\dwcsdhc\dwcsdhc.sys`
   If the hashes differ, `Remove-Item D:\Windows\System32\drivers\dwcsdhc.sys`
   then `Copy-Item C:\pkg\dwcsdhc\dwcsdhc.sys D:\Windows\System32\drivers\`
   (remove first: the old file is a hard link into the DriverStore).
4. Registry of the main system:
   ```
   reg load HKLM\OFF D:\Windows\System32\config\SYSTEM
   reg query HKLM\OFF\Select /v Current
   ```
   `Current` is normally 1, so the control set is `ControlSet001` (use
   `ControlSet00<Current>` otherwise):
   ```
   reg add HKLM\OFF\ControlSet001\Services\dwcsdhc\Parameters /v CrashDump /t REG_DWORD /d 1 /f
   reg add HKLM\OFF\ControlSet001\Control\CrashControl /v CrashDumpEnabled /t REG_DWORD /d 3 /f
   reg add HKLM\OFF\ControlSet001\Control\CrashControl /v AutoReboot /t REG_DWORD /d 1 /f
   reg query HKLM\OFF\ControlSet001\Services\dwcsdhc
   reg query HKLM\OFF\ControlSet001\Control\CrashControl
   reg unload HKLM\OFF
   ```
   Expect `Start` 0 and `ImagePath` `System32\drivers\dwcsdhc.sys` for the
   service, and `MinidumpDir` `%SystemRoot%\Minidump` and `DumpFile`
   `%SystemRoot%\MEMORY.DMP` in CrashControl. Close every `regedit` and
   shell that has the hive open before `reg unload`.
5. `shutdown /r /t 0`. `/bootsequence` was one-time, so this boots the main
   system.

Back in the main system, the checks above should show the new
`DriverVersion` (if it still shows the old one, PnP kept its old binding;
the driver that runs is the file from step 3 either way), `CrashDump` 1,
and no volmgr event 45 or 46 from this boot. The pagefile must be on `C:`
(`Win32_PageFileUsage`); a minidump needs only a few MB of it.

Install it on the main system only. The rescue Windows keeps its own copy
of the old driver, which is what makes the way back below possible.

## The one-shot test

Back up anything you care about first: the test crashes the board once.
Save the UEFI variables too if you might reflash firmware afterwards.

1. Check `CrashDump` is 1, `CrashDumpEnabled` 3 and the pagefile is on
   `C:` (the install above sets the first two; online it is the `reg add`
   lines above plus a restart). Keep `bcdedit /debug off`.
2. Start with the minidump (3). Once that works, try a kernel dump (2),
   which needs a pagefile at least as big as the kernel memory in use
   (usually a few hundred MB here), or a dedicated dump file:
   `reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v DedicatedDumpFile /t REG_SZ /d C:\dedicateddumpfile.sys /f`
   `reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v DumpFileSize /t REG_DWORD /d 1500 /f` (MB)
   and restart once so the dump stack picks it up.
3. Crash it, either way:
   - USB keyboard: `reg add HKLM\SYSTEM\CurrentControlSet\Services\kbdhid\Parameters /v CrashOnCtrlScroll /t REG_DWORD /d 1 /f`,
     restart, then hold the right Ctrl key and press Scroll Lock twice.
     Bugcheck 0xE2 (MANUALLY_INITIATED_CRASH).
   - Over SSH, no keyboard: `Stop-Process -Name wininit -Force`. Bugcheck
     0xEF (CRITICAL_PROCESS_DIED).
   - Sysinternals NotMyFault with `/crash`: bugcheck 0xD1 from its own
     signed driver. Use the native ARM64 executable from the package; an
     x64 one runs emulated and cannot load the driver.
   - Not `tools/wdthang`: it hangs every CPU with interrupts masked, so
     there is no bugcheck and no dump, only a watchdog reset.
4. The blue screen counts its percentage up while it writes. Let it restart.
5. Expected: a new file in `C:\Windows\Minidump`, event 1001 naming it, and
   Kernel-Power 41 with bugcheck code 0xE2 or 0xEF:
   ```
   Get-ChildItem C:\Windows\Minidump, C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue
   wevtutil qe System /q:"*[System[(EventID=1001)]]" /c:3 /rd:true /f:text
   wevtutil qe System /q:"*[System[(EventID=41 or EventID=161)]]" /c:3 /rd:true /f:text
   ```
   Open the dump with WinDbg (`!analyze -v`) to be sure it is readable.

   Copy the file to another machine (`scp`) and open it there with WinDbg
   (`windbg -z MEMORY.DMP`, `.symfix; .reload; !analyze -v`), or with
   `kd.exe -z` on the board itself.
6. Then the real case: leave `CrashDump` at 1 and wait for the next
   MEMORY_MANAGEMENT. The four parameters and the stack are in the dump.

If the percentage stays at 0 and volmgr logs event 161 after the restart,
the dump copy refused to start: check `CrashDump` is 1 and that the boot
really used the build with this change (the share is matched by PE
timestamp, so the running and dump copies must be the same file).
If the percentage never moves, or no file appears, collect the volmgr events
above and, for the next attempt, a serial KD session
([debugging.md](debugging.md)) to see where the dump copy stops.

## Backing out

The first thing to try is `CrashDump` 0: with it the dump copy does nothing.

The old driver stays in the DriverStore
(`C:\Windows\System32\DriverStore\FileRepository\dwcsdhc.inf_arm64_*`, one
folder per version). If Windows still boots, the cleanest way back is to
copy the old `dwcsdhc.sys` over `C:\Windows\System32\drivers\dwcsdhc.sys`
from the rescue system, as below.

If the main Windows no longer boots:

- Rescue Windows (TaishanPi 3M): pick "Windows 11 Rescue" in the boot menu
  (or `bcdedit /bootsequence <guid>` beforehand). The main system is `D:`
  there. Copy the old `D:\Windows\System32\DriverStore\FileRepository\dwcsdhc.inf_arm64_<old>\dwcsdhc.sys`
  to `D:\Windows\System32\drivers\dwcsdhc.sys` and restart. To make PnP
  forget the new package as well: `dism /Image:D:\ /Remove-Driver /Driver:oemN.inf`
  with the `oemN.inf` of the new build.
- Linux on the same eMMC (if installed): mount the main partition with
  `ntfs3` read-write. Do not use `cp` (it can write zeros for a compressed
  source on ntfs3). `cat` the old file to `/tmp`, `rm` the target, then
  `dd if=/tmp/dwcsdhc.sys of=<target> conv=fsync`, then
  `echo 3 > /proc/sys/vm/drop_caches` and compare SHA-256 with the source.
  `drivers\dwcsdhc.sys` is a hard link into the DriverStore, so remove it
  rather than writing into it, or the DriverStore copy changes too.
