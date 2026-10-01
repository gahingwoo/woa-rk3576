# Installing Windows on the CM5-IO

Windows 11 23H2 Enterprise (22631.2428) installed this way boots to the
desktop from the CM5-IO's eMMC. That installation was repaired in place after
its first boots; a fresh run of the scripts as they are now has not been done.

## Which Windows

RK3576 is ARMv8.0. Windows 11 24H2 and later need ARMv8.1 atomics and cannot
run on it, and firmware cannot work around that.

| Target | Build | |
|---|---|---|
| Windows 10 ARM64 22H2 | 19045 | should work, not tried |
| Windows 11 23H2 | 22631 | works; the one tested here |
| Windows 11 24H2 and later | 26100+ | cannot boot |

`dism /Get-ImageInfo` shows an image's build. The deploy script refuses
anything newer than 25999.

## Why not Windows Setup

23H2 Setup refuses this board: the eMMC is 29 GB against a 64 GB minimum, and
there is no TPM 2.0 or Secure Boot. The installed system also needs test
signing from its first boot, because its boot disk is behind a test-signed
driver. So the image is applied with DISM from this repo's WinPE.

## What you need

- Firmware 0.2.0 or later from
  [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/releases), on eMMC
  partition 1. Earlier builds have no working Ethernet or USB-C under Windows.
- The WinPE image from this repo's release, on a USB stick.
- `install.wim` or `install.esd` from a Windows 11 23H2 ARM64 ISO.

The finished layout: firmware in eMMC partition 1 (untouched), Windows on the
rest of the eMMC, and the NVMe untouched.

## Procedure

1. Write the WinPE image to a USB stick. It has one 515 MB FAT32 partition,
   `WINPE`, with `woa-deploy\`, `woa-debug\` and the signed drivers in
   `woa-drivers\`; the rest of the stick is unallocated.
2. In the unallocated space, create an exFAT or NTFS partition and copy
   `sources\install.wim` (or `.esd`) onto it. FAT32 cannot hold it.
3. Take the SD card out. The script stops if it sees more than one SD/eMMC
   disk.
4. Boot the stick from the firmware's boot menu and wait for the collector to
   finish. Then run

   ```
   E:\woa-deploy\deploy-windows.cmd
   ```

   with the stick's drive letter, which the collector prints.

The script:

1. finds the eMMC (the one `SD\` disk of 28 to 33 GB) and checks that its first
   partition is the firmware (offset 32768, length 67076096). Anything else and
   it stops without writing.
2. finds the image, lists its editions, and refuses non-ARM64 images and builds
   newer than 25999.
3. asks you to type `ERASE`, deletes every partition after the first, and
   creates an ESP, an MSR and an NTFS partition.
4. applies the image with `/Compact`, adds the drivers, runs `bcdboot`, turns on
   test signing, makes `dwcsdhc` and `sdstor` boot-start, disables `sdbus`,
   turns off hibernation and Fast Startup, and sets `BypassNRO` so OOBE
   finishes offline.

The log is `woa-deploy\deploy.log` on the stick.

## First boot

Remove the stick and reboot. The firmware tries the SD slot and USB before the
eMMC, then boots `\EFI\Boot\bootaa64.efi` from the eMMC's ESP; `bcdboot`'s
boot entry does not reach the firmware. OOBE can finish offline. The desktop
shows "Test Mode".

`tools/woa-deploy/add-drivers.cmd` adds or updates drivers in an existing
installation from WinPE without erasing it.

## If it does not boot

`INACCESSIBLE_BOOT_DEVICE` (0x7B) with second parameter `0xC0000034` means
Windows found no driver for its boot disk. The deploy script handles the three
causes seen so far; if you installed some other way, check them:

- `dwcsdhc` must be boot-start. DISM only installs a boot-start driver
  offline; it merely stages the others.
- `sdstor` must be boot-start. It ships demand-start, and DISM does not change
  that the way Setup would.
- `sdbus` must be disabled. The eMMC's `_CID` is `PNP0D40`, sdbus matches it,
  and once PnP has bound the controller to sdbus it keeps that binding. If it
  already has, delete `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0D40` and
  its entry under the `{a0a588a4-...}` class key.

If a crashed first boot interrupted the specialize pass, the next boot says
"The computer restarted unexpectedly". Press Shift+F10, open `regedit`, set
`HKLM\SYSTEM\Setup\Status\ChildCompletion\setup.exe` to 3, and click OK.

If shutdown takes minutes, Fast Startup is on: it hibernates, and the
hibernation path through the eMMC driver hits a WHEA error. Run
`powercfg /h off`. The deploy script and `net-debug.cmd` already do this.
