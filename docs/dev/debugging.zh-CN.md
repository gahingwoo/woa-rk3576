# 从这块板子上的 Windows 取出数据

[English](debugging.md) | 简体中文

这里的 Windows 没有串口控制台：SAC 需要 `sacdrv.sys`，而 WinPE 里没有它；SPCR 只是标出一个调试端口。下面的方法要么把结果写到
U 盘上，之后在 Linux 下读取，要么通过内核调试器传出来。

## 在 WinPE 中：`collect.cmd`

本仓库的 WinPE 映像在启动时运行 `tools/woa-debug/collect.cmd`，把结果写到 U 盘的 `woa-debug\out\`。用的是
Windows 安装介质时，U 盘根目录的 `autounattend.xml` 会在安装程序第一个界面出现之前运行同一个脚本。

`autounattend.xml` 里绝对不能加 `<DiskConfiguration>` 或 `<ImageInstall>` 段。任何一个都会让安装程序进入无人值守模式，
而无人值守的安装程序会自己重新分区磁盘，在这里就是 NVMe 和 eMMC 上的固件。

| 文件 | 回答的问题 |
|---|---|
| `10-devices.txt` | Windows 枚举到的每个设备，以及是否绑定了驱动 |
| `11-enum-acpi.txt`, `12-enum-pci.txt` | 设备树中 ACPI 和 PCI 的分支 |
| `20-diskpart.txt` | 磁盘和卷 |
| `30-setupact.log`, `31-setuperr.log` | 从安装介质运行时，安装程序的日志 |
| `40-drivers.txt` | 这个 WinPE 中暂存的驱动包 |
| `60-resourcemap.txt` | 仲裁器分配出去的每个中断和内存范围 |
| `61-hw-description.txt` | Windows 根据 ACPI 表建立的内容，包括固件的构建标记 |
| `62-acpi-tables.txt` | 加载了哪些 ACPI 表 |
| `63-problem-devices.txt` | 带问题代码的设备 |

放进 `woa-debug\drivers\<name>\` 的驱动包会用 `drvload` 加载，之后再取一次设备列表。

## 在已安装的系统中：`net-debug` 和 `usb-debug`

插着 U 盘，在管理员命令提示符里运行。每个脚本都会在 U 盘的 `woa-debug\` 下写一个带时间戳的文件夹。

- `net-debug.cmd`：`dwc_eqos` 的 ETW trace、带各层计数的 pktmon 抓包、一次 DHCP 尝试和一次静态 IP 的 ARP 测试、
  适配器属性和事件日志。它还会关闭快速启动。`dwc_eqos` 在 `Get-NetAdapterStatistics` 里无论如何都报 0，所以要看
  pktmon 的数据。
- `net-update.cmd`：用 U 盘上的 `dwc_eqos` 替换已安装的版本。只用 `pnputil /add-driver` 会保留 DISM 装进去的那份；
  这个脚本会删除其他所有版本，然后由你重启。
- `usb-debug.cmd`：在重启 `XHC0` 的过程中抓取 USBXHCI、UCX、USBHUB3 和 PnP 的 trace，并记录每个 xHCI 控制器的问题代码和状态。

`dwc_eqos` 用 TraceLogging 记录日志。Windows 的 `tracerpt` 显示不出这些事件名；在 Linux 上用 `etl-parser`
这个 Python 包可以。USB 的 provider 基于 manifest，`tracerpt` 能解码。

## 通过串口使用内核调试器

调试端口是 UART0，地址 `0x2AD40000`，也就是固件的控制台。无论 BCD 怎么写，Windows 都以 1500000 波特率运行 KDCOM。
edk2-rk3576 里的 `scripts/kd-listen.py` 实现了足够多的协议，能让启动继续下去，并记录每次模块加载和任何
`*** Fatal System Error`。

如果 BCD 里是 `debug on` 而没有程序在监听，Windows 启动会非常慢。用完后用 `bcdedit /debug off` 关掉。

## 分析资源分配失败

`CM_PROB_NORMAL_CONFLICT` 表示仲裁器无法满足某个设备。三个来源分别回答三个不同的问题：

- Enum 转储里的 `LogConf\BasicConfigVector`：设备能接受什么。
- `LogConf\BootConfig`：固件留给它的是什么。对 PCI 设备来说，就是 UEFI 编程的 BAR，ARM64 Windows 会保留它们。
- `60-resourcemap.txt`：实际分配给所有设备的是什么。

这些二进制列表里的描述符是 20 字节，不是结构体看起来的 16 字节。`reg query` 把每个值打印在一行上，所以按行号挑出它；
并且要先去掉值名，再只保留十六进制字符：`BootConfig` 和 `REG_RESOURCE_LIST` 里也有十六进制字母。
