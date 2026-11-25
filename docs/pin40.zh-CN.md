# 在 Windows 里使用 40 针排针

[English](pin40.md) | 简体中文

把 40 针排针上的 UART 变成 COM 口，并让应用程序通过 `Windows.Devices.Gpio`、`.I2c`、`.Spi`
使用排针上的 GPIO、I2C 和 SPI（即树莓派 Windows IoT 的做法）。这里的东西都还没有在板子上跑过：
驱动在 CI 里能编译，固件表能编译也能正常反汇编。最后的[测试计划](#测试计划)是接下来要先做的事。

## 需要什么

| 部分 | 在哪里 | 说明 |
|---|---|---|
| 设置项 "40-pin UART (experimental)" | edk2-rk3576 | 默认关闭。把排针上 UART 的 TX/RX 引脚切到 UART，给这些 UART 提供 24 MHz 时钟，并安装一个 SSDT，每个 UART 一个 `RKCP3006` 设备 |
| 设置项 "40-pin GPIO/I2C/SPI for apps (experimental)" | edk2-rk3576 | 默认关闭。把排针上的 I2C 和 SPI 引脚切到这些总线，并安装一个带 `\_SB.RHPX`（`MSFT8000`）的 SSDT |
| [rk3xuart](../drivers/serial/rk3xuart) | 本仓库 | UART 的 SerCx2 驱动。新加的 |
| rk3xgpio、rk3xi2c、[rk3xspi](../drivers/spi/rk3xspi) | 本仓库 | RhProxy 交给应用的控制器。已经随其他驱动一起安装 |
| `rhproxy.sys` | Windows | 系统自带。微软的文档说 Windows IoT 和 Windows 企业版（15063 起）都有；请先确认（见下文） |

两个选项都在 Setup 的 ACPI / Device Tree 页，下次启动生效。需要 ACPI 模式（默认就是）。对使用设备树的
Linux 没有任何作用。

各板子的固件支持：

| 板子 | 分支 | 状态 |
|---|---|---|
| 泰山派 3M | `main` | 两个选项都有 |
| CM5-IO | `cm5io-parity` | 两个选项都有，还没合并到 `main` |
| ROCK 4D | `rock4d-parity` | 两个选项都有，还没合并到 `main` |

调试串口（UART0）在任何板子上都不会成为排针 UART，rk3xuart 遇到它也会拒绝启动。

## 为什么要自己写 UART 驱动

Windows 自带的 `serial.sys` 只支持寄存器间隔 1 字节的 16550。寄存器间隔是编译期常量（微软在
Windows-driver-samples 里公开的 `serial.sys` 源码 `serial/serial/serial.h` 中 `SERIAL_REGISTER_STRIDE`
为 1），寄存器按字节读写，注册表值和 `_DSD` 属性都改变不了。RK3576 的 UART 是 DesignWare APB UART，
32 位寄存器、间隔 4 字节（`rk3576.dtsi` 里 `reg-shift = 2`、`reg-io-width = 4`），还会产生 DesignWare
特有的 "busy detect" 中断，`serial.sys` 不会清它。SPCR/DBG2 里支持 32 位间隔的 "16550 with GAS"
子类型只给内核调试器用，与 `serial.sys` 无关。

所以 rk3xuart 是一个 SerCx2 控制器驱动（PIO，不用 DMA）。固件在每个 UART 的 `_DSD` 里写
`"SerCx-FriendlyName"`，SerCx2 就会发布 `GUID_DEVINTERFACE_COMPORT` 接口（Windows 10 1903 起）：
`Windows.Devices.SerialCommunication` 找的就是它，Win32 程序可以用接口路径打开它。对只认 COM 名的程序，
rk3xuart 还为 UARTn 建立 `COMn`，并写入 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`。详见驱动的
[README](../drivers/serial/rk3xuart/README.zh-CN.md)。

## 引脚表

排针引脚号是板子上的编号。"Windows 引脚" 是应用传给 `GpioController.OpenPin` 的编号：按固件列出的顺序
从 0 开始编号（顺序编号；排针的引脚分布在四个 GPIO 控制器上，不能直接用 SoC 编号）。UART 引脚排在最后，
只在 "40-pin UART" 关闭时才作为 GPIO 出现，所以切换 UART 不会改变其他引脚的编号。表里没有的引脚是
电源、地、未连接，或者板上已经占用。

排针信号都是 3.3 V。固件不改变 GPIO 引脚的方向、电平和上下拉；无论应用选择哪种驱动模式，引脚都保持
复位后的上下拉（RK3576 大多数引脚是下拉），因为 rk3xgpio 不设置上下拉。因此
`GPIO-SupportedDriveModes` 只声明输入和推挽输出。

### 泰山派 3M

来自立创的 40 针功能表（`泰山派3M-RK3576开发板40Pin_v1.3`），并与主线 `rk3576-pinctrl.dtsi` 核对。

| 用途 | 排针引脚 | SoC | Windows 中的名字 |
|---|---|---|---|
| UART4（M0） | TX 8，RX 10 | GPIO2_D0，GPIO2_D1 | 端口 `UART4`，`COM4` |
| UART8（M1） | TX 37，RX 40 | GPIO2_A6，GPIO2_A7 | 端口 `UART8`，`COM8` |
| I2C7（M1） | SDA 3，SCL 5 | GPIO3_A1，GPIO3_A0 | `I2C7`（默认 I2C 总线） |
| I2C8（M2） | SDA 27，SCL 28 | GPIO2_B7，GPIO2_B6 | `I2C8` |
| SPI1（M1） | MOSI 19，MISO 21，SCLK 23，CS0 24，CS1 26 | GPIO2_C2、C3、C5、C4、C1 | `SPI1`，片选 0 和 1 |

| Windows 引脚 | 排针引脚 | SoC | | Windows 引脚 | 排针引脚 | SoC |
|---|---|---|---|---|---|---|
| 0 | 7 | GPIO4_A6 | | 10 | 32 | GPIO2_C7 |
| 1 | 11 | GPIO4_A4 | | 11 | 33 | GPIO4_A7 |
| 2 | 12 | GPIO0_B5 | | 12 | 35 | GPIO0_D2 |
| 3 | 13 | GPIO2_C6 | | 13 | 36 | GPIO2_D7 |
| 4 | 15 | GPIO0_C7 | | 14 | 38 | GPIO3_A2 |
| 5 | 16 | GPIO0_C6 | | 15 | 8（UART 关闭时） | GPIO2_D0 |
| 6 | 18 | GPIO3_A3 | | 16 | 10（UART 关闭时） | GPIO2_D1 |
| 7 | 22 | GPIO2_D6 | | 17 | 37（UART 关闭时） | GPIO2_A6 |
| 8 | 29 | GPIO2_C0 | | 18 | 40（UART 关闭时） | GPIO2_A7 |
| 9 | 31 | GPIO2_B0 | | | | |

控制台在板子单独的调试接口上，不在排针上。

### CM5-IO

来自 ArmSoM CM5-IO V1.1 原理图（第 7 页，40 针连接器 J4）和主线 `rk3576-pinctrl.dtsi`。排针 8 和 10 脚
是 UART0（调试串口），不动。不提供 UART4（M0 选项在 16/18 脚）：CM5 模块的蓝牙在用 UART4。28 脚在焊了
风扇选项电阻时是风扇 PWM，不提供。22 脚是 ADC 输入，不是 GPIO。

| 用途 | 排针引脚 | SoC | Windows 中的名字 |
|---|---|---|---|
| UART2（M1） | RX 3，TX 5 | GPIO4_B4，GPIO4_B5 | 端口 `UART2`，`COM2` |
| I2C4（M3） | SDA 27，SCL 29 | GPIO3_B7，GPIO3_C0 | `I2C4` |
| SPI3（M0） | MOSI 19，MISO 21，SCLK 23，CS0 24 | GPIO3_A1、A2、A0、A3 | `SPI3`，片选 0 |

| Windows 引脚 | 排针引脚 | SoC | 说明 |
|---|---|---|---|
| 0 | 7 | GPIO0_C4 | 也是红外接收的信号线 |
| 1 | 16 | GPIO2_D1 | 红色 LED |
| 2 | 18 | GPIO2_D0 | 绿色 LED（固件的状态灯） |
| 3 | 3（UART 关闭时） | GPIO4_B4 | |
| 4 | 5（UART 关闭时） | GPIO4_B5 | |

### ROCK 4D

来自 Radxa 的 ROCK 4D 40 针表（radxa-docs，`rock4d/hardware-use/40-pin-gpio`）和主线
`rk3576-pinctrl.dtsi`。8 和 10 脚是 UART0（调试串口）。27/28 脚是 I2C6，板上 EEPROM 在 0x50（主线
`&i2c6`），所以作为 I2C 总线提供，不作为 GPIO。

| 用途 | 排针引脚 | SoC | Windows 中的名字 |
|---|---|---|---|
| UART4（M1） | TX 11，RX 15 | GPIO1_C4，GPIO1_C5 | 端口 `UART4`，`COM4` |
| UART7（M0） | TX 16，RX 18 | GPIO2_B6，GPIO2_B7 | 端口 `UART7`，`COM7` |
| I2C8（M1） | SDA 3，SCL 5 | GPIO1_C7，GPIO1_C6 | `I2C8`（默认 I2C 总线） |
| I2C6（M3） | SDA 27，SCL 28 | GPIO4_C7，GPIO4_C6 | `I2C6`（EEPROM 在 0x50） |
| SPI1（M0） | MOSI 19，MISO 21，SCLK 23，CS0 24，CS1 26 | GPIO1_B5、B6、B4、B7、GPIO1_C0 | `SPI1`，片选 0 和 1 |

| Windows 引脚 | 排针引脚 | SoC | | Windows 引脚 | 排针引脚 | SoC |
|---|---|---|---|---|---|---|
| 0 | 7 | GPIO1_C3 | | 9 | 36 | GPIO1_D4 |
| 1 | 12 | GPIO1_D1 | | 10 | 37 | GPIO3_A3 |
| 2 | 13 | GPIO2_C0 | | 11 | 38 | GPIO1_D3 |
| 3 | 22 | GPIO2_D7 | | 12 | 40 | GPIO1_D0 |
| 4 | 29 | GPIO3_A2 | | 13 | 11（UART 关闭时） | GPIO1_C4 |
| 5 | 31 | GPIO1_C1 | | 14 | 15（UART 关闭时） | GPIO1_C5 |
| 6 | 32 | GPIO1_D5 | | 15 | 16（UART 关闭时） | GPIO2_B6 |
| 7 | 33 | GPIO1_C2 | | 16 | 18（UART 关闭时） | GPIO2_B7 |
| 8 | 35 | GPIO1_D2 | | | | |

## 安装

1. 在固件 Setup 的 ACPI / Device Tree 页打开 "40-pin UART"、"40-pin GPIO/I2C/SPI for apps"，或两个都开。
   保存并重启。
2. 用 CI 产物（`rk3576-woa-uart-arm64`，或 WinPE 工作流放到介质上的签名包）安装 rk3xuart，方法和其他
   测试签名驱动一样（[INSTALL.zh-CN.md](INSTALL.zh-CN.md)）：`pnputil /add-driver rk3xuart.inf /install`。
3. 确认系统里有 rhproxy：`reg query HKLM\SYSTEM\CurrentControlSet\Services\rhproxy` 和
   `dir C:\Windows\INF\rhproxy.inf`。之后设备管理器的系统设备下应该出现已启动的
   "Resource Hub proxy device"。问题代码 51 表示它用到的某个控制器（GPIO、I2C 或 SPI）没有启动。

## 使用

UART，把 TX 接到 RX：

```
serialtest list
serialtest UART4 --sweep
serialtest COM4 --baud 115200 --bytes 65536
```

[serialtest](../tools/serialtest) 在 CI 产物 `rk3576-woa-serialtest-arm64` 里。任何接受 COM 名的终端程序也能用。

GPIO、I2C 和 SPI：在 Windows PowerShell 5.1（`powershell.exe`，不是 PowerShell 7，后者加载不了 WinRT
类型）里运行 [tools/pin40](../tools/pin40) 的脚本：

```
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -List
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 1 -Count 10
powershell -ExecutionPolicy Bypass -File i2c-scan.ps1 -Bus I2C7
```

微软的 GpioTestTool、I2cTestTool、SpiTestTool 和 MinComm（Windows-iotcore-samples 的 `BusTools`）
也可以用同样的名字。

## 限制

- 波特率由 24 MHz UART 时钟除以 16 再除以整数得到，误差超过 3% 的会被拒绝：1200 到 115200 以及
  500000、750000、1500000 可用；230400、460800、921600 不行（误差 7%、8.5%、18%）。时钟由固件决定
  （`_DSD` 里的 `clock-frequency`），以后的固件可以提高它，驱动不用改。
- 没有流控、没有 modem 线：排针上只有 TX 和 RX。硬件流控和 XON/XOFF 会被拒绝；DTR 和 RTS 可以设置、
  可以读回，但不驱动任何东西；CTS 和 DSR 读出来是有效。
- 没有 DMA：UART 用中断驱动，64 字节 FIFO，驱动里有 4 KiB 接收缓冲。排针上常用的速率够用。
- SPI：只支持 8 位帧，模式 0 到 3，3.1 kHz 到 50 MHz（rk3xspi）。
- 没有运行时引脚复用：引脚功能在开机时由两个设置项决定。打开 "40-pin GPIO/I2C/SPI for apps" 后，
  I2C 和 SPI 引脚是总线，不是 GPIO。

## 测试计划

以上都还没有在硬件上跑过。按这个顺序，一次一块板子（先泰山派 3M：它的固件支持在 `main` 上）：

1. 两个选项都关：启动，确认什么都没变（设备管理器；固件日志里没有 `Pin40:` 行）。
2. 打开 "40-pin UART"：固件日志有 `Pin40: 2 UART(s)` 和 `AcpiPlatform: 40-pin UART on`。Windows 里出现
   `RKCP3006` 设备，装上 rk3xuart 后启动。`serialtest list` 显示 `UART4`、`UART8` 接口和 `COM4`/`COM8`。
   用跳线把 TX 接到 RX，`serialtest UART4 --sweep` 通过；然后接电脑上的 USB 串口适配器，115200 双向收发。
3. 打开 "40-pin GPIO/I2C/SPI for apps"：`\_SB.RHPX` 启动（有没有 rhproxy？没有的话记下是哪个版本的
   Windows）。`gpio-blink.ps1 -List` 显示 19 个引脚（UART 选项打开时 15 个）。接在排针 7 脚上的 LED 或
   万用表跟随 `gpio-blink.ps1 -Pin 0` 变化。`i2c-scan.ps1 -Bus I2C7` 找到接在 3/5 脚上的已知 I2C 设备。
   SPI1 上把 MOSI 接到 MISO，用 SpiTestTool 做回环。
4. 两个都开：GPIO 列表里没有 8、10、37、40 脚，UART 正常。
5. 然后在各自分支上测 CM5-IO 和 ROCK 4D，步骤相同；在 CM5-IO 上确认两个选项都打开时 8/10 脚上的调试串口
   照常工作。
