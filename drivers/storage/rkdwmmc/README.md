# rkdwmmc — RK3576 SD card host (dw_mmc, Windows on ARM)

An `sdport.sys` miniport for the RK3576 **SD card** controller, which is a
Synopsys **dw_mmc** (DesignWare Mobile Storage Host) — device-tree
`rockchip,rk3576-dw-mshc` — published as `ACPI\RKCPFE2C`.

> **This is for the SD card slot, not the eMMC.** The eMMC is a separate,
> SDHCI-compatible **DWCMSHC** controller that should use the Windows *inbox*
> SD host driver. See [../../../docs/STORAGE.md](../../../docs/STORAGE.md) for the
> full storage strategy and the one-line ACPI `_CID` change the eMMC needs.

## Why a custom driver

dw_mmc is **not** SDHCI register-compatible, and Windows has no inbox driver for
it. The OS SD stack (`sdport.sys`) provides the SD protocol, PnP/power, and the
interrupt; this miniport implements the dw_mmc register operations it calls.

## Layering

| File | Confidence | Purpose |
|------|-----------|---------|
| [hw.c](hw.c) | **kernel-verified** | dw_mmc engine: reset, clock divider, bus width, command issue, FIFO PIO. Checked against `drivers/mmc/host/dw_mmc.c`. |
| [miniport.c](miniport.c) | **template — verify on build** | SDPORT_* callbacks mapping sdport ops onto hw.c. |
| [rkdwmmc_regs.h](rkdwmmc_regs.h) | kernel-verified | register map + bit defs |

## Design (v1)

- **PIO data path** (no IDMAC). Data moves through the dw_mmc FIFO data register
  (offset 0x100 or 0x200, chosen by `VERID`) on RXDR/TXDR/DATA_OVER interrupts.
  Internal-DMA (IDMAC descriptors) is a later optimization.
- **Card detect via GPIO.** ACPI routes CD through `GpioInt` on `\_SB.GPI0`, so
  the [GPIO driver](../../gpio/rk3576gpio) must be present. `GetCardDetectState`
  falls back to the controller CDETECT register.
- **Clock** assumes a fixed CIU input (`RKDWMMC_CIU_CLOCK_HZ`, 150 MHz, biased
  so the card is never overclocked) until the EDK2 port exports the real rate.

## Status / limitations — read before building

- **The dw_mmc hardware engine (`hw.c`) is verified against the kernel driver.**
- **The sdport integration (`miniport.c`) compiles clean against
  `<sdport.h>`** (WDK 10.0.26100) but has not run on silicon. Building it
  corrected the `GetSlotCount` and `Cleanup` signatures, `CrashdumpSupported`,
  and a wrong PIO event mapping: there is no `SDPORT_EVENT_BUFFER_WRITE_READY`;
  sdport names those events after the FIFO state (`BUFFER_EMPTY` = send more,
  `BUFFER_FULL` = data waiting), and the read path was reporting no event at
  all, so a PIO read would have stalled.
- **PIO only**, single outstanding request, no tuning/HS400, 4-bit max.
- **R2 response byte-ordering is still open** and is *not* a build question. A
  one-byte shift may be needed (the SD spec drops the CRC) and the RK3588
  reference cannot settle it, because that driver is SDHCI, whose RESPONSE
  register presents an already-shifted view that dw_mmc's RESP registers do not.
  Read a known CID/CSD on hardware and compare.
- **Builds for ARM64 in CI; not yet run on silicon.**

## Bring-up order

Needs the GPIO driver (card detect). The eMMC inbox path (storage doc) is the
higher-priority boot enabler; this driver brings up the removable SD slot.
