# woa-rk3576

[![CI](https://github.com/gahingwoo/woa-rk3576/actions/workflows/ci.yml/badge.svg)](https://github.com/gahingwoo/woa-rk3576/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Target](https://img.shields.io/badge/target-Windows%20on%20ARM64-blue)]()
[![SoC](https://img.shields.io/badge/SoC-RK3576-green)]()

Windows-on-ARM (WOA) kernel drivers for Rockchip **RK3576** boards — Radxa
ROCK 4D, ArmSoM CM5-IO and the other CM5 carriers.

Windows on ARM discovers hardware through **ACPI**, not Device Tree. The boot
firmware and ACPI tables live in the separate
[EDK2 RK3576 port](https://github.com/gahingwoo/edk2-rk3576); **this repo is
the Windows `.sys`/`.inf` driver packages** for the SoC peripherals Windows has
no inbox driver for, plus docs for the peripherals that *do* use inbox drivers.

## Windows 11 on the CM5-IO

![Windows 11 23H2 on the ArmSoM CM5-IO, booted from its eMMC](docs/imgs/cm5io-win11-desktop.jpg)

**2026-10-01: Windows 11 23H2 Enterprise (build 22631.2428) boots to the
desktop from the CM5-IO's eMMC.** Task Manager reports a Rockchip RK3576 with
all eight cores, 3.7 GB of memory, the eMMC as C: (type SD), the NVMe, the SD
card as a removable disk, and the Ethernet adapter. Test Mode, because the
drivers are test-signed.

It was installed with [`tools/woa-deploy`](tools/woa-deploy) from this repo's
WinPE, not with Windows Setup, which refuses this board. The procedure and what
it took to make it boot are in [docs/INSTALL.md](docs/INSTALL.md).

## Where the drivers come from

The drivers are ported from
[worproject/Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers),
the RK3588 set that runs on real silicon. Every one of them binds the same
ACPI `_HID` the RK3576 firmware publishes for the same IP block. They were
imported unmodified at `e00e70d`, and each RK3576 change is a separate commit
on top, so `git log -- drivers/<class>/<name>` shows exactly what differs from
RK3588 and why. Provenance and licences per driver:
[THIRD_PARTY.md](THIRD_PARTY.md).

The drivers this repo had before were written from scratch. They were
replaced on 2026-10-01; they are in git history.

## Status

| Peripheral | `_HID` | Driver | RK3576 changes | State |
|---|---|---|---|---|
| eMMC | `RKCP0D40` | [dwcsdhc](drivers/sd/dwcsdhc) (sdport) | card clock from the CRU instead of a BL31 SiP call; vendor bits restored after reset | **Windows 11 boots from it**; boot-start, see [INSTALL](docs/INSTALL.md) |
| SD card | `RKCPFE2C` | [dwcmshc](drivers/sd/dwcmshc) (sdport) | clock from the CRU, phases in the controller's `TIMING_CON`, 3.3 V only | **The card is a disk** in WinPE and in the installed system; goes and comes back on eject/reinsert |
| GPIO | `RKCP3002` | [rk3xgpio](drivers/gpio/rk3xgpio) (GpioClx) | accepts GPIO version `V2_2` | **Started**, all 5 banks¹ |
| I²C | `RKCP3001` | [rk3xi2c](drivers/i2c/rk3xi2c) (SpbCx) | none; needs `rockchip,bclk` from the firmware² | **Started**, all 10 |
| DMA | `ARMH0330` | [pl330dma](drivers/dma/pl330dma) | none | **Started**, all 3 |
| Ethernet GMAC0 | `RKCP6543` | [dwc_eqos](drivers/net/dwc_eqos) (NetAdapterCx) | DMA enhanced address mode (EAME) on, so buffers above 4 GB work³ | **Working**: the installed system gets a DHCP lease (2026-10-01, one boot) |
| SPI | `RKCP3003` | [rk3xspi](drivers/spi/rk3xspi) (SpbCx) | ours; RK3588 has no SPI driver | **Started**, all 5; no SPI device exercised |
| NVMe | — (PCIe) | **inbox** stornvme | | **Working** in a stock ADK WinPE |
| USB (xHCI) | `PNP0D10` | **inbox** usbxhci | | working for input; `XHC0` (the USB-C DWC3) is code 10 |
| Display | — | **inbox** BasicDisplay | | UEFI GOP framebuffer ([display](docs/DISPLAY.md)) |
| Audio | — | not ported | | RK3576 uses SAI, not RK3588's I²S-TDM ([audio](docs/AUDIO.md)) |

¹ The driver it replaced, written for this repo, had started on four of the
  five banks.
² The firmware did not publish it, and `rk3xi2c` refuses to start without it.
  Added in firmware `de1d712`.
³ The driver asked Windows for 40-bit DMA addresses but never set
  `DMA_SysBus_Mode.EAME`, so the DMA used only the low 32 bits. RK3576 DRAM
  starts at 0x40000000 and runs past 4 GB, so a buffer above 4 GB lands
  outside DRAM and both DMA channels stopped with a Fatal Bus Error. RK3588,
  whose DRAM starts at 0, truncates into DRAM instead and does not fault.
  Mainline stmmac sets EAME whenever the DMA is wider than 32 bits. Ethernet
  also needs firmware `4e3f7af` or later, which fixed the PHY's receive delay.

Measured 2026-10-01 on CM5-IO, in WinPE built from this tree, with firmware
`de1d712`: every one of the 26 devices above Started, and the four devices
Windows reports a problem with are the ones it always has (the RTC and a UART
with Linux-only IDs, `XHC0`, one `PRP0001`). One boot, with an SD card in the
slot. At boot the SD card was first identified with an all-zero CID; after
reinsertion it read the real one. Both times the disk worked.

WinPE carries every class extension these need (sdport, GpioClx, SpbCx,
NetAdapterCx): all of them started there.

### Before the port

The from-scratch eMMC driver ran identification through EXT_CSD and stopped at
the bus-width test; the SD driver never issued a command.

**An SD card in the slot used to make Windows crawl or bugcheck**, because no
UEFI driver quiesced the SD controller at ExitBootServices. Fixed in the
firmware; four clean card-present boots since, which is not yet a number to
trust. Record whether a card is in the slot on every run.

## Build

These are **ARM64 kernel drivers**. The ported set builds as one solution, the
way worproject's own CI builds it:

```cmd
msbuild build\RockchipDrivers.sln /p:Configuration=Release /p:Platform=ARM64
```

Packages land in `build\ARM64\Release\Output\<driver>\`. SPI builds on its own:

```cmd
msbuild drivers\spi\rk3xspi\rk3xspi.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

CI builds both on every push and fails on any error
([ci.yml](.github/workflows/ci.yml)). The WinPE image workflow
([winpe.yml](.github/workflows/winpe.yml)) injects eMMC, SD, GPIO, I²C, DMA,
GMAC and SPI, test-signed. It leaves the audio drivers out: there is no RK3576
audio device in the DSDT, and `rk3xi2sbus` binds `RKCP3003`, which is our SPI.

Toolchain, test-signing and install: [docs/BUILDING.md](docs/BUILDING.md).

## Firmware (ACPI) changes

The RK3576 EDK2 port is part of this project; the ACPI changes the drivers
depend on are made there.

In the firmware's `main`:

- ACPI built into the image, with `PcdConfigTableModeDefault = 0x3` so one
  image serves both FDT (Linux) and ACPI (Windows).
- The RK3588 SCMI device removed: it rang a doorbell RK3576 does not have, and
  it is what made Setup bugcheck.
- The MCFG and MADT fixes that got NVMe working and all eight CPUs up.
- The eMMC `_DSD` and `_DSM` corrected for RK3576.
- Both SD/eMMC controllers quiesced at ExitBootServices.

Added in `de1d712` for the ported drivers:

- `rockchip,bclk` on every I²C controller.
- The SD UHS modes off and `no-1-8-v` set: nothing Windows can reach switches
  the card's signalling to 1.8 V on RK3576.

Still open:

- **Audio.** RK3576 has SAI blocks, not RK3588's I²S-TDM, so `rk3xi2sbus`
  does not apply. The stale `I2s.asl` also reuses `RKCP3003`, our SPI's HID.
  See [docs/AUDIO.md](docs/AUDIO.md).

## Layout

```
build/                  RockchipDrivers.sln, common.props (from worproject)
drivers/
  sd/dwcsdhc/           eMMC host (DWCMSHC)        sdport miniport
  sd/dwcmshc/           SD card host (dw_mmc)      sdport miniport
  gpio/rk3xgpio/        GPIO controller            GpioClx
  i2c/rk3xi2c/          I²C controller             SpbCx
  dma/pl330dma/         PL330 DMA controller
  net/dwc_eqos/         GMAC Ethernet (DWC EQoS)   NetAdapterCx
  spi/rk3xspi/          SPI controller             SpbCx (ours)
  audio/                I²S, codec, audio port (RK3588 only, not injected)
  lib/, shared/, include/, sd/rk_sip_sdmmc_lib/   shared code
  inc/                  RK3576 SoC constants (ours)
docs/                   bring-up plan, install, storage, display, audio
tools/
  make-woa-usb.sh       build a Windows install stick that boots without NVRAM
  woa-debug/            collect.cmd: what to run inside WinPE, and why
.github/workflows/      CI and the WinPE image
```

## Acknowledgements

Thanks to **[ArmSoM](https://www.armsom.org/)** for sponsoring the **CM5** module
and the **CM5-IO** carrier board this project is developed on.

The drivers are the work of the
[worproject Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers)
authors: Mario Bălănică (SD/eMMC), CoolStar (GPIO, I²C, DMA, audio) and
Doug Cook (Ethernet), building on Microsoft's driver samples.

## License

Code written for this repository is MIT — see [LICENSE](LICENSE). The ported
drivers keep their own licences (MIT, Apache-2.0, BSD); see
[THIRD_PARTY.md](THIRD_PARTY.md).
