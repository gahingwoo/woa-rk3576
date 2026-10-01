# Installing Windows on ARM on the CM5-IO (RK3576)

Status: **the procedure below has not been run end to end yet.** Everything it
depends on has been seen working on hardware on 2026-10-01: the WinPE this repo
builds boots, all of this repo's drivers start in it, and the eMMC is a 29 GB
disk there.

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

Remove the stick, reboot. The firmware keeps UEFI variables on the eMMC, so
`bcdboot`'s "Windows Boot Manager" entry survives; `\EFI\Boot\bootaa64.efi` is
there as well if it does not.
