# 音频

[English](audio.md) | 简体中文

音频驱动怎样驱动 RK3576 的 SAI，以及固件为它们做了什么。2026-11-24 的状态：

| 端点 | 板子 | 固件设置 | Windows 驱动 | 状态 |
|---|---|---|---|---|
| 耳机孔和耳机孔麦克风（ES8388） | 泰山派 3M | `PcdAnalogAudioPreset`（这块板子上为 TRUE） | rk3xi2sbus、csaudiork3x 1.0.0.135 或更新、es8323、pl330dma | 2026-11-22 起可用 |
| HDMI 输出，48 kHz 立体声 | 泰山派 3M | `PcdHdmiAudioPreset`（TRUE） | 同上，另加负责 HDMI 的 wddm-mali-g52 | 2026-11-23 起可用 |
| HDMI 输出 | CM5-IO | `PcdHdmiAudioPreset`（TRUE） | 同上 | 未试过 |

这些需要比 0.4.1 新的 edk2-rk3576 固件（下一个正式版发布前用 `ci` 构建）。USB 音频设备用 Windows 自带的
USB Audio Class 驱动就能用。

CM5-IO 的 ES8388 只接到一个 3 针排针（CN3），没有耳机孔，而且它的接线不在本项目拿到的任何设备树里，所以没有描述它。

这一页最初来自音频还不能用时写的计划，见
[history](../history/2026-11-21-audio-plan.md)。

## 硬件

RK3576 的音频是 Rockchip SAI（Serial Audio Interface），不是 RK3588 驱动和固件 `I2s.asl` 所对应的旧
I²S/TDM 模块。

| 节点 | 基址 | GIC SPI | ACPI GSIV | DMA（tx, rx） | 电源域 |
|---|---|---|---|---|---|
| sai0 | 0x2A600000 | 187 | 219 | | PD_AUDIO |
| sai1 | 0x2A610000 | 188 | 220 | dmac0 2, 3 | PD_AUDIO |
| sai2 | 0x2A620000 | 189 | 221 | | PD_AUDIO |
| sai3 | 0x2A630000 | 190 | 222 | | PD_AUDIO |
| sai4 | 0x2A640000 | 191 | 223 | | PD_AUDIO |
| sai5 | 0x27D40000 | 192 | 224 | | |
| sai6 | 0x27D50000 | 193 | 225 | dmac2 4, 5 | PD_VO0 |
| sai7 | 0x27ED0000 | 194 | 226 | | |
| sai8 | 0x27EE0000 | 195 | 227 | | |
| sai9 | 0x27EF0000 | 196 | 228 | | |

Windows 没有这颗 SoC 的时钟、复位、电源域和引脚驱动，所以由固件把每个 SAI 初始化一次，驱动只负责编程 SAI 本身。

## 驱动怎样使用 SAI

`rk3xi2sbus` 绑定 `RKCP3004`，读取设备的 `_DSD`
（`rockchip,dma`、`rockchip,tx`、`rockchip,rx`、`rockchip,tplg`、
`rockchip,sai`、`rockchip,mclk-fs`、`rockchip,sai-tx-route`、
`rockchip,sai-rx-route`）。设置了 `rockchip,sai` 时，`csaudiork3x` 按 mainline `rockchip_sai.c` 处理
I²S 的方式运行这个模块：作为位时钟和帧的 master，每帧两个 32 位 slot，都在 lane 0 上，sclk = 64 fs，每个采样占一个
32 位 FIFO 字。因此每个 SAI 端点都是 48 kHz 立体声，32 位容器里有 24 个有效位。sclk 和 LRCK 从启动一直运行到 D3，
两个音频流之间也不停；开始和停止只是开关 TX 或 RX 以及对应的 DMA。

`rockchip,sai-tx-route` 和 `rockchip,sai-rx-route` 就是 mainline 的
`rockchip,sai-tx-route`/`-rx-route`，每个 lane 打包成一个 nibble，lane 0 在 bit 15:12。泰山派 3M 的
`<2 1 0 3>` 就是 `0x2103`：SDO2 承载 lane 0。

## HDMI 音频

通路来自 mainline `rk3576.dtsi`（`hdmi_sound`）：SAI6 是 I²S master，送进 SoC 内部的 DW-HDMI-QP。Windows 没有
HDMI 控制器的驱动，所以固件在 ExitBootServices 时通过 `PcdHdmiAudioPreset` 设置它：`mclk_sai6` = 12.288 MHz，
来自 `clk_audio_frac_0`；控制器的音频通路设为 48 kHz 双声道 L-PCM（N 6144，CTS 为 TMDS 速率 / 1000，channel
status，audio InfoFrame，ACR/AUDI/AUDS 包）。对 DVI 显示器，或 EDID 没有声明 basic audio 的显示器，什么都不写。
然后固件发布 `\_SB.SAI6`，带 `"rockchip,tplg" = "hdmi"`。HDMI 拓扑没有录音端点。

## 泰山派 3M 的模拟音频

ES8388 不在 mainline 设备树里。接线取自立创的厂商设备树（以 Armbian 板级补丁中的形式）：codec 在 I2C3 的 0x11，
MCLK 来自 SAI1 的 MCLK 输出，SAI1 用 M0 引脚，lane 0 输出走 SDO2、输入走 SDI0，LOUT1/ROUT1 接耳机功放（使能脚
GPIO4 PB2，高电平有效），耳机检测在 GPIO4 PB0（上拉），板载麦克风在 LINPUT1/2，耳麦麦克风在 RINPUT1/2。

