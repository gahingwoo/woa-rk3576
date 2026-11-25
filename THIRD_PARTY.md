# Third-party code

English | [简体中文](THIRD_PARTY.zh-CN.md)

`build/` and most of `drivers/` come from
[worproject/Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers),
the Windows on Arm drivers for RK3588, imported at commit `e00e70d`
(2025-03-18). The import commit carries them unmodified; every RK3576 change
after it is a separate commit, so `git log -- <driver>` shows exactly what was
changed and why.

Each driver keeps its own licence and copyright lines.

| Path | Author | Licence |
|---|---|---|
| `drivers/gpio/rk3xgpio` | CoolStar | Apache-2.0 (`LICENSE.txt`) |
| `drivers/i2c/rk3xi2c` | CoolStar | Apache-2.0 (`LICENSE.txt`) |
| `drivers/dma/pl330dma` | CoolStar | Apache-2.0 (`LICENSE.txt`) |
| `drivers/audio/rk3xi2sbus` | CoolStar | Apache-2.0 (`LICENSE.txt`) |
| `drivers/audio/codecs/es8323` | CoolStar | Apache-2.0 (`LICENSE.txt`) |
| `drivers/audio/csaudiork3x` | Microsoft sample, CoolStar | No licence file in the source repository |
| `drivers/net/dwc_eqos` | Doug Cook | MIT upstream (`LICENSE`); distributed here as GPL-2.0-only (`LICENSING.md`) |
| `drivers/sd/dwcmshc` | Mario Bălănică, Microsoft sample | MIT upstream (SPDX headers); distributed here as GPL-2.0-only (`LICENSING.md`) |
| `drivers/sd/dwcsdhc` | Microsoft sdhc sample, Greg Garbern, Mario Bălănică | Copyright lines only; no licence statement. The upstream samples it derives from are MIT. |
| `drivers/sd/rk_sip_sdmmc_lib` | Mario Bălănică | MIT (SPDX headers) |
| `drivers/lib/arm_smc_lib` | ARM Limited, Microsoft | BSD (file headers) |
| `drivers/shared`, `drivers/include` | Mario Bălănică, Microsoft, NXP, DataCore | MIT / BSD-3-Clause (file headers) |

Not from worproject. The vendor code is dual licensed; the option used is named:

| Path | Author | Licence |
|---|---|---|
| `drivers/video/vdpu383/lib/vdpu383_cabac.c` | Rockchip Electronics (MPP) | Apache-2.0 OR MIT (file header) |
| `drivers/video/vdpu383/lib/vdpu383_h264.c`, register layouts | ported from Rockchip MPP, mpp/hal/rkdec | Apache-2.0 OR MIT, used as MIT; the file as a whole, with the changes made here, is GPL-2.0-only (file header) |
| `drivers/video/vdpu383/warmup.c`, warm-up data and sequence | Rockchip Electronics, vendor kernel mpp_hack_rk3576.c | GPL-2.0+ OR MIT, used as GPL-2.0; the file is GPL-2.0-only (file header) |
| `drivers/net/aic8800usb`: protocol values, message layouts, download sequence | AICSemi, RivieraWaves (Linux driver in radxa-pkg/aic8800) | GPL-2; no code copied; the files are GPL-2.0-only (`LICENSING.md`) |

Two entries have no explicit licence: `dwcsdhc` and `csaudiork3x`.

Code written for this repository is GPL-2.0-only (see [LICENSE](LICENSE)).
Where a ported driver's licence allows it, the driver is distributed here
under GPL-2.0-only: `dwc_eqos` and `dwcmshc`, both MIT upstream. Their
upstream notices are kept, and each has a `LICENSING.md` saying so.

The rest stay under their own licences, and changes made here to them are
made under that licence:

- **The Apache-2.0 drivers.** Apache-2.0 code cannot be combined with GPL-2.0
  code, so those drivers and every change to them stay Apache-2.0.
- **The shared libraries and headers** (`drivers/shared`, `drivers/include`,
  `drivers/lib`, `rk_sip_sdmmc_lib`). They stay permissive because the
  Apache-2.0 drivers build against them too.
- **`dwcsdhc` and `csaudiork3x`.** Their upstream licence is unclear, so they
  are left as they are.

Each driver is built as its own binary, so the repository only distributes
them side by side.

## Firmware not carried

`drivers/bluetooth/bcm4345bt` needs Broadcom's BCM4345C5 patchram, which is
not in this repository or in any package it builds. Ampak's build for the
AP6256 (armbian/firmware) has no stated licence; Raspberry Pi's BCM43456
build (RPi-Distro/bluez-firmware) is under Cypress's licence, which allows
redistribution in object code for use with Cypress parts. The driver's
`install-firmware.ps1` downloads either on the board, at a pinned commit,
and checks its SHA-256. Microsoft's SerialHCIBus sample (MS-PL) was read for
the BthX interface; no code from it is used, since MS-PL and GPL-2.0 cannot
be combined.

`drivers/net/aic8800usb` needs AICSemi's AIC8800D80 firmware and ROM
patches, which AICSemi publishes without a licence. They are not in this
repository or in any package it builds; the driver's `install-firmware.ps1`
downloads them on the board from radxa-pkg/aic8800 at a pinned commit and
checks each SHA-256.

## Not imported

- `drivers/storage_fix/*.sys` and `drivers/usb/usbehci_nointerlocked`: patched
  Microsoft system binaries (`storahci.sys`, `stornvme.sys`, `usbehci.sys`).
  Modified Windows binaries are not redistributable. Neither is needed here:
  NVMe works with the inbox `stornvme.sys`, and the firmware does not publish
  the EHCI controller. The `storage_fix` readme and registry file are kept for
  reference.
