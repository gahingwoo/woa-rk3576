# bcm4345bt：泰山派 3M 的蓝牙（Windows on ARM）

[English](README.md) | 简体中文

板上 Ampak AP6256 模块的蓝牙部分（接在 UART6 上的 Broadcom BCM4345C5），给 Windows 蓝牙协议栈使用。
这是一个 KMDF 总线驱动：它自己驱动 UART，给控制器上电，加载 patchram，再把 HCI 链路交给系统自带的 BthMini。

| 板子 | 状态 |
|---|---|
| 泰山派 3M（`RKCP4345`） | 已在 Windows 11 23H2 上板测试：配对和文件传输可用；没有语音（SCO），没有空闲省电 |
| CM5-IO（`RKCP4362`） | 仅 CI |

## 绑定的设备

`ACPI\RKCP4345`，由泰山派 3M 固件（比 0.4.1 新的 edk2-rk3576）声明为 `\_SB.BTH0`，资源依次是：

- `Memory32Fixed (0x2ad90000, 0x100)`：UART6 的寄存器；
- `Interrupt { 114 }`：UART6，高电平触发；
- `\_SB.GPI1` 上的 `GpioIo { 23 }`：BT_REG_ON（GPIO1 PC7）；
- `\_SB.GPI1` 上的 `GpioIo { 28 }`：BT_WAKE（GPIO1 PD4）。

所以它依赖 [rk3xgpio](../../gpio/rk3xgpio)。固件负责 UART6 引脚（GPIO2 PD2 到 PD5）的复用，并把 UART 时钟设为 24 MHz；
旧固件里根本没有 `RKCP4345` 这个设备。

### CM5-IO：`ACPI\RKCP4362`

ArmSoM CM5 模组上的 BW3752-50B1 是一颗 BCM43752，Linux 的 btbcm 把它的蓝牙内核叫作 BCM4362A2。
CM5-IO 的固件把它声明为 `\_SB.BTH0`，HID 为
`RKCP4362`，四项资源和顺序都一样：UART4（`0x2ad70000`，`Interrupt { 112 }`）、BT_REG_ON 为
GPIO1 PC7、BT_WAKE 为 GPIO1 PD4，和泰山派 3M 用的 GPIO 相同（ArmSoM-CM5-1V1 原理图第 17 页）。
固件把 UART4 M1（GPIO1 PC2 到 PC5）切好复用，时钟设为 24 MHz。INF 把它绑到同一个驱动，
`FirmwarePath` 设为 `BCM4362A2.hcd`；用 `install-firmware.ps1 -Chip 4362a2` 安装这个文件
（80602 字节，armbian/firmware `db8cc456`，Ampak 的版本，没有给出许可）。还没在板子上试过：
切到 1500000 波特率，以及这块模组有没有自己的蓝牙地址（驱动只替换 BCM4345C5 的占位地址）。

## 工作方式

```
ACPI\RKCP4345          bcm4345bt.sys  (this driver, class System)
  BCM4345BT\UART_H4    BthMini.sys    (in-box bth.inf, compatible ID MS_BTHX_BTHMINI)
    BTHENUM\...        the Windows Bluetooth stack and profiles
```

- **为什么由驱动自己驱动 UART。** Windows on ARM 没有 RK3576 的 DesignWare UART 驱动：22621 的 ARM64 映像里有
  SerCx 和 SerCx2 类扩展，但没有 `serial.sys`，也没有 16550 控制器驱动；而且 `serial.sys` 本来也处理不了 4 字节的寄存器间隔。
  所以没有可以挂靠的 COM 口，微软的 SerialHCIBus 模型（架在 SerCx2 UART 上的蓝牙总线驱动）也无处可放。
  本驱动自己实现 8N1、FIFO、硬件 RTS/CTS 以及中断驱动的收发（`uart.c`）。
