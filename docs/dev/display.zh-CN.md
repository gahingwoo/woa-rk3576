# 显示

[English](display.md) | 简体中文

2026-11-24 的状态。

没有 GPU 驱动时，Windows 在固件设置好的帧缓冲上绘制。固件初始化 VOP2 和 HDMI PHY（或泰山派 3M 的 DSI 屏），
交出一个 UEFI GOP 帧缓冲，由系统自带的 BasicDisplay 驱动输出。这样只有 UEFI 选的那个分辨率，别的都没有：不能换分辨率，
不能热插拔，没有加速。

[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) 是 Mali-G52 的 WDDM 驱动，同时也接管显示：桌面在 GPU
上合成，由 VOP2 输出，支持旋转、DSI 亮度、按显示器自己的时序进行 HDMI 热插拔，以及 HDMI 音频。它绑定固件的 ACPI 设备
`RKCP7402`（GPU）和 `RKCP7403`（显示）。它已经在泰山派 3M 上运行过。本仓库与它无关。

在这个驱动出现之前的笔记见
[history](../history/2026-10-01-display-notes.md)。

## 显示相关模块

| 模块 | 基址 | Compatible |
|---|---|---|
| VOP2 | `0x27D00000` | `rockchip,rk3576-vop` |
| HDMI 2.1 控制器（DW-HDMI-QP） | `0x27DA0000` | `rockchip,rk3576-dw-hdmi-qp` |
| HDPTX PHY | `0x2B000000` | `rockchip,rk3576-hdptx-phy` |
| HDPTX PHY GRF | `0x26032000` | `rockchip,rk3576-hdptxphy-grf` |
| MIPI DSI | `0x27D80000` | DSI host |
