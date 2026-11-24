# gt9271：Goodix GT9271 触摸屏（Windows on ARM）

[English](README.md) | 简体中文

立创为泰山派 3M 做的 10.1 英寸 800x1280 MIPI DSI 屏的触摸控制器。这是一个 KMDF 驱动，通过 I2C 读取控制器，
再通过 Virtual HID Framework（VHF）把它作为 HID 触摸屏交给 Windows。

## 绑定的设备

`ACPI\RKCP9271`，由泰山派 3M 固件（edk2-rk3576）声明为 `\_SB.I2C0.TCH0`：

- `I2cSerialBusV2 (0x5d, 400 kHz, \_SB.I2C0)`，所以依赖 [rk3xi2c](../../i2c/rk3xi2c)；
- `\_SB.GPI0` 上的 `GpioInt (Edge, ActiveLow, Exclusive, PullUp) { 21 }`（GPIO0_C5），所以依赖 [rk3xgpio](../../gpio/rk3xgpio)。

固件只有在启动时找到了控制器才不会返回 `_STA = 0`，所以没接屏时 Windows 完全看不到触摸设备。
用旧固件时也没有这个设备。这个设备默认是隐藏的：DSDT 里是 `Name (TCHP, 0)`，`TCHP` 为 1 之前 `_STA` 都是 0。固件的 TouchKeysDxe 在 ReadyToBoot 时把已安装 DSDT 里的这一个字节改成 1，而且只在探测到控制器有应答时才改（edk2-rk3576 `8a526ab`）；没有应答时，设备保持隐藏。

## 工作方式

```
ACPI\RKCP9271   gt9271.sys  (this driver; class HIDClass)
                vhf.sys     (in-box lower filter, from the INF)
  HID\...       hidclass + in-box touch stack: "HID-compliant touch screen"
```

- **I2C**：先以大端序发送寄存器地址，再读取，作为一个 SPB 序列完成。状态在 0x814E（bit 7 表示缓冲区就绪，bits 3:0 是触点数），
  触点数据从 0x814F 开始，每个 8 字节。每帧读完后把 0x814E 写回 0；不写的话控制器不会再填充缓冲区。除此之外从不写任何东西：
  配置区（0x8047 起）只读，用于写日志。
- **中断**：下降沿，这是控制器的出厂配置。ISR 运行在 passive 级别（`PassiveHandling`），自己完成 I2C 读取。
  手指按下期间，驱动还会每 100 ms 查看一次控制器，免得漏掉一个边沿导致触点卡住；但它从不因为超时而抬起一个触点。
  `_CRS` 里没有中断时，每 10 ms 轮询一次。
- **不做复位**。GT9xx 的复位时序还会根据中断线的电平选择 I2C 地址；固件让控制器以 0x5d 地址运行，本驱动就用这个状态。
- **HID**：一个 Digitizer / Touch Screen 集合。输入报告 1 有十个手指集合（Tip Switch、Contact Identifier、X 0..799、Y 0..1279，
  物理尺寸 135.6 x 217.0 mm），外加 Scan Time 和 Contact Count。特性报告 2 是 Contact Count Maximum = 10。没有认证（THQA）报告：
  从 Windows 10 起它是可选的，没有它时 Windows 显示 "Windows Touch Support" 而不是 "Full Windows Touch Support"。
- **抬起**：某个触点从一帧中消失时，会在它最后的位置再发送一次，tip switch 清零。一帧最多带十个触点；
  如果抬起的加上新出现的会超过十个，就先把抬起的单独放在一帧里发出去。

坐标是面板原生的竖屏坐标，这块屏上 Windows 桌面也是这个方向。[README](../../../README.zh-CN.md) 里的截图显示桌面转成横屏后报告了 10 个触点；触摸坐标是否跟着旋转，没有记录。

## 状态

**在泰山派 3M 上可用**（ci 固件）：支持十点触摸（[README](../../../README.zh-CN.md#状态)、[硬件功能](../../../docs/features.zh-CN.md#触摸泰山派-3mci-固件)）。如果它启动不了，日志行（`DbgPrintEx`，IHV driver 组件）以 `gt9271:` 开头。

依据 Goodix GT9xx 寄存器说明和微软的 VHF、SPB 及触摸屏 HID 文档编写。没有使用 Linux goodix 驱动的代码。
