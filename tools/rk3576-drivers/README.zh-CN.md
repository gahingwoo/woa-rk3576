# RK3576 Windows 驱动包

[English](README.md) | 简体中文

RK3576 板子的全部 Windows 驱动打成一个 zip：`rk3576-drivers-arm64.zip`，一个版本号，一个安装脚本。
Maskrom Studio 按所选的板子安装它。CI 构建放在
[`ci` 预发布版](https://github.com/gahingwoo/woa-rk3576/releases/tag/ci)，没有在板子上测过。

## 里面有什么

哪个驱动装到哪块板子，由 `drivers.tsv` 决定。简单说：

- 所有板子：eMMC、SD/SDIO、GPIO、I2C、DMA、以太网、SPI、视频解码（VDPU383）、SARADC 按键、
  40-pin UART、LED、音频、GPU（maliwddm）和 NPU（rknpu）。
- 泰山派 3M：另有 GT9271 触摸屏和蓝牙。
- CM5-IO 和 ROCK 4D：另有 RK806 电源键。ROCK 4D：HAT EEPROM。
- 可选（`/optional`）：CM5-IO 的蓝牙（未测试）。

固件没有发布对应设备的驱动不会绑定任何东西。

驱动包是测试签名的，`certs\` 里是签名证书。`manifest.json` 写明每个驱动来自哪次构建。

## 安装

必须打开测试签名（`bcdedit /set testsigning on`）。

装进正在运行的 Windows，在管理员命令提示符里：

    install.cmd /online tspi3m

装进没有运行的 Windows（在 WinPE 里，`W:` 下有 `W:\Windows`）：

    install.cmd W:\ cm5io

板子写 `tspi3m`、`cm5io` 或 `rock4d`。装完重启。

## 构建

`.github/workflows/drivers.yml` 用 WinPE 工作流、wddm-mali-g52 和 woa-rk3576-npu 最近一次成功的构建运行 `assemble.py`。
