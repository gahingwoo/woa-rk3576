# Installing Windows on the CM5-IO

The same procedure installed Windows on the LCKFB TaishanPi 3M; what differs
there is in [its own section](#lckfb-taishanpi-3m) at the end.

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

- The latest firmware from
  [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/releases/latest), on
  eMMC partition 1. It can be written from the browser at
  [flash.gahingwoo.com](https://flash.gahingwoo.com/).
- The WinPE image from this repo's
  [release 0.1.1](https://github.com/gahingwoo/woa-rk3576/releases/latest)
  or later, on a USB stick.
- `install.wim` or `install.esd` from a Windows 11 23H2 ARM64 ISO.

Do not mix in older parts. Firmware before 0.3.0 and the drivers of 0.1.0
both ran the eMMC at 148.5 MHz instead of 52 MHz, and files written under
Windows could come back different with no error reported. Firmware before
0.2.0 also has no working Ethernet or USB-C under Windows. For the CM5-IO
the first firmware images with the clock fix are those of 0.4.0, and no
installation has been done with them yet.

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

1. finds the eMMC (the one `SD\` disk of 28 to 33 GB, or 57 to 63 GB) and checks that its first
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

## LCKFB TaishanPi 3M

Installed once, on 13 November 2026, with Windows 11 Enterprise 23H2
(22631.2428) and firmware 0.3.0. That installation has since booted with
firmware 0.4.0 and 0.4.1.

- Firmware: the TaishanPi 3M image from the
  [latest edk2-rk3576 release](https://github.com/gahingwoo/edk2-rk3576/releases/latest),
  written to the eMMC as the whole image: "UEFI firmware, whole image" at
  [flash.gahingwoo.com](https://flash.gahingwoo.com/), or
  `rkdeveloptool wl 0` with the `emmc` file. That creates the firmware
  partition the script checks for. It also replaces what the eMMC boots
  today: LCKFB's stock system will not start again until the stock firmware
  is flashed back.
- To update the firmware later, under an installed Windows, use "UEFI
  firmware, keep the partitions". The whole image would erase the partition
  table.
- The firmware has to be on the eMMC. With the firmware on an SD card the
  script stops, because it then sees two SD/eMMC disks.
- The eMMC is 62008590336 bytes. The script accepts 57 to 63 GB from the
  WinPE image of release 0.1.1 on.
- The stick has to be on a USB-A port. The firmware does not use the USB-C
  port, and it is not in the board's ACPI tables.
- Touch works in the firmware's menus and, with the
  [gt9271](../drivers/input/gt9271) driver, in Windows (10 touch points). It
  needs firmware whose DSDT declares the touch controller
  (`\_SB.I2C0.TCH0`, `RKCP9271`): edk2-rk3576 built from `main` after
  0.4.1; no release has it yet. With no screen attached the firmware hides
  the device and nothing appears in Device Manager. Keep a USB keyboard at
  hand for WinPE and the first setup.
- To add a driver to an installed Windows with `pnputil` instead of
  `add-drivers.cmd`, take it from the WinPE artifact's `woa-drivers\`, not
  from the CI build artifact: only the WinPE workflow signs the catalogs. Each
  run signs with a new test certificate, so trust that certificate first.
  It can be taken out of the catalog itself (on Linux:
  `openssl pkcs7 -inform DER -in gt9271.cat -print_certs -out signer.pem`,
  then `openssl x509 -in signer.pem -outform DER -out signer.cer`), then on
  the board from an administrator prompt:
  `certutil -addstore -f Root signer.cer`,
  `certutil -addstore -f TrustedPublisher signer.cer`,
  `pnputil /add-driver gt9271.inf /install`.
- With LCKFB's 800x1280 DSI screen attached, the firmware uses it instead of
  HDMI, and Windows shows its picture there in portrait; wddm-mali-g52's
  rotation (`next` branch) lets Settings turn it to landscape. An installed Windows has been seen booting that way. The installation
  itself was done over HDMI and has not been repeated on the screen. To
  install over HDMI, unplug the screen's ribbon with the power off.
- Windows Setup from a plain install stick starts on this board but does not
  see the eMMC, for the reason given above: the driver is not inbox.
- Do not make the stick with Ventoy. With this WinPE image its UDF driver
  stopped with a synchronous exception.

Not available under Windows on this board: WiFi and Bluetooth, audio (HDMI
and headphone audio are written and not run, see [audio](AUDIO.md)), the RTC (the clock is wrong until it syncs) and the EC20 modem's network
interface.

Everything else is as for the CM5-IO.
