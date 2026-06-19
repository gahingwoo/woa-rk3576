# Storage bring-up — RK3576 (eMMC + SD)

RK3576 exposes three MSHC-family storage controllers, but they are **two
different IP blocks** with very different Windows stories. Getting this right
matters: one needs *no* driver from us, the other needs a custom one.

| Controller | Base | ACPI `_HID` | IP | dtsi compatible |
|-----------|------|-------------|----|-----------------|
| eMMC (SDC3) | 0x2A330000 | `RKCP0D40` | **DWCMSHC** (SDHCI-compatible) | `rockchip,rk3576-dwcmshc` |
| SD card (SDHC) | 0x2A310000 | `RKCPFE2C` | **dw_mmc** (DesignWare MMC, *not* SDHCI) | `rockchip,rk3576-dw-mshc` |
| SDIO | 0x2A320000 | — | dw_mmc | `rockchip,rk3576-dw-mshc` |

## eMMC — use the Windows inbox SDHCI driver (no custom driver)

The eMMC controller is a **DWCMSHC**, which is register-compatible with the
**SD Host Controller Standard (SDHCI 3.0/4.0)**. Windows ships an inbox standard
SD host controller driver (`sdport.sys` + the standard host miniport) that binds
to **`ACPI\PNP0D40`** ("SDA Standard Compliant SD Host Controller").

The EDK2 ACPI for this device is already built for that path — note the
`_DSM` with GUID `434addb0-8ff3-49d5-a724-95844b79ad1f` in `Emmc.asl`. That is
the **Microsoft-defined SD/eMMC clock-control `_DSM`**: the inbox driver invokes
it to change the card clock, and the firmware reprograms the RK3576 CRU
(`CCLK_SRC_EMMC`) behind it. This is exactly how a vendor SoC reuses the inbox
SDHCI stack without a custom driver.

**The one gap:** the device is published as `_HID "RKCP0D40"`, which Windows
does *not* recognize. The inbox driver matches `PNP0D40`. So the eMMC needs a
compatible id added in the EDK2 DSDT:

```asl
Device (SDC3) {
    Name (_HID, "RKCP0D40")
    Name (_CID, "PNP0D40")        // <-- add this so the inbox driver binds
    ...
}
```

That is a one-line change in the EDK2 firmware port (also part of this project)
and is **WIP** there. With that `_CID`
present, Windows binds its inbox SDHCI driver and the eMMC works at the speeds
the `_DSM` clock table supports (24 MHz → 200 MHz). HS400/enhanced-strobe tuning
may need additional `_DSM` work, but legacy/HS/HS200 should come up.

> If, on hardware, the inbox driver cannot drive the DWCMSHC (vendor DLL/PHY
> quirks at higher speeds), the fallback is a custom DWCMSHC `sdport` miniport
> that reuses most of the standard SDHCI logic plus the Rockchip DLL block at
> offset 0x500. We do not write that pre-emptively.

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

For installing/booting Windows, **eMMC is the primary target** (non-removable,
on the CM5 module) and is best served by the inbox path above — so the highest
-leverage storage action is the eMMC `_CID` ACPI change, not a driver. The
custom **dw_mmc** driver here enables the **removable SD card**, which is useful
for installation media and as a secondary volume.
