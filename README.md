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
| [GPIO](drivers/gpio/rk3576gpio) | `RKCP3002` | GpioClx miniport | builds for ARM64² · not run on silicon |
| [I²C](drivers/i2c/rk3xi2c) | `RKCP3001` | SpbCx (rk3x) | builds for ARM64² · not run on silicon |
| [SPI](drivers/spi/rk3xspi) | `RKCP3003`¹ | SpbCx (rk3066) | builds for ARM64² · not run on silicon |
| [SD card](drivers/storage/rkdwmmc) | `RKCPFE2C` | sdport (dw_mmc) | builds for ARM64² · not run on silicon |
| [Ethernet GMAC0](drivers/net/dwmac) | `RKCP6543` | NetAdapterCx (DWMAC-4.20a) | builds for ARM64² · not run on silicon |
| eMMC | `RKCP0D40` | **inbox** SDHCI | no driver — add `_CID PNP0D40`¹ ([storage](docs/STORAGE.md)) |
| USB (xHCI) | `PNP0D10` | **inbox** usbxhci | no driver — already enumerated |
| Display | — (no ACPI) | **inbox** BasicDisplay | UEFI GOP framebuffer ([display](docs/DISPLAY.md)) |
| Audio (SAI + ES8388) | — (no ACPI) | blocked | needs firmware SAI enablement; **USB Audio** works inbox meanwhile ([audio](docs/AUDIO.md)) |

¹ Paired with a small **EDK2/ACPI** change in the RK3576 firmware port (also
  part of this project) — **WIP**, tracked under [firmware changes](#firmware-acpi-changes-wip).
² Zero warnings at `/W4 /WX` against WDK 10.0.26100, checked by CI on every
  push, and the SoC register engines are verified against the mainline kernel
  drivers. Neither has run on hardware — a clean build is not a working driver.

With inbox display (GOP) + USB input + storage + the drivers above, the platform
has every piece needed to boot Windows to the desktop with networking.

## Build

These are **ARM64 kernel drivers**. An ARM64 Windows machine builds them
**natively** (Visual Studio 2022 ARM64 + WDK 10.0.26100); an x64 EWDK also works
under emulation. You only need a board to *run* them.

```cmd
msbuild drivers\gpio\rk3576gpio\rk3576gpio.vcxproj ^
    /p:Configuration=Release /p:Platform=ARM64 ^
    /p:WindowsTargetPlatformVersion=10.0.26100.0
```

CI builds all five for ARM64 on every push and **fails on any error** — no
graceful skip. Each job uploads the `.sys`/`.inf`/`.cat`/`.pdb` as an artifact.
See [.github/workflows/ci.yml](.github/workflows/ci.yml).

Full instructions, the project settings that are easy to get wrong,
test-signing and install: [docs/BUILDING.md](docs/BUILDING.md).

## Firmware (ACPI) changes

The RK3576 EDK2 firmware port is also part of this project, so these ACPI
changes are made there alongside the drivers.

Done:

- **ACPI built into the CM5-IO image** — `AcpiTables.inf` +
  `RK3576AcpiPlatformDxe.inf` are in the platform build, with
  `PcdConfigTableModeDefault = 0x3` so one image serves both FDT (Linux) and
  ACPI (Windows). See [docs/BRINGUP-PLAN.md](docs/BRINGUP-PLAN.md).
- **eMMC** — `Emmc.asl` carries `Name (_CID, "PNP0D40")`, so the inbox SDHCI
  driver binds (the device is SDHCI-compatible and exposes the SD clock `_DSM`).
- **SPI** — `Spi.asl` publishes `_HID "RKCP3003"` with `_CID "PRP0001"` kept for
  Linux.

Still open:

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
  BRINGUP-PLAN.md       staged plan to first boot + Windows build ceiling
  ARCHITECTURE.md       WOA driver model + bring-up plan
  BUILDING.md           toolchain, signing, install, debug
  STORAGE.md            eMMC (inbox) vs SD (custom)
  DISPLAY.md            inbox GOP vs custom VOP2 WDDM
  AUDIO.md              SAI + ES8388 status; USB Audio stopgap
.github/workflows/      CI (structure checks + ARM64 WDK build)
```

## Acknowledgements

Thanks to **[ArmSoM](https://www.armsom.org/)** for sponsoring the **CM5** module
and the **CM5-IO** carrier board that this project is developed on. The RK3576
bring-up work here — firmware, ACPI tables and these drivers — is done against
that hardware.

Thanks also to the [worproject Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers)
project (Mario Bălănică, MIT). Its RK3588 drivers run on real silicon and were
the authority for every framework and ABI question here, from the SiP SD/MMC SMC
interface to the GMAC TX-clock `_DSM` contract.

## License

MIT — see [LICENSE](LICENSE).