固件（`PcdAnalogAudioPreset`，`Tspi3mAudio.c`）：打开 PD_AUDIO，`mclk_sai1` = 12.288 MHz，来自
`clk_audio_frac_1`，MCLK 输出到引脚，打开 SAI1、I2C3、GPIO4 和 DMAC0 的时钟，配置引脚复用。DSDT 里有
`\_SB.SAI1`（`"i2s-jack"`）和 `\_SB.I2C3.ES83`。

`es8323` 以 `RKCP8388` 绑定 codec，然后运行它自己的初始化序列（`es8388_sai.c`），数值取自 Everest ES8388
数据手册（rev 5.0）和用户指南（第 10 节），不是来自 Linux：codec 作为 slave，MCLK = 256 fs，DAC 和 ADC 都是 24 位
I²S，两者共用一个 LRCK，LOUT1/ROUT1 为 0 dB，OUT2 关闭，板载麦克风（LINPUT1）接到两个录音声道，PGA +24 dB。
功放使能由 ACPI GpioIo 控制。

### 耳机插拔检测（分支 tspi3m-hp-jack，未在硬件上运行）

固件（edk2-rk3576 分支 `tspi3m-hp-jack`，基于 `rk3576-gpio-pull`，后者给 GPIO4 PB0 加上上拉）：codec 的
`_CRS` 把检测线同时声明为双边沿触发的 GpioInt 和 GpioIo 输入，都是 Shared、上拉；还有一个 `_DSD`，包含
`rockchip,hp-det-gpio-io`（哪个 GpioIo 是检测线）、
`rockchip,hp-det-active-high`（泰山派 3M 上为 1：
`hp-det-gpios = <&gpio4 RK_PB0 GPIO_ACTIVE_HIGH>`）和
`rockchip,hp-det-debounce-ms`（150）。

`es8323`（`es8388_jack.c`）：每个边沿都重新启动一个 150 ms 的被动定时器；定时器到时，驱动读取电平，相应地开关耳机功放，
如果状态变了，就通过 `\Callback\CsAudioCallbackAPI` 为耳机端点发送 `CSAudioEndpointJackState`。`csaudiork3x`
保存这个状态，只在 SAI 耳机拓扑上用它回答 `KSPROPERTY_JACK_DESCRIPTION`（并在 `KSPROPERTY_JACK_DESCRIPTION2`
里给出 `JACKDESC2_PRESENCE_DETECT_CAPABILITY`），并在它的 bridge pin 上触发 `KSEVENT_PINCAPS_JACKINFOCHANGE`。
之后 Windows 会把 "Headphones" 端点显示为已拔出（DeviceState 8）或可用（1）。在 codec 报告之前，以及在没有
`_DSD` 的旧固件上，端点和以前一样保持已连接。

插拔事件发生时，Windows 不会在两个适配器之间切换默认设备，所以切到耳机、再切回 HDMI 由 rk-monitor 完成
（[tools/rk-monitor](../../tools/rk-monitor/README.zh-CN.md)）。

注册表位置 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP8388\0\Device Parameters`：
`JackDetect` = 0 关闭报告，`HpDetInvert` = 1 反转有效电平，`JackAmpGate` = 0 让功放在拔出时保持打开（都在驱动启动时读取）。
驱动会在这里写入 `JackSetupStatus`、`JackOpenStatus`、
`JackReadStatus`、`JackLevel`、`JackConnected`、`JackEdges`、`JackChanges`
和 `JackNotifies`；csaudiork3x 在 SAI1 适配器的键下写入 `JackConnected`、`JackReports`、
`JackEvents` 和 `JackTopologyRegistered`。

## 还没做的

- 耳机插拔检测已经写好（见上文），还没在板子上运行过。
- 硬件音量：Windows 的音量在软件里实现，codec 固定在 0 dB。
- 耳麦麦克风（RINPUT）、48 kHz 以外的采样率，以及 16 位格式。
- CM5-IO 的 ES8388。

## 安装

1. 固件：比 0.4.1 新的 edk2-rk3576（见上文）。
2. 已签名的驱动包在 WinPE 介质的 `woa-audio\` 里，旁边是这次构建的测试证书。部署脚本不会注入它们。
   按[驱动](../drivers.zh-CN.md)一页的方法安装：先信任证书，然后对 rk3xi2sbus、csaudiork3x 和 es8323 运行
   `pnputil /add-driver ... /install`。`pl330dma` 已经从 `woa-drivers\` 装好了。CI 工作流的 artifact
   里是未签名的 catalog，装不上。
3. 设备管理器里，DSDT 发布的每个 SAI 会出现一个 "Rockchip SAI Bus"，每个下面有一个 "Rockchip I2S Audio (WDM)"；
   泰山派 3M 上还有 "Everest 8388 Audio Codec (RK3576 SAI)"。
4. 声音设置里会出现 "Rockchip HDMI"（接了带扬声器的 HDMI 显示器时），泰山派 3M 上还有 "Rockchip Headphones" 和
   "Rockchip Microphone"。

2026-11-21 之前，Windows 用 RK3588 I2S-TDM 的寄存器布局驱动 SAI，TX 从未启动（XFER 为 0x3 而不是 0x7，DMACR 为
0x70010 而不是同一块板子上 Linux 的 0xF0110）；csaudiork3x 1.0.0.135 改用 RK3576 SAI 的布局。
