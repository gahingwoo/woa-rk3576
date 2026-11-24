# Installing Windows

How to install Windows 11 23H2 on the eMMC of an ArmSoM CM5-IO or an LCKFB
TaishanPi 3M with the WinPE stick from this repository.
[简体中文](zh-CN/install.md)

New to these boards? Start at
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576#from-a-new-board-to-windows-or-linux).
[Maskrom Studio](https://github.com/gahingwoo/maskrom-studio) does the same
install from a PC over USB, without a stick; it has not been run on a board
yet (2026-11-24).

Installed this way: Windows 11 23H2 Enterprise (22631.2428) on the CM5-IO on
2026-10-01 (repaired in place after its first boots), and on the TaishanPi 3M
on 2026-11-13, a fresh run of these scripts.

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

23H2 Setup has no driver for the eMMC controller, so it does not see the
eMMC. It also refuses these boards for having no TPM 2.0 and no Secure Boot,
and on the CM5-IO for an eMMC smaller than 64 GB. The installed system needs
test signing from its first boot, because its boot disk is behind a
test-signed driver. So the image is applied with DISM from this repository's
WinPE.

## What you need

- The firmware from
  [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/releases/latest),
  written to the eMMC **as the whole image** (it creates the firmware
  partition the script checks for). From the browser:
  [flash.gahingwoo.com](https://flash.gahingwoo.com/), "UEFI firmware, whole
  image". How: [edk2-rk3576 flashing](https://edk2.gahingwoo.com/flash.html).
- The WinPE image from this repository's
  [latest release](https://github.com/gahingwoo/woa-rk3576/releases/latest)
  (0.1.1 or later), and a USB stick large enough for it and `install.wim`.
- `install.wim` or `install.esd` from a Windows 11 23H2 ARM64 ISO.
- A USB keyboard. On the TaishanPi 3M, an HDMI monitor or LCKFB's DSI screen.

Do not mix in older parts. Firmware before 0.3.0 on the TaishanPi 3M, before
0.4.0 on the CM5-IO, and the drivers of 0.1.0 ran the eMMC at 148.5 MHz
instead of 52 MHz, and files written under Windows could come back different
with no error reported.

On the TaishanPi 3M, touch, Bluetooth, the headphone jack, HDMI audio and the
USB-C port need firmware newer than 0.4.1: the edk2-rk3576
[`ci` build](https://edk2.gahingwoo.com/flash.html#the-rolling-ci-build)
until the next release. The install itself works with 0.4.1.

The finished layout: firmware in eMMC partition 1 (untouched), Windows on the
rest of the eMMC, any NVMe untouched.

## Steps

1. Write the WinPE image to the USB stick. It has one 515 MB FAT32
   partition, `WINPE`, with `woa-deploy\`, `woa-debug\` and the signed drivers
   in `woa-drivers\`; the rest of the stick is unallocated.
2. In the unallocated space, create an exFAT or NTFS partition and copy
   `sources\install.wim` (or `.esd`) onto it. FAT32 cannot hold it.
3. Take any SD card out of the board. The script stops if it sees more than
   one SD/eMMC disk.
4. Plug the stick into a USB-A port. (On the TaishanPi 3M, the USB-C port
   works only for a device attached before power-on.)
5. Power on, open the firmware's boot menu and boot the stick.
6. Wait for the collector to finish. It prints the stick's drive letter.
7. Run, with that drive letter:

   ```
   E:\woa-deploy\deploy-windows.cmd
   ```

8. Type `ERASE` when it asks.
9. When it has finished, remove the stick and reboot.

Windows finishes its setup (OOBE can finish offline) and reaches the
desktop, which shows "Test Mode".

The script's log is `woa-deploy\deploy.log` on the stick.

## What the script does

1. Finds the eMMC (the one `SD\` disk of 28 to 33 GB, or 57 to 63 GB) and
   checks that its first partition is the firmware (offset 32768, length
   67076096). Anything else and it stops without writing.
2. Finds the image, lists its editions, and refuses non-ARM64 images and
   builds newer than 25999.
3. Asks you to type `ERASE`, deletes every partition after the first, and
   creates an ESP, an MSR and an NTFS partition.
4. Applies the image with `/Compact`, adds the drivers, runs `bcdboot`, turns
   on test signing, makes `dwcsdhc` and `sdstor` boot-start, disables `sdbus`,
   turns off hibernation and Fast Startup, and sets `BypassNRO` so OOBE
   finishes offline.

## First boot

The firmware tries the SD slot and USB before the eMMC, then boots
`\EFI\Boot\bootaa64.efi` from the eMMC's ESP; `bcdboot`'s boot entry does not
reach the firmware unless "Save OS Variable Changes to eMMC" is on.

After the first boot:

- set the clock up as in [troubleshooting](troubleshooting.md#the-clock-is-wrong-after-every-boot)
  (TaishanPi 3M);
- add the drivers you want: [features](features.md),
  [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) for the GPU.

`tools/woa-deploy/add-drivers.cmd` adds or updates drivers in an existing
installation from WinPE without erasing it. Other ways:
[drivers](drivers.md).

## Board notes

### ArmSoM CM5-IO

- The eMMC is 29 GB.
- Windows sees the NVMe; the script leaves it alone.

### LCKFB TaishanPi 3M

- The eMMC is 62008590336 bytes; the WinPE image of release 0.1.1 and later
  accepts it.
- The firmware has to be on the eMMC. With the firmware on an SD card the
  script stops, because it then sees two SD/eMMC disks.
- Writing the firmware's whole image replaces LCKFB's stock system, which
  will not start again until LCKFB's firmware is written back.
- To update the firmware later, under an installed Windows, use "UEFI
  firmware, keep the partitions". The whole image would erase the partition
  table.
- With LCKFB's DSI screen attached, the firmware uses it instead of HDMI,
  and Windows shows its picture there in portrait (landscape with
  wddm-mali-g52). The 2026-11-13 installation was done over HDMI; to do the
  same, unplug the screen's ribbon with the power off.
- Keep a USB keyboard at hand for WinPE and the first setup.

If it does not boot: [troubleshooting](troubleshooting.md).
