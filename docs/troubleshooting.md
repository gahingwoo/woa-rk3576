# Troubleshooting Windows

English | [简体中文](troubleshooting.zh-CN.md)

Firmware problems (MaskROM, flashing, settings) are on edk2-rk3576's
[troubleshooting](https://edk2.gahingwoo.com/troubleshooting.html). To
collect data from Windows, see [debugging](dev/debugging.md).

## INACCESSIBLE_BOOT_DEVICE (0x7B)

With second parameter `0xC0000034`, Windows found no driver for its boot
disk. The deploy script handles the three causes seen so far; if Windows was
installed some other way, check them:

- `dwcsdhc` must be boot-start. DISM only installs a boot-start driver
  offline; it merely stages the others.
- `sdstor` must be boot-start. It ships demand-start, and DISM does not
  change that the way Setup would.
- `sdbus` must be disabled. The eMMC's `_CID` is `PNP0D40`, sdbus matches
  it, and once PnP has bound the controller to sdbus it keeps that binding.
  If it already has, delete `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0D40`
  and its entry under the `{a0a588a4-...}` class key.

## "The computer restarted unexpectedly"

A crashed first boot interrupted the specialize pass.

1. Press Shift+F10.
2. Open `regedit`.
3. Set `HKLM\SYSTEM\Setup\Status\ChildCompletion\setup.exe` to 3.
4. Close regedit and click OK.

## Shutdown takes minutes

Fast Startup is on: it hibernates, and the hibernation path through the
eMMC driver hits a WHEA error. Run `powercfg /h off`. The deploy script and
`net-debug.cmd` already do this.

## The clock is wrong after every boot

The TaishanPi 3M's RTC holds no valid time.

1. Set
   `HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation\RealTimeIsUniversal`
   to 1 (REG_DWORD), so Windows reads the firmware's clock as UTC.
2. Set the Windows Time service to start automatically.
3. Keep a network cable in: the firmware sets its clock over NTP at boot.

Until the clock is right, `pnputil` refuses a driver signed after the time
the board believes it is ("not within its validity period"); run
`w32tm /resync /force` first.

## Windows Setup does not see the eMMC

Expected: Setup has no driver for the eMMC controller. Install with the
WinPE stick ([install](INSTALL.md)).

## The WinPE stick made with Ventoy does not start

Do not use Ventoy for this image: its UDF driver stopped with a synchronous
exception. Write the image directly.

## The deploy script stops before writing

- "more than one SD/eMMC disk": take the SD card out, and keep the firmware
  on the eMMC, not on a card.
- The first partition is not the firmware: write the firmware's whole image
  to the eMMC first.
- The image is refused: it is not ARM64, or its build is newer than 25999
  (24H2 and later cannot run).

## No sound after reinstalling an audio driver

Check the default playback device in the taskbar's sound menu
([drivers](drivers.md#replacing-a-driver-that-is-already-bound-and-running)).
