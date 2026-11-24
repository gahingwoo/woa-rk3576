# Architecture

How the drivers fit together, and where they come from.

## How Windows finds the hardware

Windows finds hardware through ACPI. The firmware and its ACPI tables are in
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576) (which device on which
board: its [ACPI page](https://edk2.gahingwoo.com/dev/acpi.html)). This
repository has the drivers for the controllers Windows has no inbox driver
for. Windows has no clock, reset, power-domain or pin driver for this SoC, so
the firmware sets each block up once and the drivers only program the block
itself.

| Peripheral | `_HID` | Driver | Framework |
|---|---|---|---|
| eMMC | `RKCP0D40` | [dwcsdhc](../../drivers/sd/dwcsdhc) | sdport miniport |
| SD card | `RKCPFE2C` | [dwcmshc](../../drivers/sd/dwcmshc) | sdport miniport |
| GPIO | `RKCP3002` | [rk3xgpio](../../drivers/gpio/rk3xgpio) | GpioClx |
| I2C | `RKCP3001` | [rk3xi2c](../../drivers/i2c/rk3xi2c) | SpbCx |
| SPI | `RKCP3003` | [rk3xspi](../../drivers/spi/rk3xspi) | SpbCx |
| DMA | `ARMH0330` | [pl330dma](../../drivers/dma/pl330dma) | |
| Ethernet | `RKCP6543` | [dwc_eqos](../../drivers/net/dwc_eqos) | NetAdapterCx |
| Touch (TaishanPi 3M) | `RKCP9271` | [gt9271](../../drivers/input/gt9271) | VHF |
| Bluetooth (TaishanPi 3M) | `RKCP4345` | [bcm4345bt](../../drivers/bluetooth/bcm4345bt) | BthX bus driver, in-box BthMini |
| Audio | `RKCP3004`, `RKCP8388` | [rk3xi2sbus, csaudiork3x, es8323](../../drivers/audio) | ([audio](audio.md)) |
| NVMe | PCIe | inbox stornvme | |
| USB | `PNP0D10` | inbox usbxhci | |
| GPU and display | `RKCP7402`, `RKCP7403` | [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) | WDDM |
| NPU | `RKCP3576` | [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) | KMDF |

## Where the drivers come from

Most are ports of
[worproject/Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers),
the RK3588 drivers, which bind the same ACPI `_HID`s. The import at `e00e70d`
is one unmodified commit, and each RK3576 change is its own commit after it,
so `git log -- drivers/<class>/<name>` shows what differs from RK3588:

- dwcsdhc: card clock from the CRU instead of a TF-A SiP call; Rockchip vendor
  bits restored after each controller reset; ADMA2 descriptors split at the
  128 MiB boundary (merged 2026-11-24).
- dwcmshc: clock from the CRU, phases in the controller's `TIMING_CON`, 3.3 V
  only.
- rk3xgpio: accepts GPIO version `V2_2`.
- dwc_eqos: turns on the DMA's 40-bit addressing (EAME). RK3576's memory runs
  past 4 GB, and without it every transfer to a buffer above 4 GB faulted.
- csaudiork3x and rk3xi2sbus: drive the RK3576 SAI instead of RK3588's
  I2S-TDM block.

Written for this repository:

- rk3xspi; the RK3588 set has no SPI driver.
- gt9271, the TaishanPi 3M touch screen driver.
- bcm4345bt, the TaishanPi 3M Bluetooth driver. It runs the UART itself
  (Windows on Arm has no driver for it) and does not ship the Broadcom
  patchram, which `install-firmware.ps1` fetches.
- es8323's RK3576 sequence for the ES8388 codec.

worproject's RK3588 README, as imported, is in
[history](../history/upstream-README-rk3588.md). Licences per driver:
[THIRD_PARTY.md](../../THIRD_PARTY.md).

## Layout

```
build/                  RockchipDrivers.sln, common.props (from worproject)
drivers/
  sd/dwcsdhc/           eMMC host (DWCMSHC)        sdport miniport
  sd/dwcmshc/           SD host (dw_mmc)           sdport miniport
  gpio/rk3xgpio/        GPIO                       GpioClx
  i2c/rk3xi2c/          I2C                        SpbCx
  dma/pl330dma/         PL330 DMA
  net/dwc_eqos/         Ethernet (DWC EQoS)        NetAdapterCx
  spi/rk3xspi/          SPI                        SpbCx (written here)
  input/gt9271/         GT9271 touch screen        VHF (written here)
  bluetooth/bcm4345bt/  BCM4345C5 on UART6         BthX bus driver (written here)
  audio/                SAI audio and the ES8388 codec, on the WinPE media in woa-audio\
  lib/, shared/, include/, sd/rk_sip_sdmmc_lib/   shared code
tools/
  woa-deploy/           install Windows to the eMMC from WinPE
  woa-debug/            collect data in WinPE and in the installed system
  pageverify/           page-by-page integrity test for large files
  make-woa-usb.sh       a Windows Setup stick that boots without NVRAM
  rk-monitor/           tray app and desktop widget: CPU, GPU and NPU load, temperatures
  rk3576-tools/         installer of the user tools (rk-monitor, rknputest, charsiu_run)
docs/
.github/workflows/      CI and the WinPE image
```
