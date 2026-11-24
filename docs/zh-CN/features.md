# 在 Windows 下使用硬件

各板在 Windows 11 23H2 下能用什么，以及每项功能需要什么。截至 2026-11-24。
[English](../features.md)（本页对应 2026-11-24 的英文版）

"ci 固件"指 edk2-rk3576 的 [`ci` 构建](https://edk2.gahingwoo.com/zh-CN/flash.html#滚动的-ci-构建)：截至 2026-11-24，0.4.1 之后还没有发布包含这些功能的正式版。

## 存储

- eMMC：两块板都从它启动 Windows（`dwcsdhc`）。
- SD 卡：CM5-IO 上可用，包括弹出和重新插入（`dwcmshc`）。泰山派 3M 上驱动能启动，没插卡试过。
- NVMe（CM5-IO）：系统自带的 `stornvme`。

## USB

- USB-A：两块板都可用（系统自带的 `usbxhci`）。
- CM5-IO 的 USB-C：USB 2.0。
- 泰山派 3M 的 USB-C（ci 固件）：上电前插好的设备可用，试过的硬盘达到 USB 3 速度（329 MB/s）。开机后再插的设备要等下次启动才有电。要接充电器或电脑，先重启。

## 以太网

两块板都可用（`dwc_eqos`），能拿到 DHCP 地址；泰山派 3M 上是 1 Gbps。

## 显示

没有 GPU 驱动时，Windows 用系统自带的 BasicDisplay 显示固件准备好的帧缓冲：只有一个分辨率，没有热插拔，由 CPU 绘制。

装了 [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)（泰山派 3M）：桌面由 Mali-G52 绘制，DSI 屏可以在设置里转成横屏，有亮度滑块，HDMI 支持热插拔和 HDMI 音频。具体设置见那个仓库。

## 触摸（泰山派 3M，ci 固件）

DSI 屏的 GT9271 配合 [gt9271](../../drivers/input/gt9271) 可用，支持十点触摸。只有接着屏幕时，固件才会声明触摸控制器（`\_SB.I2C0.TCH0`，`RKCP9271`）；不接屏幕时，设备管理器里什么也看不到。

## 音频（泰山派 3M，ci 固件）

- 耳机孔：配合 rk3xi2sbus、es8323 和 csaudiork3x 1.0.0.135 或更新版本，播放和耳机麦克风都可用（2026-11-22 起）。保留已经装好的 pl330dma。
- HDMI 音频：wddm-mali-g52 驱动 HDMI 时可用（它的 `HdmiHpd` 设为 3），2026-11-23 确认过。
- CM5-IO 上固件声明了 HDMI 音频，但还没试过。

细节见[音频](../dev/audio.md)（英文）。

## 蓝牙（泰山派 3M，ci 固件）

配合 [bcm4345bt](../../drivers/bluetooth/bcm4345bt) 可用（2026-11-22 起）：设置里能看到蓝牙，能配对、传文件。不能通话，耳机麦克风也不能用（SCO 走的 PCM 链路没有驱动）。

1. 用驱动附带的 `install-firmware.ps1` 下载一次 patchram（它会下载并校验 SHA-256；这个文件不能随本仓库分发）。
2. 安装 bcm4345bt 驱动包（见[驱动](drivers.md)）。

## 不支持

- 两块板的 WiFi（见[无线](../dev/wireless.md)，英文）。
- CM5-IO 的蓝牙。
- 泰山派 3M 上 EC20 模块的网络接口。
- 泰山派 3M 上有效的 RTC 时间（见[故障排查](troubleshooting.md#每次启动后时间都不对)）。

## NPU

见 [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu)（泰山派 3M）。
