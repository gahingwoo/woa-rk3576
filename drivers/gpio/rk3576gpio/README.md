# rk3576gpio — RK3576 GPIO controller (Windows on ARM)

A GpioClx miniport for the Rockchip RK3576 "GPIO v2" controller. It binds to the
five GPIO banks (`ACPI\RKCP3002`) published by the EDK2 RK3576 DSDT and exposes
them to Windows so that `GpioIo` / `GpioInt` resources from other ACPI devices
(I²C touch, buttons, card-detect, regulators, …) resolve correctly.

## Why a custom driver

Windows on ARM has no inbox driver for Rockchip GPIO. The OS GPIO stack
(`msgpioclx.sys`, *GpioClx*) handles PnP, power, interrupt connection, and the
`GpioIo`/`GpioInt` resource hub, but it needs a hardware-specific **miniport**
to actually poke the controller's registers. This is that miniport.

## Hardware model

RK3576 has 5 banks × 32 pins = 160 GPIOs. Each bank is a separate ACPI device
with its own MMIO window and one GIC SPI:

| Bank  | Base        | GIC SPI | ACPI GSIV | Power domain |
|-------|-------------|---------|-----------|--------------|
| GPIO0 | 0x27320000  | 153     | 185       | PMU          |
| GPIO1 | 0x2AE10000  | 157     | 189       | bus          |
| GPIO2 | 0x2AE20000  | 161     | 193       | bus          |
| GPIO3 | 0x2AE30000  | 165     | 197       | bus          |
| GPIO4 | 0x2AE40000  | 169     | 201       | bus          |

All five use the same v2 register layout. The driver is base-address agnostic —
it maps whatever MMIO the ACPI `_CRS` hands it and works for any bank, so one
binary serves all five instances.

### v2 register quirk

Each logical 32-bit register is split into two 16-bit halves (`+0x0` = pins
0–15, `+0x4` = pins 16–31). Writes carry a per-bit *write-enable* mask in the
upper 16 bits, so a pin can be updated without read-modify-write. All of this is
encapsulated in [rk3576gpio_regs.h](rk3576gpio_regs.h)
(`RkGpioWrite32` / `RkGpioRead32` / `RkGpioWriteBit`).

## Files

| File | Purpose |
|------|---------|
| [driver.c](driver.c) | `DriverEntry`, WDF device-add, GpioClx client registration |
| [controller.c](controller.c) | All `CLIENT_*` callbacks (read/write/connect/interrupt) |
| [rk3576gpio.h](rk3576gpio.h) | Device context, prototypes, logging |
| [rk3576gpio_regs.h](rk3576gpio_regs.h) | v2 register map + MMIO accessors |
| [rk3576gpio.inf](rk3576gpio.inf) | Install info, binds `ACPI\RKCP3002` |
| [rk3576gpio.vcxproj](rk3576gpio.vcxproj) / [build.cmd](build.cmd) | EWDK build |

## Status / limitations

- **Pin mux & pull configuration are not handled here.** IOMUX and pull-up/down
  live in the GRF / pinctrl block (0x26040000), a separate device. v1 assumes
  firmware has already muxed referenced pins to GPIO. A pinctrl driver (or
  `_DSD` pull hints consumed at `ConnectIoPins`) is future work.
- **Debounce**: native HW debounce is wired to the `DebounceTimeout` parameter
  (a non-zero timeout enables it for the pin). The debounce *period* uses a
  coarse fixed divider rather than mapping the exact requested timeout.
- **Built and bench-tested?** The source is authored against the documented
  GpioClx ABI but has **not** been compiled with a WDK or run on silicon yet.
  See [../../../docs/BUILDING.md](../../../docs/BUILDING.md). The GpioClx struct
  field names / helper signatures are owned by the WDK's `gpioclx.h`; if a name
  differs in your WDK it will surface as a compile error at exactly that line.

## Quick test once installed

`devcon`/`pnputil` should show five `Rockchip RK3576 GPIO Controller` devices
started (one per bank). A downstream device with a `GpioInt`/`GpioIo` resource
pointing at `\_SB.GPIx` should then enumerate without a yellow-bang resource
error in Device Manager.
