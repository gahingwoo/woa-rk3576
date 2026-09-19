# Storage bring-up — RK3576 (eMMC + SD)

RK3576 exposes three MSHC-family storage controllers, but they are **two
different IP blocks** with very different Windows stories. Getting this right
matters: one needs *no* driver from us, the other needs a custom one.

| Controller | Base | ACPI `_HID` | IP | dtsi compatible |
|-----------|------|-------------|----|-----------------|
| eMMC (SDC3) | 0x2A330000 | `RKCP0D40` | **DWCMSHC** (SDHCI-compatible) | `rockchip,rk3576-dwcmshc` |
| SD card (SDHC) | 0x2A310000 | `RKCPFE2C` | **dw_mmc** (DesignWare MMC, *not* SDHCI) | `rockchip,rk3576-dw-mshc` |
| SDIO | 0x2A320000 | — | dw_mmc | `rockchip,rk3576-dw-mshc` |

## eMMC — the inbox driver binds and cannot work (measured 2026-09-19)

The eMMC controller is a **DWCMSHC**, register-compatible with the SD Host
Controller Standard. Windows does bind its inbox driver to it, and it starts:

    ACPI\RKCP0D40\3   SDA Standard Compliant SD Host Controller
                       Status: Started    Driver: sdbus.inf
                       Resources: mem 0x2A330000 len 0x10000, interrupt 285

`_CID "PNP0D40"` is present in `Emmc.asl` and doing its job. **This section
used to say adding it was the one remaining gap and was WIP. It is done, and it
was not enough.**

**No card ever appears under it.** There is no `SD\`, `MMC\` or `SFFDISK\`
device node, and `ACPI\RKCP0D40\3` has no child at all. Confirmed on a clean
WinPE 22621 built from the ADK with no third-party drivers, whose image
contains the whole stack — `sdbus.sys`, `sdport.sys`, `sdstor.sys`,
`sdstor.inf` are all in `boot.wim`. `sdstor` is absent from the running
registry only because nothing exists for it to bind to.

### Why

Three Rockchip vendor bits hold the controller usable, and **an SDHCI
`SW_RST_ALL` clears all three**:

| Register | Bit | Meaning if cleared |
|---|---|---|
| `EMMC_CTRL` @ 0x52C | 0 `CARD_IS_EMMC` | controller not in eMMC mode |
| `EMMC_CTRL` @ 0x52C | 2 `EMMC_RST_N` | **the card is held in hardware reset** |
| `EMMC_MISC_CON` @ 0x81C | 1 `MISC_INTCLK_EN` | **internal clock off — every command times out** |

(The vendor area base is the u16 at 0xE8 masked with 0xFFF; it is 0x500 here.)

Two independent records of that, neither of them a guess: mainline's
`rk35xx_sdhci_reset()` in `sdhci-of-dwcmshc.c` writes `MISC_INTCLK_EN` back
after every `sdhci_reset()`, and this project's own `DwcSdhciDxe.c` does the
same in `EdkiiSdMmcResetPost`, written from a measured failure on this board.

A standard SDHCI driver resets the controller when it starts. Microsoft's
cannot know about three Rockchip vendor bits, so nothing restores them.

**No ACPI change can fix this** — the reset happens after ExitBootServices,
where firmware has no say. `_DSD`, `_DSM`, `_RMV` and the `_CID` binding are
all irrelevant to it.

### Ruled out by measurement, do not revisit

- *Base clock broken.* `CAPS0 = 0x3A6DC881`, bits[15:8] = `0xC8` = 200 MHz.
  (mainline sets `SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN` on every dwcmshc variant,
  which is what suggested this; it is conservative here.)
- *No card detect.* `PRESENT_STATE = 0x03F700F0` — CardInserted=1, from the
  real pin (`HOST_CONTROL1 = 0x34`, CD_SigSel=0).
- Also read: `HOST_VERSION 0x0005` (SDHCI 4.20), `POWER_CONTROL 0x0D` (on,
  3.0V), `HOST_CONTROL2 0x380F` (1.8V signalling, HS400).

Those first two were read from a running Linux, which had already fixed
everything up. **Windows sees the post-reset state, which is a different
machine** — a register read from a working OS tells you what that OS made of
the hardware, not what another OS finds.

Still not measured: that Windows actually issues `SRST_ALL`. It is what a
standard SDHCI driver does at start, but it was not observed. Reading
`0x2A330000 + 0x52C` and `+ 0x81C` from inside Windows would settle it.

### So: a DWCMSHC miniport

The fallback this document said it would not write pre-emptively is now the
plan. It needs the standard SDHCI logic plus the Rockchip vendor fixups
re-applied after every reset — the three bits above, and the DLL block at
0x800 for the higher speeds.

## SD card — custom dw_mmc driver (this repo)

The SD card slot is a **dw_mmc** controller. This is the *older* Synopsys
DesignWare Mobile Storage Host IP and is **not** SDHCI register-compatible —
there is **no Windows inbox driver** for it. It needs a custom `sdport.sys`
miniport: [../drivers/storage/rkdwmmc](../drivers/storage/rkdwmmc).

Notable wiring from `Sdhc.asl`:
- MMIO 0x2A310000, GIC SPI 283 (GSIV 283).
- **Card detect via GPIO**, not the controller's CDETECT register:
  `GpioInt (Edge, ActiveBoth, ... "\\_SB.GPI0") { GPIO_PIN_PA7 }`. This is why
  the GPIO driver ([rk3576gpio](../drivers/gpio/rk3576gpio)) must come first.
- 4-bit bus, SDR/DDR50/SDR104 capable per `_DSD`.

## Boot implications

For installing/booting Windows, **eMMC is still the primary target** — 29 GiB,
non-removable, on the CM5 module, and it leaves the NVMe to Fedora, which makes
dual boot and rescue straightforward. But it needs a **driver**, not an ACPI
change; see above. That is the highest-leverage storage action.

The custom **dw_mmc** driver here enables the **removable SD card**: useful for
installation media, as a secondary volume, and as a rescue path.

Both drivers have to load in **WinPE** as well as in the installed system, or
Setup cannot see the disk it is installing to. That means injecting them into
`boot.wim` and enabling test signing on the media's BCD until they are signed.
