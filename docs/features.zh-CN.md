# 在 Windows 下使用硬件

[English](features.md) | 简体中文

各板在 Windows 11 23H2 下能用什么，以及每项功能需要什么。截至 2026-11-24。

"ci 固件"指 edk2-rk3576 的 [`ci` 构建](https://edk2.gahingwoo.com/flash.zh-CN.html#滚动的-ci-构建)：截至 2026-11-24，0.4.1 之后还没有发布包含这些功能的正式版。

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

DSI 屏的 GT9271 配合 [gt9271](../drivers/input/gt9271) 可用，支持十点触摸。只有接着屏幕时，固件才会声明触摸控制器（`\_SB.I2C0.TCH0`，`RKCP9271`）；不接屏幕时，设备管理器里什么也看不到。

## 音频（泰山派 3M，ci 固件）

- 耳机孔：配合 rk3xi2sbus、es8323 和 csaudiork3x 1.0.0.135 或更新版本，播放和耳机麦克风都可用（2026-11-22 起）。保留已经装好的 pl330dma。
- HDMI 音频：wddm-mali-g52 驱动 HDMI 时可用（它的 `HdmiHpd` 设为 3），2026-11-23 确认过。
- CM5-IO 上固件声明了 HDMI 音频，但还没试过。

细节见[音频](dev/audio.md)（英文）。

## 蓝牙（泰山派 3M，ci 固件）

配合 [bcm4345bt](../drivers/bluetooth/bcm4345bt) 可用（2026-11-22 起）：设置里能看到蓝牙，能配对、传文件。不能通话，耳机麦克风也不能用（SCO 走的 PCM 链路没有驱动）。

1. 用驱动附带的 `install-firmware.ps1` 下载一次 patchram（它会下载并校验 SHA-256；这个文件不能随本仓库分发）。
2. 安装 bcm4345bt 驱动包（见[驱动](drivers.zh-CN.md)）。

## 不支持

- 两块板的 WiFi（见[无线](dev/wireless.md)，英文）。
- CM5-IO 的蓝牙。
- 泰山派 3M 上 EC20 模块的网络接口。
- 泰山派 3M 上有效的 RTC 时间（见[故障排查](troubleshooting.zh-CN.md#每次启动后时间都不对)）。

## 视频解码器

RK3576 的 VDPU383 在 Windows 下可以解码 H.264，用的是早期调试驱动 [vdpu383](../drivers/video/vdpu383) 和它的测试工具 `vdpu383test`：2026-11-24 在泰山派 3M 上试过的 H.264 一致性码流，输出全部与 FFmpeg 逐位一致。需要 edk2-rk3576 并在设置里打开 *Video Decoder (VDPU383)*（默认关闭）。视频播放器还用不上它，计划见[视频解码](dev/video-decode.md)（英文）。

## NPU

见 [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu)（泰山派 3M）。

## 负载和温度

Windows 11 23H2 的任务管理器不显示 NPU，也不显示温度。rk-monitor 可以：托盘图标显示最高温度，点开有弹窗，还有一个桌面挂件，显示 CPU 各簇、GPU（占用、温度、频率）、NPU 各核心和各温区。用 RK3576 工具包安装，即 CI 产物 `rk3576-tools-arm64`：解压后运行 `install.cmd`。它把 rk-monitor、`rknputest` 和 `charsiu_run` 放进 `%ProgramFiles%\RK3576`，让 rk-monitor 在每次登录时启动并默认打开挂件，并在每次驱动更新后重新注册 GPU 和 NPU 的性能计数器。`uninstall.cmd` 会全部删除。详情（英文）：[tools/rk3576-tools](../tools/rk3576-tools/README.md)、[tools/rk-monitor](../tools/rk-monitor/README.md)。
