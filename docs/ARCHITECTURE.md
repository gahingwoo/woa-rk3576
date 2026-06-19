# Windows-on-ARM driver architecture — RK3576

This repository holds the **Windows kernel drivers** for running Windows on ARM
(WOA) on Rockchip RK3576 boards (Radxa ROCK 4D, ArmSoM CM5-IO, and the other
CM5 carriers). The boot firmware (TF-A BL31 + OP-TEE BL32 + U-Boot SPL + EDK2
UEFI) and the ACPI tables live in the separate EDK2 port; this repo is only the
`.sys`/`.inf` driver packages.

## How WOA sees the hardware

Windows on ARM **does not use Device Tree** — it discovers hardware exclusively
through **ACPI**. The EDK2 RK3576 port already ships a fairly complete static
ACPI table set (`Silicon/Rockchip/RK3576/AcpiTables/`): FADT, MADT (GICv3),
GTDT, DBG2, SPCR, PPTT, plus a DSDT describing eMMC/SDHC, GPIO, I²C, SPI, USB,
PCIe, GMAC and DMA. Each peripheral is published under a Rockchip-specific ACPI
`_HID` (e.g. GPIO = `RKCP3002`).

For each ACPI device, Windows needs **either**:

1. an **inbox** driver (GIC, arch timer, PSCI, xHCI, PCIe, 16550 serial), driven
   purely by the ACPI description — no code in this repo; **or**
2. a **custom driver** in this repo, for IP blocks Windows has no inbox support
   for (Rockchip GPIO, Rockchip rk3x I²C, Rockchip SPI, DWCMSHC/dw-mshc
   eMMC/SD, Synopsys DWMAC ethernet, VOP2/HDMI display, audio).

```
   ACPI (in EDK2)                 Windows driver stack
   ┌────────────────┐            ┌───────────────────────────┐
   │ DSDT: RKCP3002 │ ─enumerate→ │ rk3576gpio.sys (GpioClx)   │  ← this repo
   │ DSDT: I2C ...  │ ─enumerate→ │ rk3576i2c.sys  (SpbCx)     │  ← planned
   │ DSDT: SDHC ... │ ─enumerate→ │ sdport + miniport          │  ← planned
   │ MADT/GTDT/xHCI │ ─enumerate→ │ inbox (HAL / usbxhci)      │
   └────────────────┘            └───────────────────────────┘
```

## Bring-up order

Dependency-driven. Each stage unblocks the next.

| # | Driver | ACPI `_HID` | Framework | Why this order |
|---|--------|-------------|-----------|----------------|
| 0 | Boot at all | MADT/GTDT/FADT | inbox (HAL) | GIC + timer + PSCI from ACPI |
| 0 | Serial debug | DBG2/SPCR | inbox 16550 | kernel debug / KDNET-less bring-up |
| 0 | Display | UEFI GOP | inbox BasicDisplay | framebuffer from firmware until a real VOP2 driver |
| **1** | **GPIO** | **RKCP3002** | **GpioClx** | **foundation: card-detect, IRQs, regulators, buttons** |
| 2 | I²C | `RKCP3001` | SpbCx | Rockchip rk3x IP (not DesignWare); PMIC RK806, RTC, EEPROM |
| 3a | eMMC | `RKCP0D40` | **inbox** SDHCI | DWCMSHC is SDHCI-compatible — use inbox via ACPI `_CID PNP0D40`; the boot/install volume |
| 3b | SD card | `RKCPFE2C` | sdport miniport | dw_mmc (not SDHCI) — custom driver; removable slot |
| 4 | USB host | `PNP0D10` | **inbox** usbxhci | DWC3 in host mode; ACPI already uses PNP0D10 → no custom driver |
| 5 | Ethernet | `RKCP6543` (DWMAC-4.20a) | NetAdapterCx | networking; real Synopsys DWMAC |
| 6 | SPI | `RKCP3003`* | SpbCx | Rockchip rk3066-family IP (not DW); *needs ACPI `_HID` (now PRP0001) |
| 7 | Display | — (no ACPI) | **inbox** BasicDisplay | UEFI GOP framebuffer; custom VOP2 WDDM is a later project |
| 8 | Audio (SAI + ES8388) | — | PortCls/WaveRT | blocked on firmware SAI enablement; USB Audio works inbox ([AUDIO.md](AUDIO.md)) |
| 9 | Thermal, PWM, … | various | | polish |

Done: **GPIO** ([rk3576gpio](../drivers/gpio/rk3576gpio)), **I²C**
([rk3xi2c](../drivers/i2c/rk3xi2c)), **storage** ([STORAGE.md](STORAGE.md): eMMC
inbox, SD [rkdwmmc](../drivers/storage/rkdwmmc)), **SPI**
([rk3xspi](../drivers/spi/rk3xspi)), **GMAC**
([dwmac](../drivers/net/dwmac)). **USB** = inbox xHCI (PNP0D10); **display** =
inbox BasicDisplay over UEFI GOP ([DISPLAY.md](DISPLAY.md)). All core bring-up
peripherals are now covered; remaining work is hardware validation + a real VOP2
WDDM driver and datapath/offload polish.

## Repository layout

```
drivers/
  inc/                 shared SoC constants (rk3576_soc.h)
  gpio/rk3576gpio/      GPIO controller miniport (GpioClx)
  i2c/rk3xi2c/          Rockchip rk3x I2C controller (SpbCx)
  storage/rkdwmmc/      SD card host (dw_mmc) sdport miniport
  spi/rk3xspi/          Rockchip SPI controller (SpbCx)
  net/dwmac/            GMAC Ethernet (DWMAC-4.20a) NetAdapterCx  ← current
docs/
  STORAGE.md            eMMC (inbox) vs SD (custom) strategy
  DISPLAY.md            inbox GOP framebuffer vs custom VOP2 WDDM
  AUDIO.md              SAI + ES8388 status; USB Audio stopgap
docs/
  ARCHITECTURE.md       this file
  BUILDING.md           EWDK build + test-signing + install
```

## Conventions

- All drivers target **ARM64** only.
- Drivers obtain MMIO/interrupt resources from ACPI `_CRS` at runtime; SoC
  addresses in `drivers/inc/rk3576_soc.h` are reference/sanity data, never the
  source of truth in device logic.
- Hardware facts (register offsets, IRQs, clocks) are cross-checked against the
  RK3576 TRM and the mainline `rk3576.dtsi`, and cited in the source.