- **初始化**（`hci.c`），按 Broadcom UART 控制器的要求，和 Linux 的 `hci_bcm` 驱动做法一样：BT_REG_ON 和 BT_WAKE 拉高，等 100 ms；
  在 115200 波特率下发 HCI_Reset、Read Local Version、Read Local Name；发 Download Minidriver，再发 `.hcd` 的每一条记录，等 250 ms；
  再发一次 HCI_Reset；用厂商命令 0xFC18 切到 1500000 波特率，UART 随之切换。如果 Read BD_ADDR 返回的是 BCM4345C5 的占位地址
  （43:45:C5:00:1F:AC），就用 0xFC01 写入一个每块板子自己的地址：优先用注册表里的 `BdAddress`，没有就随机生成一个并存进去。
- **BthX**（`hci.c`、`pdo.c`）：一个兼容 ID 为 `MS_BTHX_BTHMINI` 的子设备；系统自带的 `bth.inf` 会把 BthMini 绑定到它上面
  （已在 22621 ARM64 映像里核对过）。它的 IOCTL 转发给本驱动，本驱动把写入的数据封装成 H4，并把接收的数据流解析成事件和 ACL。
- **没有语音，但声明了 HCI bypass。** `ScoSupport` 设为 `ScoSupportHCIBypass`，带一个 SCO 通道。系统自带的 BthMini（22621）
  只接受这一种：在 `IOCTL_BTHX_QUERY_CAPABILITIES` 之后，除非 `MaxAclTransferInSize` >= 0x103、`ScoSupport` == 2 且
  `MaxScoChannels` == 1，否则它启动失败，返回 `STATUS_DEVICE_CONFIGURATION_ERROR`（0xC0000182，`BCM4345BT\UART_H4` 上显示代码 10）
  （这是从 BthMini.sys 里读出来的）。bypass 的意思是 SCO 音频
  应该走模块的 PCM 引脚（这块板子上是 SAI2），而这条路径没有音频驱动：数据类协议（A2DP 播放、HID、文件传输）可用，
  通话和耳机麦克风不能用。
- **不休眠。** 驱动处于 D0 时控制器一直上电（`IsDeviceIdleCapable` 为 FALSE）。没有使用 HOST_WAKE。

## patchram

控制器需要 `BCM4345C5.hcd`，这是 Broadcom 的固件，本仓库不带，驱动包里也没有。没有它时控制器运行 ROM 里的固件：
驱动照常继续，并记录 `FirmwareLoaded = 0`。

[install-firmware.ps1](install-firmware.ps1) 在板子上从一个固定的提交下载它，并校验 SHA-256：

| `-Source` | 文件 | 许可 |
|---|---|---|
| `ampak`（默认） | Ampak 为 AP6256 做的版本，"BCM4345C5 Ampak_CL1 UART 37.4 MHz BT 5.0"，45667 字节，来自 armbian/firmware | 没有写明；Linux 在这块板子上加载的就是这个文件 |
| `rpi` | 树莓派为 Pi 400 做的 BCM43456 版本，55676 字节，来自 RPi-Distro/bluez-firmware | Cypress 许可：允许以目标代码形式再分发，用于 Cypress 芯片 |

两者都不在 linux-firmware 里。两者都按模块的 37.4 MHz 晶振做的，这块板子上用的正是这个晶振（原理图里是 SX1B37.400F1210F30）。

它放在 `%SystemRoot%\System32\drivers\brcm\BCM4345C5.hcd`；注册表值 `FirmwarePath`（NT 路径）可以改变位置。

## 安装

1. 固件要带 `\_SB.BTH0`（比 0.4.1 新的 edk2-rk3576），并已装好 rk3xgpio。替换已经在运行的驱动，见
   [docs/drivers.zh-CN.md](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/drivers.zh-CN.md)。
