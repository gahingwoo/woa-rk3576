# woa-rk3576

[English](README.md) | 简体中文

Rockchip RK3576 开发板的 Windows on Arm 驱动，以及把 Windows 11 23H2 装到板子上的 WinPE 映像和脚本。在 ArmSoM CM5-IO 和立创泰山派 3M 上跑过。

第一次接触这些板子？请从 [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/blob/main/README.zh-CN.md) 开始：先装固件，它的说明里有从新板子到 Windows 或 Linux 的完整路径。

![泰山派 3M 的 DSI 屏上横屏显示的 Windows 11，报告 10 点触摸](docs/imgs/tspi3m-win11-touch-landscape.png)

## 安装

1. 把 [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576) 固件以完整映像写到 eMMC。
2. 按 [安装说明](docs/INSTALL.zh-CN.md) 操作：[最新正式版](https://github.com/gahingwoo/woa-rk3576/releases/latest)的 WinPE U 盘，Windows 11 23H2 ARM64 ISO 里的 `install.wim`，运行一个脚本。

也可以用 [Maskrom Studio](https://github.com/gahingwoo/maskrom-studio) 在电脑上直接安装，不用 U 盘（截至 2026-11-24 还没有在板子上跑过）。

之后：[在 Windows 下使用硬件](docs/features.zh-CN.md)（CPU、GPU、NPU 负载和温度：见 [RK3576 工具](tools/rk3576-tools/README.zh-CN.md)）、[添加和替换驱动](docs/drivers.zh-CN.md)、[故障排查](docs/troubleshooting.zh-CN.md)。

## 状态

2026-11-24，Windows 11 23H2（22631）。"ci 固件"指 0.4.1 之后的 edk2-rk3576（下一个正式版发布前用它的 `ci` 构建）。

| | ArmSoM CM5-IO | 立创泰山派 3M | 驱动 |
|---|---|---|---|
| eMMC（系统盘） | 可用 | 可用 | dwcsdhc |
| SD 卡 | 可用，包括弹出和重新插入 | 驱动能启动；没插卡试过 | dwcmshc |
| NVMe | 可用 | 没有 M.2 插槽 | 系统自带 stornvme |
| USB-A | 可用 | 可用 | 系统自带 usbxhci |
| USB-C | USB 2.0 | 上电时插着的设备可用（ci 固件） | 系统自带 usbxhci |
| 以太网 | 可用，DHCP | 可用，1 Gbps | dwc_eqos |
| GPIO、I2C、SPI、DMA | 能启动 | 能启动 | rk3xgpio、rk3xi2c、rk3xspi、pl330dma |
| 40 针排针：UART 作 COM 口，GPIO/I2C/SPI 供应用使用 | 固件在板子分支上；未试 | 未试（设置项，默认关闭） | rk3xuart、系统自带 rhproxy（[40 针排针](docs/pin40.zh-CN.md)） |
| 显示 | 固件帧缓冲，一个分辨率 | 配合 [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) 由 GPU 绘制桌面；否则是帧缓冲 | 系统自带 BasicDisplay，或 wddm-mali-g52 |
| 触摸（DSI 屏） | 未试过 | 可用，10 点（ci 固件） | gt9271 |
| 耳机孔和麦克风 | 底板上没有耳机孔 | 可用（ci 固件） | rk3xi2sbus、csaudiork3x、es8323 |
| HDMI 音频 | 未试过 | 配合 wddm-mali-g52 可用（ci 固件） | 同上 |
| 蓝牙 | 没写驱动 | 可用：配对、传文件；不能通话（ci 固件） | bcm4345bt |
| WiFi | 不能用 | 不能用 | 无 |
| NPU | 不支持 | 配合 [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) 可用 | rknpu |
| 视频解码器（VDPU383） | 未试 | 用 vdpu383test 硬件解码可用（H.264 一致性码流与 FFmpeg 逐位一致，2026-11-24）；播放器尚不能用（[视频解码](docs/dev/video-decode.zh-CN.md)） | vdpu383 |
| ADC 按键、ADC 读取的耳机插孔 | 驱动已写，未试过 | 驱动已写，未试过 | rk3xsaradc |
| RTC、PMIC、引脚控制器、UART2 | 不需要驱动 | 不需要驱动；有名称，没有黄色感叹号（2026-11-24 在泰山派 3M 上；那里已不再声明 UART2） | [null](drivers/null) |

每项结果都来自同一块板子。CM5-IO 一栏大部分是 2026-10-01 看到的。各项功能的细节见[硬件功能](docs/features.zh-CN.md)。

## 正式版和滚动的 ci 构建

正式版包含 WinPE 映像（`woa-rk3576-winpe-<版本>.img.xz`）、驱动和 `SHA256SUMS.txt`。最新的是 0.1.1（2026-11-13）；触摸、蓝牙和音频驱动都比它新。

标签为 [`ci`](https://github.com/gahingwoo/woa-rk3576/releases/tag/ci) 的预发布版是 `main` 最近一次成功构建的 WinPE 映像（`woa-rk3576-winpe-ci-<sha>.img.xz` 和 `SHA256SUMS.txt`）。它不会被标为最新版，每次构建成功都会被替换，整体也没有测试过。除非需要只在 `main` 上的改动，否则请用正式版。Maskrom Studio 的 CI 档用的就是这个 `ci` 版本。

## 开发文档

[架构](docs/dev/architecture.zh-CN.md)、[编译](docs/dev/building.zh-CN.md)、[调试](docs/dev/debugging.zh-CN.md)、[音频](docs/dev/audio.zh-CN.md)、[显示](docs/dev/display.zh-CN.md)、[无线](docs/dev/wireless.zh-CN.md)、[EC20 4G](docs/dev/ec20.zh-CN.md)、[历史记录](docs/history/README.md)。

## 致谢

感谢[嘉立创](https://jlc.com/)（JLC）赞助本项目使用的立创泰山派 3M、MIPI DSI 屏、风扇和 EC20 4G 模块，感谢 [ArmSoM](https://www.armsom.org/) 赞助 CM5 核心板和 CM5-IO 底板。

移植来的驱动出自 worproject Rockchip-Windows-Drivers 的作者：Mario Bălănică（SD/eMMC）、CoolStar（GPIO、I2C、DMA、音频）和 Doug Cook（以太网），基于 Microsoft 的驱动示例。

## 许可证

为本仓库编写的代码采用 GPL-2.0-only（[LICENSE](LICENSE)）。在此之前的正式版是 MIT，仍保持 MIT。移植的驱动保留各自的许可证，对它们的 RK3576 改动沿用所改驱动的许可证；见 [THIRD_PARTY.md](THIRD_PARTY.zh-CN.md)。
