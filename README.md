# woa-rk3576

[![CI](https://github.com/gahingwoo/woa-rk3576/actions/workflows/ci.yml/badge.svg)](https://github.com/gahingwoo/woa-rk3576/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Target](https://img.shields.io/badge/target-Windows%20on%20ARM64-blue)]()
[![SoC](https://img.shields.io/badge/SoC-RK3576-green)]()

Windows-on-ARM (WOA) kernel drivers for Rockchip **RK3576** boards — Radxa
ROCK 4D, ArmSoM CM5-IO and the other CM5 carriers.

Windows on ARM discovers hardware through **ACPI**, not Device Tree. The boot
firmware and ACPI tables live in the separate EDK2 RK3576 port; **this repo is
the Windows `.sys`/`.inf` driver packages** for the SoC peripherals Windows has
no inbox driver for, plus docs for the peripherals that *do* use inbox drivers.

## Status

| Peripheral | Bind (`_HID`) | Approach | State |
|---|---|---|---|
| [GPIO](drivers/gpio/rk3576gpio) | `RKCP3002` | GpioClx miniport | source complete · verified HW layer |
| [I²C](drivers/i2c/rk3xi2c) | `RKCP3001` | SpbCx (rk3x) | source complete · verified HW layer |
| [SPI](drivers/spi/rk3xspi) | `RKCP3003`¹ | SpbCx (rk3066) | source complete · verified HW layer |
| [SD card](drivers/storage/rkdwmmc) | `RKCPFE2C` | sdport (dw_mmc) | HW layer verified · framework glue is a template² |
| [Ethernet GMAC0](drivers/net/dwmac) | `RKCP6543` | NetAdapterCx (DWMAC-4.20a) | HW layer verified · framework glue is a template² |
| eMMC | `RKCP0D40` | **inbox** SDHCI | no driver — add `_CID PNP0D40`¹ ([storage](docs/STORAGE.md)) |
| USB (xHCI) | `PNP0D10` | **inbox** usbxhci | no driver — already enumerated |
| Display | — (no ACPI) | **inbox** BasicDisplay | UEFI GOP framebuffer ([display](docs/DISPLAY.md)) |
| Audio (SAI + ES8388) | — (no ACPI) | blocked | needs firmware SAI enablement; **USB Audio** works inbox meanwhile ([audio](docs/AUDIO.md)) |

¹ Paired with a small **EDK2/ACPI** change in the RK3576 firmware port (also
  part of this project) — **WIP**, tracked under [firmware changes](#firmware-acpi-changes-wip).
² The SoC register engine is verified against the mainline kernel driver;
  aligning the Windows class-extension glue (sdport / NetAdapterCx) to the WDK
  ABI is **in progress** (the spots are marked `VERIFY-ON-BUILD` in the source).

With inbox display (GOP) + USB input + storage + the drivers above, the platform
has every piece needed to boot Windows to the desktop with networking.

## Build

These are **ARM64 kernel drivers**. An ARM64 Windows machine builds them
**natively** (Visual Studio 2022 ARM64 + WDK 26100); an x64 EWDK also works under
emulation. You only need a board to *run* them. Full instructions, test-signing
and install: [docs/BUILDING.md](docs/BUILDING.md).

```cmd
cd drivers\gpio\rk3576gpio
build.cmd Release
```

CI builds every driver for ARM64 on each push — see
[.github/workflows/ci.yml](.github/workflows/ci.yml).

## Firmware (ACPI) changes (WIP)

The RK3576 EDK2 firmware port is also part of this project, so these ACPI
changes are being made there alongside the drivers (work in progress):

- **eMMC** — add `Name (_CID, "PNP0D40")` to `Emmc.asl` so the inbox SDHCI driver
  binds (the device is SDHCI-compatible and already exposes the SD clock `_DSM`).
- **SPI** — give the SPI controllers a Windows `_HID` (e.g. `RKCP3003`) instead
  of the Linux-only `PRP0001` in `Spi.asl`.
- **Audio (SAI)** — bigger task: enumerate the RK3576 **SAI** block (not the old
  I²S) with correct addresses/clocks/DMA, under a distinct `_HID` (the stale
  `I2s.asl` wrongly reuses `RKCP3003`). Solution being worked out — see
  [docs/AUDIO.md](docs/AUDIO.md).

## Layout

```
drivers/
  inc/                  shared SoC constants
  gpio/rk3576gpio/      GPIO controller         (GpioClx)
  i2c/rk3xi2c/          rk3x I²C controller     (SpbCx)
  spi/rk3xspi/          rk3066 SPI controller   (SpbCx)
  storage/rkdwmmc/      SD card host (dw_mmc)   (sdport miniport)
  net/dwmac/            GMAC Ethernet (DWMAC)   (NetAdapterCx)
docs/
  ARCHITECTURE.md       WOA driver model + bring-up plan
  BUILDING.md           toolchain, signing, install, debug
  STORAGE.md            eMMC (inbox) vs SD (custom)
  DISPLAY.md            inbox GOP vs custom VOP2 WDDM
  AUDIO.md              SAI + ES8388 status; USB Audio stopgap
.github/workflows/      CI (structure checks + ARM64 WDK build)
```

## License

MIT — see [LICENSE](LICENSE).
