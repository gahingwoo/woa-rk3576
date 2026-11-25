# rk-monitor

[English](README.md) | 简体中文

RK3576 板子在 Windows on Arm 下用的托盘图标。这些板子能跑的最新版本 Windows 11 23H2，任务管理器里没有 NPU 页，
也不显示温度；这个工具把它们显示出来，旁边还有 CPU 和 GPU 的状态。

图标显示的是最热的 ACPI 热区的温度，单位摄氏度（85 C 起变红）。鼠标悬停提示里有每个簇的 CPU 占用、温度、GPU 占用和
NPU 忙碌度。左键单击打开弹出窗口，每秒刷新一次：

| 分区 | 行 | 数据来源 |
|---|---|---|
| CPU | `A53, cores 0-3`、`A72, cores 4-7`：占用和频率，带 60 秒历史 | `\Processor(*)\% Processor Time`、`\Processor Information(*)\Processor Frequency` |
| CPU | 每个热区一行 | `\Thermal Zone Information(*)\High Precision Temperature`（单位为十分之一开尔文）。不用 WMI 的 `MSAcpi_ThermalZoneTemperature`，它在这些板子上返回的是过时的缓存值 |
| GPU | 利用率，按引擎类型，带历史 | `\GPU Engine(*)\Utilization Percentage`，像任务管理器那样按引擎求和 |
| GPU | 显存 | `\GPU Adapter Memory(*)\Shared Usage` 和 `Dedicated Usage`，所有适配器 |
| GPU | 忙碌度、温度、频率、降频 | 来自 maliwddm 的 `\RK3576 GPU(*)\% busy`、`Temperature (C)`、`Clock (MHz)`、`Throttled` |
| NPU | `core0`、`core1`：忙碌百分比和每秒任务数，带历史 | 来自 rknpu.sys 的 `\RK3576 NPU(*)\% Busy Time`、`Jobs/sec` |

簇的划分来自 Windows 报告的效率等级；如果它对所有核心只报告一个等级，并且共有 8 个核心，就把 0-3 号核心当作
A53，4-7 号当作 A72。

缺少某组计数器不算错误。对应的行会注明（`not reported`、`driver has no counters`），并给出注册它的命令；
rk-monitor 每 30 秒重新查找一次，所以驱动装上或注册完成后它就会用上这些计数器：

- GPU：maliwddm 驱动包（wddm-mali-g52）里的 `maliwddm-counters.cmd install`，每次驱动更新后运行；
- NPU：rknpu 驱动包（woa-rk3576-npu，docs/PERFCOUNTERS.md）里的 `rknpu-perfcounters.ps1`，以管理员身份运行，每次驱动更新后运行。

（内核模式的计数器提供程序要用 lodctr 注册，而 INF 不能运行 lodctr。）

## 桌面小部件

托盘菜单里的 **Desktop widget**（桌面小部件）：桌面上的一个小面板，有四行：CPU（按簇，带图表）、GPU（占用、温度、频率）、NPU
（按核心，带图表）和各个热区。它是承载桌面图标的那个窗口的子窗口，所以总在所有应用程序下面，按 Win+D 或
"show desktop"（显示桌面）之后也还在。它的背景半透明，跟随浅色或深色模式。可以拖到任何地方；位置按用户记住
（如果那台显示器不在了，就回到默认位置，即主屏幕右上角）。**Widget click-through**（小部件点击穿透）让点击落到桌面上；
关掉它之后才能再次移动小部件。在小部件上右键会打开托盘菜单。

Explorer 重启后，小部件会和托盘图标一起回来。如果桌面窗口不能接受子窗口，小部件就退回为一个置底的普通窗口，
这时 Win+D 会把它隐藏。

和 RK3576 工具（[../rk3576-tools](../rk3576-tools/README.zh-CN.md)）一起安装时，小部件默认打开
（`HKLM\Software\RK3576\Monitor`，`Widget` = 1）；每个用户自己的选择保存在 `HKCU\Software\RK3576\Monitor`
（`Widget`、`WidgetClickThrough`、`WidgetX`、`WidgetY`）。

## 使用

它就是一个便携 exe：`rk-monitor.exe`。复制到任意位置运行即可；或者用 RK3576 工具包
（[../rk3576-tools](../rk3576-tools/README.zh-CN.md)）为所有用户安装，这样还会注册 GPU 和 NPU 计数器。
右键图标可以选 **Start with Windows**（随 Windows 启动：在 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下加一项，
指向 exe 当前所在的位置；如果安装程序已经通过登录任务为所有用户启动它，这一项会显示为已勾选并变灰）和 **Exit**（退出）。
再启动一次会打开已在运行的那个实例的弹出窗口。它跟随应用的浅色或深色模式，图标则跟随任务栏的模式。

`rk-monitor.exe --autostart`（登录任务和 Start with Windows 用的就是它）在已有实例运行时什么也不做，
5 秒后取第一个样本，第一分钟以后台优先级运行。

## 耳机

托盘菜单里的 **自动切换耳机**，默认打开：插头插进板子的 3.5 mm 耳机孔时，Rockchip 音频驱动的 "Headphones" 端点
会成为默认播放设备（控制台、多媒体和通信）；拔出时，之前的默认设备（通常是 HDMI）重新成为默认。Windows 本身
不会因为耳机孔事件在两个音频适配器之间切换默认设备。这个功能需要 es8323 和 csaudiork3x 的耳机孔检测
（[docs/dev/audio](../../docs/dev/audio.zh-CN.md)），这样端点才会在未插入和活动之间变化；它只看适配器名包含
"Rockchip" 的 Headphones 端点，从不理会 USB 或蓝牙耳机。启动时如果耳机已经插着，它会切换到耳机。

Windows 没有设置默认设备的公开 API。这里用的是未公开的 `IPolicyConfig` 接口（声音控制面板和 SoundSwitch
之类的工具用的就是它），Microsoft 可能在任何一个版本里改掉它。所以它只在 Windows 版本号 10240 到 26999 上启用
（设置 `AutoSwitchAnyBuild` = 1 可以在其他版本上试），接口创建不了时这一项会变灰，调用出错时会在本次会话中关闭这个功能，
而不是让 rk-monitor 停下。设置项：`AutoSwitchHeadphones`（1/0），以及 `AutoSwitchPrevious`，即要切回去的设备，
由 rk-monitor 自己维护。

`rk-monitor.exe --once` 以文本打印一个样本，接着列出找到了哪些计数器组，再加一行耳机切换的情况（它从不更改默认设备），
然后退出：

```
rk-monitor.exe --once > R:\rk-monitor.txt
```

## 构建

```cmd
msbuild tools\rk-monitor\rk-monitor.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

用 C 和 Win32 写成（PDH、GDI、外壳的通知区域），另有一个 C++ 文件处理音频端点通知（MMDevice API），C 运行时静态链接。
CI 构建 ARM64 和 x64，用 `--once` 运行 x64 版本，并上传 `rk-monitor-arm64` 和 `rk-monitor-x64`。
