# Third-party code

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
| `drivers/net/dwc_eqos` | Doug Cook | MIT (`LICENSE`) |
| `drivers/sd/dwcmshc` | Mario Bălănică, Microsoft sample | MIT (SPDX headers) |
| `drivers/sd/dwcsdhc` | Microsoft sdhc sample, Greg Garbern, Mario Bălănică | Copyright lines only; no licence statement. The upstream samples it derives from are MIT. |
| `drivers/sd/rk_sip_sdmmc_lib` | Mario Bălănică | MIT (SPDX headers) |
| `drivers/lib/arm_smc_lib` | ARM Limited, Microsoft | BSD (file headers) |
| `drivers/shared`, `drivers/include` | Mario Bălănică, Microsoft, NXP, DataCore | MIT / BSD-3-Clause (file headers) |

Two entries have no explicit licence: `dwcsdhc` and `csaudiork3x`. Code
written for this repository is MIT (see [LICENSE](LICENSE)); the files above
stay under their own licences.

## Not imported

- `drivers/storage_fix/*.sys` and `drivers/usb/usbehci_nointerlocked`: patched
  Microsoft system binaries (`storahci.sys`, `stornvme.sys`, `usbehci.sys`).
  Modified Windows binaries are not redistributable. Neither is needed here:
  NVMe works with the inbox `stornvme.sys`, and the firmware does not publish
  the EHCI controller. The `storage_fix` readme and registry file are kept for
  reference.
