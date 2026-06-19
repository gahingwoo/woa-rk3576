# rk3xspi — Rockchip SPI controller (Windows on ARM)

An SpbCx controller driver for the Rockchip SPI master (device-tree
`rockchip,rk3576-spi` / `rockchip,rk3066-spi`). Same SpbCx shape as the
[I²C driver](../../i2c/rk3xi2c); SPI adds full-duplex.

## ACPI prerequisite

RK3576 exposes SPI0..SPI4 (0x2ACF0000, 0x2AD00000, 0x2AD10000, 0x2AD20000,
0x2AD30000; GIC SPI 116–120 → GSIV 148–152). In the current EDK2 DSDT they are
published with `_HID "PRP0001"` — a Linux-only enumerator Windows ignores. This
INF binds **`ACPI\RKCP3003`**, so the EDK2 `Spi.asl` must give the controllers
that `_HID` (matching GPIO=`RKCP3002`, I²C=`RKCP3001`). The EDK2 port is also
part of this project, so that `_HID` change is being made there (**WIP**). (SFC0
is the SPI-NOR flash controller and is intentionally left to UEFI — not handled
by this driver.)

## Design (v1)

- **Unified full-duplex (TR) engine** in [hw.c](hw.c): every transfer clocks N
  frames, sending TX and receiving RX simultaneously. Read clocks zeros and
  keeps RX; Write sends TX and drops RX; full-duplex does both. Verified against
  `drivers/spi/spi-rockchip.c`.
- **Polled**, with the TX/RX gap bounded to the FIFO depth to avoid RX overflow.
- **CS via SER**, asserted for the whole transfer.
- **Sequences are coalesced** into one CS-held full-duplex transaction (write
  segments → TX bytes, read segments → clock zeros and capture RX), which is how
  write-then-read SPI devices expect chip-select to behave.
- **Full-duplex** `IOCTL_SPB_FULL_DUPLEX` is handled in `EvtSpbControllerIoOther`.

## Files

| File | Purpose |
|------|---------|
| [hw.c](hw.c) | rockchip SPI engine (verified) + BAUDR calc |
| [spb.c](spb.c) | SpbCx callbacks: connect (parse SpiSerialBus), read/write/sequence/full-duplex |
| [driver.c](driver.c) | DriverEntry, SpbCx init, MMIO map |
| [rk3xspi_regs.h](rk3xspi_regs.h) | register map + bits |

## Status / limitations

- **8-bit frames only** (DataBitLength must be 8); other widths rejected.
- Input clock assumed `RK3XSPI_INPUT_CLK_HZ` (200 MHz, from `_DSD`, biased so
  the bus never overclocks); read from `_DSD` once exported discretely.
- Polled; no DMA. Fine for typical SPI peripheral traffic.
- **Not yet compiled with a WDK or run on silicon.** The SPI engine is
  kernel-verified; SpbCx struct/field names come from the WDK headers
  (`SPBCx.h`, `reshub.h`) and are checked at compile time.
