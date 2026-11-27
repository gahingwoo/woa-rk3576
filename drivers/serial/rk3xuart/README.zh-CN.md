# rk3xuart：40 针排针上的 RK3576 UART（SerCx2）

[English](README.md) | 简体中文

RK3576 DesignWare APB UART 的 SerCx2 控制器驱动，用于板子 40 针排针上的 UART。它绑定
**`ACPI\RKCP3006`**，edk2-rk3576 只有在设置项 "40-pin UART (experimental)" 打开时才声明这个设备
（`Uart40.asl`，SSDT `RKUART40`）。每块板子是哪几个 UART、怎么用：[docs/pin40.zh-CN.md](../../../docs/pin40.zh-CN.md)。

**只在 CI 里编译过，没有在硬件上跑过。**

## 为什么不用 serial.sys

`serial.sys` 只支持寄存器间隔 1 字节的 16550：间隔是编译期常量（微软公开的 `serial.sys` 源码里
`SERIAL_REGISTER_STRIDE` 为 1），寄存器按字节访问；注册表和 `_DSD` 都改不了。这些 UART 的寄存器是
32 位、间隔 4 字节，还会产生 DesignWare 特有的 busy-detect 中断（IIR = 7），`serial.sys` 不会清它。

## 做了什么

- **端口名。** 固件的 `"SerCx-FriendlyName"`（`UART4` 等）让 SerCx2 发布 `GUID_DEVINTERFACE_COMPORT`
  接口：`Windows.Devices.SerialCommunication` 能列出它，Win32 程序用接口路径打开。每个端口还有一个 COM 名
  （`\DosDevices\COMn` 和 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` 里的一项），给只认 COM 名的程序用。编号由
  Windows 的 COM 名仲裁（ComDB）分配，不会占用别的设备已有的编号（比如泰山派 3M 上 EC20 模块的 COM4）：
  设备属于 Ports 类，它的类安装程序认领一个空闲编号，存为设备硬件键
  （`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3006\<n>\Device Parameters`）里的 `PortName`。没有存好的
  名字时，驱动自己认领：UARTn 优先用 COMn，被占用就用最小的空闲编号，并存下来。改 `PortName` 可以改名，
  空字符串表示不建 COM 名。存好的名字被别的驱动占用时，这次启动先用一个空闲编号，日志里会写明。
- **普通用户可访问**：用户可以打开端口（SDDL `BU` 读写），做法和微软 SerCx2 接口文档一致。
- **数据通路**：PIO。ISR 把接收 FIFO 搬到 4 KiB 环形缓冲区，把发送空转成 "ready"；DPC 通知 SerCx2。
  Drain 每毫秒查一次 `LSR.TEMT`。
- **线路参数**：波特率按 `_DSD` 里的 `clock-frequency` 计算（固件给 24 MHz，误差超过 3% 拒绝），
  5 到 8 个数据位，1、1.5 或 2 个停止位，无/奇/偶/mark/space 校验，break。写 LCR 前等 `USR.BUSY`
  清零，线路一直不空闲时清掉接收 FIFO。
- **Modem 线**：排针上没有接。DTR/RTS 可设置可读回，CTS 和 DSR 读出来是有效，流控会被拒绝
  （`STATUS_NOT_SUPPORTED`），`CTS`/`DSR`/`RLSD`/`RING` 等待事件接受但永远不触发。
- **UART0**（固件控制台、调试器端口）不管表里怎么写，启动时都会拒绝。

## 文件

| 文件 | 用途 |
|---|---|
| [driver.c](driver.c) | DriverEntry、添加设备、SerCx2 和 PIO 对象、资源、`_DSD`、COM 名 |
| [uart.c](uart.c) | 寄存器、线路设置、ISR/DPC、PIO 收发回调 |
| [ioctl.c](ioctl.c) | SerCx2 下发的串口 IOCTL、等待掩码、清空、打开/关闭 |
| [rk3xuart.inf](rk3xuart.inf) | `ACPI\RKCP3006`，服务依赖 SerCx2 |

## 测试

[tools/serialtest](../../../tools/serialtest)：把 TX 接到 RX，运行 `serialtest UART4 --sweep`。
完整计划见 [docs/pin40.zh-CN.md](../../../docs/pin40.zh-CN.md#测试计划)。
