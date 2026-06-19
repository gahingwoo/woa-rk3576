# rk3xi2c — Rockchip rk3x I2C controller (Windows on ARM)

An SpbCx controller driver for the Rockchip **rk3x** I2C IP (device-tree
`rockchip,rk3576-i2c` / `rockchip,rk3399-i2c`), published as `ACPI\RKCP3001`.
It exposes the RK3576 I2C buses to Windows so peripherals with an `I2cSerialBus`
connection in their ACPI `_CRS` (PMIC RK806, RTC, EEPROM, sensors, touch) work.

> This is **not** a DesignWare I2C controller. The rk3x register map and
> transfer engine are Rockchip's own; do not reuse a DW-APB-I2C driver here.

## Hardware model

Master engine with mode select in `REG_CON` (TX / RX / register-TX / register-RX),
a 32-byte TX FIFO at `0x100` and RX FIFO at `0x200`, and a `START`/`STOP` +
`MTXCNT`/`MRXCNT` handshake. RK3576 exposes I2C0..I2C9:

| Bus  | Base       | GIC SPI | ACPI GSIV |
|------|------------|---------|-----------|
| I2C0 | 0x27300000 | 88      | 120       |
| I2C1 | 0x2AC40000 | 89      | 121       |
| …    | …          | …       | …         |
| I2C9 | 0x2AE80000 | 97      | 129       |

One driver binary serves every bus; each `RKCP3001` instance maps its own MMIO
from ACPI `_CRS`.

## Design (v1)

- **Polled transfer engine** (`hw.c`): hardware interrupts stay disabled and
  `REG_IPD` is polled. SpbCx dispatches I/O sequentially at `PASSIVE_LEVEL`, so
  this is simple and race-free. Interrupt-driven operation is a later optimization.
- **SpbCx glue** (`spb.c`): parses each target's 7-bit address + bus speed from
  the I2cSerialBus descriptor; routes Read / Write / Sequence to the engine.
  A Sequence runs as one transaction (`START` … repeated `START` between
  messages … `STOP`) — the classic *write register offset, then read*.

## Files

| File | Purpose |
|------|---------|
| [hw.c](hw.c) | rk3x transfer engine (verified vs. `i2c-rk3x.c`) + `CLKDIV` calc |
| [spb.c](spb.c) | SpbCx callbacks: target connect, read, write, sequence |
| [driver.c](driver.c) | `DriverEntry`, SpbCx init, MMIO map in PrepareHardware |
| [rk3xi2c.h](rk3xi2c.h) | contexts, prototypes, tuning constants |
| [rk3xi2c_regs.h](rk3xi2c_regs.h) | register map + accessors |
| [rk3xi2c.inf](rk3xi2c.inf) | binds `ACPI\RKCP3001` |

## Status / limitations

- **Input clock is assumed, not read.** SCL divider math uses
  `RK3XI2C_INPUT_CLK_HZ` (200 MHz default, biased high so the bus never runs
  *faster* than requested). Once the EDK2 port exports the real I2C functional
  clock via `_DSD`, read it instead. See [rk3xi2c.h](rk3xi2c.h).
- **7-bit addressing only.** 10-bit targets are rejected at connect.
- **No clock-stretch tuning / hold-time `REG_CON` tuning bits** — defaults are
  used; fine for standard/fast mode on typical devices.
- **Not yet compiled with a WDK or run on silicon.** The rk3x engine is verified
  against the kernel driver; the SpbCx integration is modeled on Microsoft's
  *SkeletonI2C* sample. SpbCx struct/field names are owned by the WDK headers
  (`SPBCx.h`, `reshub.h`) — any mismatch surfaces as a compile error at that line.
  See [../../../docs/BUILDING.md](../../../docs/BUILDING.md).

## Quick test once installed

Device Manager → *System devices* should show *Rockchip rk3x I2C Controller*
nodes started. An I2C peripheral declared in ACPI against `\_SB.I2Cx` should
then enumerate; verify register reads against a known device (e.g. the RTC or
PMIC chip-id register) with an SPB test tool.
