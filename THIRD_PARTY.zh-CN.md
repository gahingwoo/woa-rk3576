# 第三方代码

[English](THIRD_PARTY.md) | 简体中文

`build/` 和 `drivers/` 中的大部分来自
[worproject/Rockchip-Windows-Drivers](https://github.com/worproject/Rockchip-Windows-Drivers)，
即 RK3588 的 Windows on Arm 驱动，在 commit `e00e70d`（2025-03-18）导入。
导入 commit 原样带入这些代码，没有改动；之后为 RK3576 做的每处修改都是单独的 commit，所以
`git log -- <driver>` 能准确看出改了什么、为什么改。

每个驱动都保留自己的许可证和版权声明。

| 路径 | 作者 | 许可证 |
|---|---|---|
| `drivers/gpio/rk3xgpio` | CoolStar | Apache-2.0（`LICENSE.txt`） |
| `drivers/i2c/rk3xi2c` | CoolStar | Apache-2.0（`LICENSE.txt`） |
| `drivers/dma/pl330dma` | CoolStar | Apache-2.0（`LICENSE.txt`） |
| `drivers/audio/rk3xi2sbus` | CoolStar | Apache-2.0（`LICENSE.txt`） |
| `drivers/audio/codecs/es8323` | CoolStar | Apache-2.0（`LICENSE.txt`） |
| `drivers/audio/csaudiork3x` | Microsoft 示例，CoolStar | 源仓库中没有许可证文件 |
| `drivers/net/dwc_eqos` | Doug Cook | 上游为 MIT（`LICENSE`）；在这里以 GPL-2.0-only 分发（`LICENSING.md`） |
| `drivers/sd/dwcmshc` | Mario Bălănică，Microsoft 示例 | 上游为 MIT（SPDX 头）；在这里以 GPL-2.0-only 分发（`LICENSING.md`） |
| `drivers/sd/dwcsdhc` | Microsoft sdhc 示例，Greg Garbern，Mario Bălănică | 只有版权声明，没有许可证声明。它所衍生自的上游示例是 MIT。 |
| `drivers/sd/rk_sip_sdmmc_lib` | Mario Bălănică | MIT（SPDX 头） |
| `drivers/lib/arm_smc_lib` | ARM Limited，Microsoft | BSD（文件头） |
| `drivers/shared`、`drivers/include` | Mario Bălănică，Microsoft，NXP，DataCore | MIT / BSD-3-Clause（文件头） |

以下不是来自 worproject。厂商代码是双许可证的，这里注明了所采用的那一项：

| 路径 | 作者 | 许可证 |
|---|---|---|
| `drivers/video/vdpu383/lib/vdpu383_cabac.c` | Rockchip Electronics（MPP） | Apache-2.0 OR MIT（文件头） |
| `drivers/video/vdpu383/lib/vdpu383_h264.c`，寄存器布局 | 移植自 Rockchip MPP，mpp/hal/rkdec | Apache-2.0 OR MIT，按 MIT 使用；整个文件连同这里做的修改为 GPL-2.0-only（文件头） |
| `drivers/video/vdpu383/warmup.c`，预热数据和顺序 | Rockchip Electronics，厂商内核 mpp_hack_rk3576.c | GPL-2.0+ OR MIT，按 GPL-2.0 使用；该文件为 GPL-2.0-only（文件头） |

有两项没有明确的许可证：`dwcsdhc` 和 `csaudiork3x`。

为本仓库编写的代码为 GPL-2.0-only（见 [LICENSE](LICENSE)）。
如果移植来的驱动的许可证允许，该驱动在这里以 GPL-2.0-only 分发：`dwc_eqos` 和 `dwcmshc`，两者上游都是 MIT。
它们的上游声明都保留着，并且各有一个 `LICENSING.md` 说明这一点。

其余的保持各自的许可证，在这里对它们做的修改也按该许可证进行：

- **Apache-2.0 驱动。** Apache-2.0 代码不能和 GPL-2.0 代码合并，所以这些驱动及对它们的每处修改都保持 Apache-2.0。
- **共享库和头文件**（`drivers/shared`、`drivers/include`、`drivers/lib`、`rk_sip_sdmmc_lib`）。它们保持宽松许可证，因为
  Apache-2.0 驱动也要用它们来构建。
- **`dwcsdhc` 和 `csaudiork3x`。** 它们的上游许可证不明确，所以保持原样。

每个驱动都构建成独立的二进制文件，所以本仓库只是把它们并排分发。

## 未附带的固件

`drivers/bluetooth/bcm4345bt` 需要 Broadcom 的 BCM4345C5 patchram，它不在本仓库中，也不在本仓库构建的任何包中。
Ampak 为 AP6256 做的版本（armbian/firmware）没有写明许可证；Raspberry Pi 的 BCM43456 版本
（RPi-Distro/bluez-firmware）采用 Cypress 的许可证，允许以目标代码形式再分发，用于 Cypress 的器件。
驱动的 `install-firmware.ps1` 在板子上下载其中之一，固定在某个 commit，并校验其 SHA-256。
为了 BthX 接口参考过 Microsoft 的 SerialHCIBus 示例（MS-PL），但没有使用其中的任何代码，因为 MS-PL 和 GPL-2.0 不能合并。

## 未导入的部分

- worproject 的 `drivers/storage_fix/*.sys` 和 `drivers/usb/usbehci_nointerlocked`：打过补丁的 Microsoft 系统二进制文件
  （`storahci.sys`、`stornvme.sys`、`usbehci.sys`）。修改过的 Windows 二进制文件不能再分发。这里也都用不到：
  NVMe 用系统自带的 `stornvme.sys` 就能工作，固件也不公开 EHCI 控制器。`storage_fix` 的 readme 和注册表文件保留下来作参考。
