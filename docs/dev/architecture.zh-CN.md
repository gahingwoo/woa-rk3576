# 架构

[English](architecture.md) | 简体中文

各个驱动如何配合，以及它们的来历。

## Windows 怎样找到硬件

Windows 通过 ACPI 找到硬件。固件和它的 ACPI 表在
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576) 里（哪块板子上有哪些设备，见它的
[ACPI 页面](https://edk2.gahingwoo.com/dev/acpi.zh-CN.html)）。本仓库放的是 Windows 没有自带驱动的那些控制器的驱动。
Windows 没有这颗 SoC 的时钟、复位、电源域和引脚驱动，所以由固件把每个模块初始化一次，驱动只负责编程模块本身。

| 外设 | `_HID` | 驱动 | 框架 |
|---|---|---|---|
| eMMC | `RKCP0D40` | [dwcsdhc](../../drivers/sd/dwcsdhc) | sdport miniport |
| SD 卡 | `RKCPFE2C` | [dwcmshc](../../drivers/sd/dwcmshc) | sdport miniport |
| GPIO | `RKCP3002` | [rk3xgpio](../../drivers/gpio/rk3xgpio) | GpioClx |
| I2C | `RKCP3001` | [rk3xi2c](../../drivers/i2c/rk3xi2c) | SpbCx |
| SPI | `RKCP3003` | [rk3xspi](../../drivers/spi/rk3xspi) | SpbCx |
| DMA | `ARMH0330` | [pl330dma](../../drivers/dma/pl330dma) | |
| 以太网 | `RKCP6543` | [dwc_eqos](../../drivers/net/dwc_eqos) | NetAdapterCx |
| 触摸（泰山派 3M） | `RKCP9271` | [gt9271](../../drivers/input/gt9271) | VHF |
| 蓝牙（泰山派 3M） | `RKCP4345` | [bcm4345bt](../../drivers/bluetooth/bcm4345bt) | BthX 总线驱动，系统自带的 BthMini |
| 音频 | `RKCP3004`, `RKCP8388` | [rk3xi2sbus, csaudiork3x, es8323](../../drivers/audio) | （[音频](audio.zh-CN.md)） |
| NVMe | PCIe | 系统自带 stornvme | |
| USB | `PNP0D10` | 系统自带 usbxhci | |
| GPU 和显示 | `RKCP7402`, `RKCP7403` | [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) | WDDM |
| NPU | `RKCP3576` | [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) | KMDF |
| 视频解码器（VDPU383） | `RKCP0383` | [vdpu383](../../drivers/video/vdpu383)（[视频解码](video-decode.zh-CN.md)） | KMDF |

## 驱动从哪里来

大多数驱动移植自
[worproject/Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers)，
也就是 RK3588 的驱动，它们绑定的是同样的 ACPI `_HID`。`e00e70d` 的导入是一个未经修改的提交，之后每一处
RK3576 改动都是单独的提交，所以 `git log -- drivers/<class>/<name>` 就能看出和 RK3588 的差别：

- dwcsdhc：卡时钟改由 CRU 提供，不再用 TF-A SiP 调用；每次控制器复位后恢复 Rockchip 厂商位；ADMA2
  描述符在 128 MiB 边界处拆分（2026-11-24 合并）。
- dwcmshc：时钟来自 CRU，相位放在控制器的 `TIMING_CON` 里，只支持 3.3 V。
- rk3xgpio：接受 GPIO 版本 `V2_2`。
- dwc_eqos：打开 DMA 的 40 位寻址（EAME）。RK3576 的内存超过 4 GB，不打开的话，每次传输到 4 GB 以上的缓冲区都会出错。
- csaudiork3x 和 rk3xi2sbus：驱动 RK3576 的 SAI，而不是 RK3588 的 I2S-TDM 模块。

为本仓库新写的：

- rk3xspi；RK3588 那套驱动里没有 SPI 驱动。
- gt9271，泰山派 3M 的触摸屏驱动。
- bcm4345bt，泰山派 3M 的蓝牙驱动。它自己驱动 UART（Windows on Arm 没有这个 UART 的驱动），不附带 Broadcom
  patchram，patchram 由 `install-firmware.ps1` 下载。
- es8323 中针对 ES8388 codec 的 RK3576 初始化序列。

worproject 的 RK3588 README（导入时的原样）在
[history](../history/upstream-README-rk3588.md)。各驱动的许可证见
[THIRD_PARTY.md](../../THIRD_PARTY.zh-CN.md)。

## 目录结构

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
