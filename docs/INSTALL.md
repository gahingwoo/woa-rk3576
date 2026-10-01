# Installing Windows on ARM on the CM5-IO (RK3576)

Status: **done on hardware, 2026-10-01.** Windows 11 23H2 Enterprise (build
22631.2428) installed this way boots to the desktop from the CM5-IO's eMMC.
The first attempt did not; what it took is under
[What it took to boot](#what-it-took-to-boot). The scripts now cover all of it,
but that installation was repaired in place: a fresh deployment with the
current scripts has not been run yet.

## Which Windows

RK3576 is 4× Cortex-A72 + 4× Cortex-A53, **ARMv8.0-A**. Windows 11 24H2 and
later use ARMv8.1 LSE atomics throughout the kernel and cannot run on it; there
is no firmware workaround, because an undefined instruction taken at EL1 never
reaches EL3.

| Target | Build | Verdict |
|---|---|---|
| Windows 10 ARM64 22H2 | 19045 | works |
| Windows 11 23H2 | 22631 | works; the target here |
| Windows 11 24H2+ / LTSC 2024 | 26100+ | cannot boot |

Check an image before spending time on it. `dism /Get-ImageInfo` shows the
version, and the deploy script refuses anything newer than build 25999.

## Why not Windows Setup

Windows 11 23H2 Setup refuses this board three ways: the eMMC is 29 GB against a
64 GB minimum, and there is no TPM 2.0 and no Secure Boot. It would also leave
the installed system without testsigning, which it needs from the first boot:
Windows lives on the eMMC, so the eMMC driver (`dwcsdhc`, test-signed) is
boot-critical.

So the image is applied with DISM from this repo's WinPE instead, which already
runs every driver here and sees the eMMC.

## Where things live

- **Firmware: eMMC partition 1**, sector 64, 64 MiB (`CM5IO-emmc.img` from the
  firmware releases). The BootROM tries the eMMC first. Never touched below.
- **Windows: the rest of the eMMC**, about 29 GB, applied compact.
- **Fedora: the NVMe**, untouched.

## Procedure

### 1. The stick

Write the WinPE image from this repo's `winpe` workflow to a USB stick. It has
one 515 MB FAT32 partition, `WINPE`, holding `woa-debug\` (the collector),
`woa-deploy\deploy-windows.cmd` and `woa-drivers\` (the test-signed driver
packages), and leaves the rest of the stick unallocated.

In that unallocated space create one **exFAT or NTFS** partition and copy
`sources\install.wim` (or `install.esd`) from the Windows 11 23H2 ARM64 ISO onto
it. FAT32 will not do: the image is larger than 4 GB.

### 2. Boot WinPE

**Take the SD card out of the slot.** The script stops if it sees more than one
SD/eMMC disk, so it cannot pick the wrong one.

Boot the stick from the firmware's boot menu. The collector runs on its own;
when it finishes, the prompt says how to start the installer.

### 3. Run the installer

```
C:\woa-deploy\deploy-windows.cmd
```

(the drive letter of the stick may differ; the collector prints it). It:

1. finds the eMMC (the one disk whose PNP id starts `SD\`, 28-33 GB) and checks
   that its first partition is the firmware region: offset 32768, length
   67076096. Anything else, and it stops without writing;
2. finds `install.wim`/`.esd` on any volume, lists the editions and asks which
   index; refuses non-ARM64 images and builds newer than 25999;
3. asks you to type `ERASE`, then deletes every partition after the first and
   creates ESP (260 MB), MSR and an NTFS Windows partition;
4. applies the image with `/Compact`, injects `woa-drivers\` with
   `dism /Add-Driver`, runs `bcdboot`, turns testsigning on in the new BCD, and
   sets `BypassNRO` so OOBE can finish without a network.

Everything goes to `woa-deploy\deploy.log` on the stick.

### 4. First boot

Remove the stick and the SD card, reboot. The firmware tries the SD slot and
USB before the eMMC, then boots `\EFI\Boot\bootaa64.efi` from the eMMC's ESP.
`bcdboot` did not leave a "Windows Boot Manager" entry the firmware can see, so
that fallback path is what boots it.

The first boot runs Windows' specialize pass and then OOBE, which can finish
offline. The desktop shows "Test Mode": the drivers are test-signed.

## What it took to boot

The first deployment ended in `INACCESSIBLE_BOOT_DEVICE` (0x7B, second
parameter `0xC0000034`: the boot device's name did not exist). Three separate
causes, each found with the kernel debugger on the serial port and by reading
the installed system's registry from Linux:

1. **`dwcsdhc` was demand-start.** Right for a data disk, and why it worked in
   WinPE; but DISM only *installs* a driver offline (binary, service, device
   binding) when the INF makes it boot-start, and only *stages* it otherwise.
   Its INF is boot-start now.
2. **`sdstor` was demand-start.** It is the inbox driver for the disk on the
   eMMC; Setup would have made it boot-start because the system disk sits on
   it, DISM does not. `deploy-windows.cmd` sets it.
3. **`sdbus` took the eMMC controller and kept it.** The firmware publishes the
   eMMC as `_CID PNP0D40`, sdbus matches that, and sdbus is loaded on SD-disk
   boots. On the first boot, while `dwcsdhc` was still demand-start, PnP bound
   the controller to sdbus and recorded it in `Enum\ACPI\RKCP0D40`; every later
   boot reused that binding, and sdbus cannot drive this controller. Deleting
   the instance and disabling sdbus fixed it. `deploy-windows.cmd` now disables
   sdbus up front; nothing on this board needs it.

Those crashed boots also interrupted the specialize pass, and the next boot
said *"The computer restarted unexpectedly or encountered an unexpected
error"*. The standard way past it: Shift+F10, `regedit`,
`HKLM\SYSTEM\Setup\Status\ChildCompletion`, set `setup.exe` to 3, OK. A
deployment that boots first time does not see this.

`tools/woa-deploy/add-drivers.cmd` re-injects the drivers into an existing
installation from WinPE without erasing it.
