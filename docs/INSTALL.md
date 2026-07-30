# Installing Windows on ARM on the CM5-IO (RK3576)

Status: **not yet completed on hardware.** EDK2 boots, ACPI installs correctly
(verified with `acpiview` on the board), and all five drivers build — but no
Windows install has run yet. This document is the procedure being followed, with
the two constraints that shape it.

## Constraint 1: the build you install matters

RK3576 is 4× Cortex-A72 + 4× Cortex-A53 = **ARMv8.0-A**. Windows 11 24H2 and
later are compiled with ARMv8.1 **LSE atomic** instructions (`CAS`, `LDADD`, …)
inlined throughout the kernel and drivers. ARMv8.0 does not implement them, and
there is no firmware-side fix: an undefined-instruction exception taken in EL1
goes to EL1's own vector table — the Windows kernel's — so EL3/TF-A never sees it
and cannot emulate.

| Target | Build | Verdict |
|---|---|---|
| Windows 10 ARM64 22H2 | 19045 | works; consumer support ended 2025-10-14 |
| Windows 11 23H2 | 22631 | works; Enterprise/Education serviced to 2026-11-10 |
| Windows 11 24H2+ / **LTSC 2024** | 26100 | **expected not to boot** — ARMv8.1 required |

This is the same ceiling the Raspberry Pi 4 has, for the same reason (also
Cortex-A72). RPi5 (Cortex-A76, ARMv8.2) is unaffected — as is RK3588
(Cortex-A76/A55), which is why the RK3588 Windows port does not hit this.

Check what an ISO actually is before spending time on it — the media label lies,
the WIM metadata does not:

```bash
python3 - /path/to/iso/sources/install.wim <<'PY'
import struct, sys, re
with open(sys.argv[1], "rb") as f:
    hdr = f.read(0xD0)
    size, off, _ = struct.unpack_from("<QqQ", hdr, 0x48)   # rhXmlData
    f.seek(off); xml = f.read(size & 0x00FFFFFFFFFFFFFF).decode("utf-16-le", "replace")
for t in ("BUILD", "ARCH", "DISPLAYNAME"):
    print(t, sorted(set(re.findall(rf"<{t}>(.*?)</{t}>", xml))))
PY
```

`ARCH 12` is ARM64. `BUILD 26100` is 24H2 — including "LTSC 2024", whose
long support window does not help here.

## Constraint 2: UEFI variables do not persist

The CM5-IO carrier's SPI NOR is only 64 KB — too small for the UEFI image, so the
firmware boots from SD/eMMC, and the variable store has nowhere durable to live:

```
FvbFindBootDiskDevice: WARNING: Variable store changes will NOT persist!
```

Two things cause it, both in the EDK2 port: `PcdFitImageFlashAddress` defaults to
`0` and no platform overrides it, so `RkFvbDxe` looks for the FIT at disk offset 0
and gets `FDT_ERR_BADMAGIC`; and the SD/eMMC FIT payload is the bare
`BL33_AP_UEFI.Fv` with no NV region appended (the SPI layout has one at
`0xFC0000`). Fixing that properly is tracked separately.

Meanwhile the install works **without** NVRAM, because UEFI falls back to a fixed
path on any ESP:

- Windows ISOs already ship `\efi\boot\bootaa64.efi`, so **Setup boots from USB
  with no NVRAM entry needed**.
- The **installed** system does not: Setup writes its boot entry to NVRAM only.
  So after the file-copy phase, `bootmgfw.efi` has to be copied to the fallback
  path on the internal ESP — see step 4.

## Where firmware and OS live

Keep them on **different** devices:

- **Firmware on SD.** The board boots this today (`Trying to boot from MMC2`).
- **Windows on eMMC.** Setup will lay down its own GPT starting at sector 2048,
  which overwrites anything at sector 64 — so do not leave firmware you care
  about on the eMMC. That is fine: with no valid `RKNS` header on eMMC the
  BootROM falls through to the SD card.

Note the BootROM prefers eMMC over SD, so a bootable eMMC image wins. If SD
firmware is being ignored, that is why.

## Procedure

### 1. Build the install stick

```bash
sudo bash tools/make-woa-usb.sh /dev/sdX /path/to/mounted/iso
```

The script refuses anything that is not a removable USB disk, requires typing
`ERASE`, then makes one FAT32 ESP spanning the stick, copies the ISO, and splits
`install.wim` only if it exceeds FAT32's 4 GiB per-file limit.

### 2. Boot Setup

Plug the stick in, power on, and pick it from the EDK2 boot menu. This needs only
inbox drivers — GOP display, xHCI for keyboard/mouse.

### 3. Install to eMMC

The eMMC is DWCMSHC and the firmware's `Emmc.asl` carries `_CID "PNP0D40"`, so the
**inbox SDHCI driver** binds and Setup sees the disk with no driver injection.
Let Setup partition the whole eMMC.

### 4. Make the installed system bootable without NVRAM

Do this **before** the first boot of the installed OS. At Setup's reboot, enter
the UEFI Shell and copy the boot manager to the fallback path on the eMMC ESP:

```
Shell> map -r
Shell> FS0:                       # the eMMC ESP
FS0:\> cp EFI\Microsoft\Boot\bootmgfw.efi EFI\BOOT\BOOTAA64.EFI
```

Adjust `FS0:` to whichever mapping is the eMMC ESP (`map -r` lists them; the ESP
has `\EFI\Microsoft\Boot`). From then on the firmware finds Windows on every cold
boot with no NVRAM involved.

### 5. Load the RK3576 drivers

Enable test-signing first, then install in dependency order — GPIO → I²C → SD →
GMAC → SPI. See [BUILDING.md](BUILDING.md) for signing and `pnputil`, and
[BRINGUP-PLAN.md](BRINGUP-PLAN.md) for what each stage proves.

Keep a serial console attached throughout: **UART0 @ `0x2AD40000`, 1500000 8N1**.