2. 以管理员身份运行 `powershell -ExecutionPolicy Bypass -File install-firmware.ps1`。
3. 信任驱动包里的 `signer.cer`（Root 和 TrustedPublisher，和其他驱动一样，见 [INSTALL](../../../docs/INSTALL.zh-CN.md)）。
   签名好的驱动包是 WinPE workflow 的 `bcm4345bt-signed-arm64` 构建产物。
4. 不论首次安装还是替换已经装好的版本，都用 `pnputil /add-driver bcm4345bt.inf`（不加 `/install`）暂存，
   再重启：在运行中的系统上停掉本驱动曾导致 DPC_WATCHDOG_VIOLATION。

## 查看发生了什么

在设备的硬件键下（`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP4345\0\Device Parameters`）：

| 值 | 含义 |
|---|---|
| `ResetStatus` | 第一次在 115200 下发 HCI_Reset 的 NTSTATUS。不为 0：控制器没有应答（电源、32 kHz 时钟、UART 引脚） |
| `RomVersion`, `PatchedVersion` | 加载 patchram 前后的 (HCI revision << 16) \| LMP subversion |
| `ChipName` | 控制器 Read Local Name 的结果 |
| `FirmwareLoaded` | patchram 已发送且控制器恢复应答时为 1 |
| `Baud` | 最终的波特率 |
| `AddressFromChip` | 占位地址被替换时为 0 |
| `BringUpStatus` | 整个初始化过程的 NTSTATUS；只有它为 0 时才会有子设备（也就才会有蓝牙） |
| `ChildCreated` | `BCM4345BT\UART_H4` 被枚举后为 1 |
| `BthX<Name>Count`, `Status`, `Lens`, `Done` | 每个 BthX IOCTL（`GetVersion`、`SetVersion`、`QueryCaps`、`WriteHci`、`ReadHci`、`Other`）各一组：调用次数、最后一次的 NTSTATUS（0x103 = 挂起）、(输入长度 << 16) \| 输出长度、挂起后完成的次数 |
| `BthXOtherIoctl` | 最后一个没人处理的 IOCTL 代码（bit 31 置位：子设备上的内部 IOCTL） |
| `BthXSetVersion`, `BthXLastCommand`, `BthXLastEvent` | BthMini 选择的版本；它写入的最后一个 HCI opcode；它读到的最后一个事件的 (事件代码 << 16) \| opcode |
| `BthXCapsSco`, `BthXCapsScoChannels`, `BthXCapsMaxAcl`, `BthXCapsIdle` | QUERY_CAPABILITIES 的应答内容 |

覆盖设置：同一个键下的 REG_DWORD，存的是值加一（0 表示“未设置”）：`BthXScoSupport`、`BthXMaxScoChannels`、`BthXIdleCapable`。

驱动还会用 `DbgPrintEx` 记录每一步（组件 IHVDRIVER，前缀 `bcm4345bt:`）。

## 已知不足

- 32 kHz 睡眠时钟来自 HYM8563 RTC 的 CLKOUT，它有电池供电，Linux 的 `rtc-hym8563` 可能会把它关掉。Windows 这边没有任何东西会把它打开。
  如果某块板子在 Linux 下蓝牙正常，而这里 `ResetStatus` 不为 0，先检查 CLKOUT（寄存器 0x0D 的 bit 7）。
- 控制器只有一种速度：1500000 波特率，即设备树里的 `max-speed`，24 MHz 的 UART 时钟可以整除它。3000000 需要 48 MHz 时钟。
- Windows 设置里的蓝牙开关由 BthMini 处理；驱动不会因此给模块断电。

## 许可

GPL-2.0-only。依据 16550 和 DesignWare UART 的寄存器模型、Linux `btbcm`/`hci_bcm` 驱动所用的 Broadcom HCI 厂商命令，
以及微软的 BthX 文档和 `bthxddi.h` 编写。微软的 SerialHCIBus 示例是 MS-PL 许可，不能和 GPL 代码合并；
只是读它来了解 BthX 接口的形态，没有使用其中任何代码。
