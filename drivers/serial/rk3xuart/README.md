# rk3xuart: RK3576 UARTs on the 40-pin header (SerCx2)

English | [简体中文](README.zh-CN.md)

A SerCx2 controller driver for the RK3576's DesignWare APB UARTs, for the
UARTs on a board's 40-pin header. It binds **`ACPI\RKCP3006`**, which
edk2-rk3576 declares only when its "40-pin UART (experimental)" setup option
is on (`Uart40.asl`, SSDT `RKUART40`). Which UARTs that is, per board, and
how to use them: [docs/pin40.md](../../../docs/pin40.md).

**Compiled in CI only. Not run on hardware.**

## Why not serial.sys

`serial.sys` takes 16550 registers one byte apart: the stride is a
compile-time constant (`SERIAL_REGISTER_STRIDE`, 1, in the `serial.sys`
source Microsoft publishes) and the registers are accessed a byte at a time;
nothing in the registry or in `_DSD` changes that. These UARTs have 32-bit
registers four bytes apart and raise the DesignWare busy-detect interrupt
(IIR = 7), which `serial.sys` does not clear.

## What it does

- **Port names.** The firmware's `"SerCx-FriendlyName"` (`UART4`, ...) makes
  SerCx2 publish a `GUID_DEVINTERFACE_COMPORT` interface:
  `Windows.Devices.SerialCommunication` lists it, and Win32 programs open
  its interface path. Each port also gets a COM name, for programs that take
  a COM name only: `\DosDevices\COMn` and an entry in
  `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`. The number comes from Windows' COM
  name arbiter (ComDB), so it never takes one another device holds (the
  EC20 modem's COM4 on the TaishanPi 3M): the device is in the Ports class,
  whose installer claims a free number and stores it as `PortName` in the
  device's hardware key
  (`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3006\<n>\Device Parameters`).
  Without a stored name the driver claims COMn for UARTn if it is free, else
  the lowest free number, and stores it. Editing `PortName` renames the
  port; an empty one drops the COM name. If the stored name is in use by
  another driver, the port takes a free one for that boot and the log says
  so.
- **Unprivileged access**: users may open the port (SDDL `BU` read/write),
  as Microsoft's SerCx2 interface page shows.
- **Data path**: PIO. The ISR moves the receive FIFO into a 4 KiB ring and
  turns transmit-empty into "ready"; the DPC tells SerCx2. Drain polls
  `LSR.TEMT` every millisecond.
- **Line settings**: baud from `clock-frequency` in `_DSD` (24 MHz from the
  firmware; refused if more than 3 % off), 5 to 8 data bits, 1, 1.5 or 2
  stop bits, none/odd/even/mark/space parity, break. LCR writes wait for
  `USR.BUSY` to clear, flushing the receive FIFO if the line never goes idle.
- **Modem lines**: not wired on the header. DTR/RTS set and read back, CTS
  and DSR read as asserted, flow control is refused (`STATUS_NOT_SUPPORTED`),
  `CTS`/`DSR`/`RLSD`/`RING` wait events are accepted and never raised.
- **UART0** (the firmware console and the debugger's port) is refused at
  start, whatever the tables say.

## Files

| File | Purpose |
|---|---|
| [driver.c](driver.c) | DriverEntry, device add, SerCx2 and PIO objects, resources, `_DSD`, COM name |
| [uart.c](uart.c) | registers, line setup, ISR/DPC, PIO receive/transmit callbacks |
| [ioctl.c](ioctl.c) | serial IOCTLs SerCx2 passes down, wait mask, purge, open/close |
| [rk3xuart.inf](rk3xuart.inf) | `ACPI\RKCP3006`, service depends on SerCx2 |

## Test

[tools/serialtest](../../../tools/serialtest): wire TX to RX and run
`serialtest UART4 --sweep`. The rest of the plan is in
[docs/pin40.md](../../../docs/pin40.md#test-plan).
