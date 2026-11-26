# Wi-Fi 和蓝牙

[English](wireless.md) | 简体中文

| | 泰山派 3M | CM5-IO |
|---|---|---|
| 蓝牙 | 板测，用 [bcm4345bt](../../drivers/bluetooth/bcm4345bt)：能配对、传文件，没有语音（SCO） | 仅 CI（bcm4345bt，`RKCP4362`） |
| Wi-Fi | SDIO 控制器（dwcmshc）和 [bcmwlsdio](../../drivers/net/bcmwlsdio) W3 于 2026-11-26 板测：固件能运行，还没有网络接口（[wifi.md](wifi.md)） | 仅 CI（bcmwlsdio BCM43752） |

ROCK 4D：USB 上的 AIC8800D80 Wi-Fi 和蓝牙，`main` 上没有驱动。`aic8800-usb` 分支上有一个驱动，还没在板上试过（[wifi.md](wifi.md#other-boards-rock-4d-dragon-q6a-cm5-io)）。

bcm4345bt 自己驱动 UART，不另外用 SerCx2 UART 驱动，并把 HCI 链路交给系统自带的 BthMini；具体做法见它的 README。最初的评估见 [history](../history/2026-11-21-wireless-assessment.md)。

## 硬件

| | 泰山派 3M | CM5-IO（CM5 核心板） |
|---|---|---|
| 模块 | AMPAK AP6256（Broadcom BCM43456） | Synaptics SYN43752（Wi-Fi 6） |
| Wi-Fi 总线 | SDIO，`sdio: mmc@2a320000`（`rockchip,rk3576-dw-mshc`），4 位 | SDIO，同一个控制器 |
| Wi-Fi 供电 | 3.3 V 开关 GPIO0 PD1（低电平有效），WL_REG_ON GPIO1 PC6，host wake GPIO0 PB0 | WL_REG_ON GPIO1 PC6 |
| 32 kHz LPO | HYM8563 RTC 的时钟输出（I2C2 0x51） | HYM8563 |
| 蓝牙总线 | UART6（uart6m1，RTS/CTS），`brcm,bcm4345c5`，最高 1.5 Mbaud | UART4（uart4m1，RTS/CTS） |
| 蓝牙 GPIO | shutdown GPIO1 PC7，host wake GPIO0 PB1，device wake GPIO1 PD4 | BT power GPIO1 PC7（不在 mainline 设备树里） |

对 Wi-Fi，固件在 ACPI 里什么都不声明，也不给任何东西上电：Linux 通过 `mmc-pwrseq` 依次控制 Wi-Fi 的供电、复位和
LPO 时钟。对泰山派 3M 上的蓝牙，固件声明了 `\_SB.BTH0`（`RKCP4345`），带 UART6、BT_REG_ON 和 BT_WAKE，配置了
UART 引脚复用并设置了它的时钟。

## Wi-Fi：写一个驱动要做什么

- 这些芯片在 ARM64 上没有任何 Windows 驱动。worproject 的 Raspberry Pi 驱动表把 Pi 3B+/4 的 CYW43455（同一系列）
  列为没有驱动。Windows 10 IoT Core 曾为 Pi 3 提供一个闭源、只有 ARM32 的 Broadcom SDIO 驱动；它在 ARM64 上加载不了。
  Microsoft 自己的 Pi BSP（ms-iot/rpi-iotcore）也没有 Wi-Fi 驱动。
- 自己写一个，意味着要为一颗走 SDIO 的 FullMAC 芯片写 WDI 或 WiFiCx miniport：SDIO 总线层，固件、NVRAM 和 CLM 的下载，
  BCDC/fwil 命令和事件协议，以及把扫描、连接、PMK 和漫游对接到 WiFiCx 上。Linux 的 brcmfmac（ISC 许可证，可以参考）
  大约有 35,000 行。WPA supplicant 的工作由芯片固件完成。
- 本仓库的 SD miniport（`drivers/sd/dwcmshc`）没有实现 SDIO 卡中断（它总是报告没有中断），所以这又是额外的工作，
  否则就得用带外的 host-wake GPIO。
- 固件 blob（AP6256 的来自 Cypress/Infineon，SYN43752 的来自 Synaptics）只允许以目标代码形式再分发，而 AP6256 的
  NVRAM 文件来自厂商 BSP，条款不明确。

粗略工作量：在一颗芯片上做到扫描、WPA2 连接和漫游要 6 到 9 人月，之后 WPA3、电源管理和加固再要 1 到 2 人月；第二颗芯片还要再单独做一遍适配。

在那之前用以太网。USB Wi-Fi 网卡只有在它的确切硬件 ID 有 Windows ARM64 驱动时才能用（Microsoft Update Catalog 里按 ARM64 筛选，或 23H2 映像自带的 `net*.inf`）；目前没有确认任何一款。
