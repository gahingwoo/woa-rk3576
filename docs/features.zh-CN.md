# 在 Windows 下使用硬件

[English](features.md) | 简体中文

各项功能在 Windows 11 23H2 下需要什么。各板的状态见 [README](../README.zh-CN.md#状态)。"ci 固件"指 0.4.1 之后的 edk2-rk3576：下一个正式版之前用它的 [`ci` 构建](https://edk2.gahingwoo.com/flash.zh-CN.html#滚动的-ci-构建)。

## 存储、USB、以太网

| 功能 | 驱动 | 说明 |
|---|---|---|
| eMMC | `dwcsdhc` | CM5-IO 和泰山派 3M 从它启动 Windows |
| SD 卡 | `dwcmshc` | 可以弹出和重新插入（CM5-IO） |
| NVMe（CM5-IO、ROCK 4D） | 系统自带 `stornvme` | ROCK 4D 从它启动 Windows |
| USB-A | 系统自带 `usbxhci` | |
| USB-C，CM5-IO | 系统自带 `usbxhci` | USB 2.0 |
| USB-C，泰山派 3M | 系统自带 `usbxhci` | ci 固件。只有上电前插好的设备可用（USB 3，硬盘 329 MB/s）；开机后再插的设备要等下次启动才有电。要接充电器或电脑，先重启 |
| 以太网 | `dwc_eqos` | DHCP；泰山派 3M 上是 1 Gbps。ROCK 4D：ci 固件（它设置 RTL8211F 的 RGMII 延时） |

## 显示

没有 GPU 驱动时，系统自带的 BasicDisplay 显示固件准备好的帧缓冲：只有一个分辨率，没有热插拔，由 CPU 绘制。

装了 [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)（泰山派 3M、ROCK 4D）后，桌面由 Mali-G52 绘制，DSI 屏可以转成横屏，有亮度滑块，HDMI 支持热插拔和 HDMI 音频（DSI 屏只有泰山派 3M 有）。具体设置见那个仓库。

## 触摸（泰山派 3M，ci 固件）

[gt9271](../drivers/input/gt9271) 驱动 DSI 屏的 GT9271，十点触摸。只有接着屏幕时，固件才声明触摸控制器（`\_SB.I2C0.TCH0`，`RKCP9271`）。

## 音频（泰山派 3M、ROCK 4D，ci 固件）

- 耳机孔和耳机麦克风：rk3xi2sbus、es8323 和 csaudiork3x 1.0.0.135 或更新版本。保留已装好的 pl330dma。
- HDMI 音频：同一套驱动，并由 wddm-mali-g52 驱动 HDMI（它的 `HdmiHpd` 设为 3）。
- ROCK 4D：耳机、麦克风和 HDMI 音频用同一套驱动都能用。它的耳机孔没有检测开关，通过 rk3xsaradc 检测还没试过（见下文）。
- CM5-IO 上固件声明了 HDMI 音频，没试过。

细节见[音频](dev/audio.zh-CN.md)。

## 蓝牙（泰山派 3M，ci 固件）

[bcm4345bt](../drivers/bluetooth/bcm4345bt)：能配对、传文件。不能通话，耳机麦克风也不能用（SCO 走的 PCM 链路没有驱动）。

1. 用驱动附带的 `install-firmware.ps1` 下载一次 patchram（它会下载并校验 SHA-256；这个文件不能随本仓库分发）。
2. 安装 bcm4345bt 驱动包（见[驱动](drivers.zh-CN.md)）。

## 视频解码器

[vdpu383](../drivers/video/vdpu383) 和它的测试工具 `vdpu383test` 在泰山派 3M 上解码 H.264，一致性码流的输出与 FFmpeg 逐位一致；在 ROCK 4D 上也能用。需要在固件设置里打开 *Video Decoder (VDPU383)*（ci 固件里默认打开，0.4.1 里默认关闭）。播放器还用不上它：见[视频解码](dev/video-decode.zh-CN.md)。

## 泰山派 3M 上的按键（仅 CI）

板上有 RECOVERY 和 RESET 两个键，电源管理芯片的开机线上没有按键。

| 按键 | Windows 下 |
|---|---|
| RECOVERY | 电源按钮：短按执行"控制面板 > 电源选项 > 选择电源按钮的功能"里设定的动作 |
| RESET | 复位开发板 |

需要 edk2-rk3576 的 `main`（设置项 "ADC keys for the OS"，这块板上默认打开）和 rk3xsaradc 驱动。开机或唤醒后要先看到按键松开，按键才生效，而且要按住两次采样（约 200 ms），所以开机时按住 RECOVERY 不会有动作。

检查：`reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0ADC\0\Device Parameters"` 应显示 `DiagKeysArmed 1`，按一下之后 `DiagHidReports` 变大。如果这个 ADC 设备碍事，关掉这个设置项，固件就不再碰 ADC。

## 其他板子的 ADC 按键和耳机插孔（仅 CI）

[rk3xsaradc](../drivers/adc/rk3xsaradc) 绑定 `ACPI\RKCP0ADC`（ROCK 4D：ci 固件；CM5-IO：固件分支）。它们的 Maskrom 键成为厂商页 0xFF00 上的 HID 按键，Windows 本身不处理。ROCK 4D 的耳机插孔没有检测开关，在 ADC 通道 3 上读取，经 es8323 控制"耳机"端点。`tools/adctest` 可读任意通道。

## NPU

见 [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu)（泰山派 3M、ROCK 4D）。

## 负载和温度

23H2 的任务管理器不显示 NPU 和温度。rk-monitor 可以：托盘图标显示最高温度，点开有弹窗，还有一个桌面挂件，显示 CPU 各簇、GPU（占用、温度、频率）、NPU 各核心和各温区。没装 maliwddm 时，GPU 一栏显示 "Mali driver not installed"。

安装 RK3576 工具包（CI 产物 `rk3576-tools-arm64`）：解压后运行 `install.cmd`。它把 rk-monitor、`rknputest` 和 `charsiu` 命令（在系统 Path 上）放进 `%ProgramFiles%\RK3576`，让 rk-monitor 在每次登录时启动并打开挂件，并在每次驱动更新后重新注册 GPU 和 NPU 的性能计数器。`uninstall.cmd` 会全部删除。详情：[tools/rk3576-tools](../tools/rk3576-tools/README.zh-CN.md)、[tools/rk-monitor](../tools/rk-monitor/README.zh-CN.md)。

## 不支持

- 所有板子的 Wi-Fi（见[无线](dev/wireless.zh-CN.md)），以及 ROCK 4D 的蓝牙。
- 泰山派 3M 上 EC20 模块的移动数据：这个固件从不回应 MBIM，RNDIS 模式不转发数据。在有移远的新固件之前，模块保持 QMI 模式（见 [EC20](dev/ec20.zh-CN.md)）。
- 泰山派 3M 上有效的 RTC 时间（见[故障排查](troubleshooting.zh-CN.md#每次启动后时间都不对)）。
