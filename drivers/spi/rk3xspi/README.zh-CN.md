# rk3xspi：Rockchip SPI 控制器（Windows on ARM）

[English](README.md) | 简体中文

Rockchip SPI 主控制器（设备树 `rockchip,rk3576-spi` / `rockchip,rk3066-spi`）的 SpbCx 控制器驱动。
结构和 [I²C 驱动](../../i2c/rk3xi2c) 一样是 SpbCx；SPI 多了全双工。

## ACPI

RK3576 有 SPI0..SPI4（0x2ACF0000、0x2AD00000、0x2AD10000、0x2AD20000、0x2AD30000；GIC SPI 116 到 120，GSIV 148 到 152）。
INF 绑定 **`ACPI\RKCP3003`**，这是 edk2-rk3576 的 `Spi.asl` 给这些控制器的 ID，另有一个 `_CID` 保留 Linux 的 PRP0001 匹配。
SFC0 是 SPI-NOR 闪存控制器，留给 UEFI；本驱动不处理它。

## 设计（v1）

- **统一的全双工（TR）引擎**，在 [hw.c](hw.c) 里：每次传输都打 N 个帧的时钟，同时发送 TX、接收 RX。读操作发送全零并保留 RX；
  写操作发送 TX 并丢弃 RX；全双工两者都做。已对照 `drivers/spi/spi-rockchip.c` 核对。
- **轮询方式**，TX 和 RX 的差距限制在 FIFO 深度以内，避免 RX 溢出。
- **片选通过 SER 控制**，整个传输期间保持有效。
- **序列会被合并**成一个片选保持不变的全双工事务（写段变成 TX 字节，读段变成发送全零并采集 RX），
  先写后读的 SPI 设备期望的片选行为正是这样。
- **全双工** `IOCTL_SPB_FULL_DUPLEX` 在 `EvtSpbControllerIoOther` 里处理。

## 文件

| 文件 | 用途 |
|------|---------|
| [hw.c](hw.c) | rockchip SPI 引擎（已核对）+ BAUDR 计算 |
| [spb.c](spb.c) | SpbCx 回调：连接（解析 SpiSerialBus）、读/写/序列/全双工 |
| [driver.c](driver.c) | DriverEntry、SpbCx 初始化、MMIO 映射 |
| [rk3xspi_regs.h](rk3xspi_regs.h) | 寄存器映射和位定义 |

## 状态和限制

- **只支持 8 位帧**（DataBitLength 必须为 8）；其他位宽会被拒绝。
- 输入时钟假定为 `RK3XSPI_INPUT_CLK_HZ`（200 MHz，来自 `_DSD`，取偏保守的值，保证总线永不超频）；等 `_DSD` 单独导出后再从中读取。
- 轮询方式，没有 DMA。对一般的 SPI 外设流量够用。
- **还没有用 WDK 编译过，也没在芯片上跑过。** SPI 引擎已对照内核核对；SpbCx 的结构体和字段名来自 WDK 头文件
  （`SPBCx.h`、`reshub.h`），编译时会检查。
