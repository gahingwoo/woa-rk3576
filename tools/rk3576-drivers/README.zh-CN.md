# RK3576 Windows 驱动包

[English](README.md) | 简体中文

RK3576 板子的全部 Windows 驱动打成一个 zip：`rk3576-drivers-arm64-<版本>.zip`，一个版本号，一个安装脚本。
Maskrom Studio 按所选的板子安装它。从 [0.5.0 正式版](https://github.com/gahingwoo/woa-rk3576/releases/tag/0.5.0)下载
（[`ci` 预发布版](https://github.com/gahingwoo/woa-rk3576/releases/tag/ci)里是 `main` 未经测试的构建）。

## 里面有什么

哪个驱动装到哪块板子，由 `drivers.tsv` 决定。简单说：

- 所有板子：eMMC、SD/SDIO、GPIO、I2C、DMA、以太网、SPI、视频解码（VDPU383）、SARADC 按键、
  40-pin UART、LED、音频（SAI 总线、ES8388 codec、HDMI 和 codec 端点）、不需要驱动的设备
  （`rk3576-null`：RK806、引脚控制器、RTC、UART2、PCIe DBI）、GPU（maliwddm）和 NPU（rknpu）。
- 泰山派 3M：另有 GT9271 触摸屏和蓝牙。
- CM5-IO 和 ROCK 4D：另有 RK806 电源键。ROCK 4D：HAT EEPROM。
- 仅 Windows 10 上的 CM5-IO 和 ROCK 4D：NVMe（`rk3xnvme`；自带的 stornvme 在那里会蓝屏）。
  装到 Windows 10 时还会从 `PCI\CC_010802` 的 DriverDatabase 条目中删去 `stornvme.inf`
  （离线用 `win10-nvme.cmd W:\`，在线由 SYSTEM 任务完成），见[它的 README](../../drivers/storage/rk3xnvme/README.md)。
  自己调用 DISM 而不用 `install.cmd` 的工具，要在 DISM 之后运行 `win10-nvme.cmd W:\`。
- 可选（`/optional`）：CM5-IO 的蓝牙（未测试）。

Windows 10（19041 到 19045）和 Windows 11 各装适合自己版本号的驱动：Windows 10 上以太网用
`dwc_eqos10`（NetAdapterCx 2.0），不用 `dwc_eqos`。目标 Windows 版本还没有构建的驱动，`install.cmd` 会逐个说明。

固件没有发布对应设备的驱动不会绑定任何东西。驱动包是测试签名的，`certs\` 里是签名证书。

## 安装

必须打开测试签名（`bcdedit /set testsigning on`）。

装进正在运行的 Windows，在管理员命令提示符里运行，然后重启：

    install.cmd /online rock4d
    shutdown /r /t 0

它只暂存驱动（`pnputil /add-driver`，从不加 `/install`：现在什么都不会启动），
并把没有驱动或驱动比包里旧的现有设备标记为下次启动时重装。它从不标记 SD/eMMC 主控（dwcsdhc、dwcmshc），
因为 Windows 可能正从它们启动；以太网卡只有在没有驱动时才标记。它们的新驱动只暂存，通过离线安装生效。

重启两分钟后，一个 SYSTEM 计划任务（`rk3576-drivers-verify`）检查每个被标记的设备是否用上了新驱动。
没用上的设备会再标记一次，需要再重启一次：没有用户登录时任务自己重启 Windows，有人登录时只提示
（消息、登录时的提示、应用程序事件日志）。之后任务删除自己。日志在 `%ProgramData%\RK3576\drivers\verify-bound.log`。

想先看会标记哪些设备（不做任何改动）：

    powershell -ExecutionPolicy Bypass -File install-online.ps1 -Board rock4d -Plan

装进没有运行的 Windows（在 WinPE 里，`W:` 下有 `W:\Windows`）：

    install.cmd W:\ cm5io

板子写 `tspi3m`、`cm5io` 或 `rock4d`。

## Windows 10 上板检查（ROCK 4D）

还没在 ROCK 4D 上跑过。把包 `install.cmd W:\ rock4d` 装进 19045 镜像后（或 `/online` 再重启）：

1. `pnputil /enum-devices /problem` 里没有 RK3576 设备（代码 10、31、39）。
2. `Get-PnpDevice -PresentOnly | ? Status -ne OK` 除了故意不装驱动的设备外为空。
3. NVMe：磁盘用的是 `rk3xnvme` 而不是 stornvme；来回拷几 GB 不蓝屏。
4. 以太网（`dwc_eqos10`）：拿到 DHCP 地址，能 ping 通网关。
5. 电源键（`rk806pwrkey`）按电源选项生效；LED（`rkled`）闪；`tools\eeprom` 能读 HAT EEPROM（`rkeeprom`）。
6. HDMI 音频和 codec 有声；SARADC 按键和耳机检测可用；`vdpu383test` 能解码；40-pin UART 和 SPI 示例能跑。
7. 前面板 USB 和蓝牙（系统自带驱动）：键盘可用，手机能配对。

第一次启动时设 `AutoReboot=0` 并准备好救援 Windows，蓝屏信息才能看清。

## 构建

`.github/workflows/drivers.yml` 用同一个提交构建并签名本仓库的驱动（`build-woa.ps1`），
从 wddm-mali-g52 和 woa-rk3576-npu 最近一次成功的构建取 maliwddm 和 rknpu，再运行 `assemble.py`；Windows 10 会装到的驱动若要求高于 KMDF 1.31，它会报错（GPU 和 NPU 只警告）。
`test_assemble.py` 测试它；工作流还会在 Windows runner 上为每块板子规划一次在线安装，
并用 `test_online.ps1` 拿虚构的设备跑一遍重启后的检查。
