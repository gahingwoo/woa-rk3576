# The 40-pin header from Windows

English | [简体中文](pin40.zh-CN.md)

The 40-pin header's UARTs as COM ports, and its GPIO pins, I2C buses and
SPI bus for applications (`Windows.Devices.Gpio`, `.I2c`, `.Spi`, the
Raspberry Pi Windows IoT model). Status: CI only on every board; the
[test plan](#test-plan) is what runs first.

## What it takes

| Part | Where | Notes |
|---|---|---|
| Setup option "40-pin UART (experimental)" | edk2-rk3576 | off by default. Switches the header's UART TX/RX pins to their UARTs, clocks them at 24 MHz and installs an SSDT with one `RKCP3006` device per UART |
| Setup option "40-pin GPIO/I2C/SPI for apps (experimental)" | edk2-rk3576 | off by default. Switches the header's I2C and SPI pins to those buses and installs an SSDT with `\_SB.RHPX` (`MSFT8000`) |
| [rk3xuart](../drivers/serial/rk3xuart) | this repository | SerCx2 driver for the UARTs. New |
| rk3xgpio, rk3xi2c, [rk3xspi](../drivers/spi/rk3xspi) | this repository | the controllers RhProxy hands to applications. Already installed with the other drivers |
| `rhproxy.sys` | Windows | inbox. Microsoft documents it for Windows IoT and Windows Enterprise (build 15063 on); check that it is there (below) |

Both options are under Setup, ACPI / Device Tree, and take effect at the
next boot. They need ACPI mode (the default). They do nothing for Linux with
a device tree.

Firmware support per board, edk2-rk3576 0.5.0 or later:

| Board | Status |
|---|---|
| TaishanPi 3M | both options |
| CM5-IO | both options, not tried on the board |
| ROCK 4D | both options, not tried on the board |

The debug console UART (UART0) is never one of the header UARTs here, on
any board, and rk3xuart refuses to start on it.

## Why a UART driver of our own

The inbox `serial.sys` only handles 16550 registers one byte apart, and does
not clear the DesignWare busy-detect interrupt; the RK3576 UARTs have 32-bit
registers four bytes apart. rk3xuart is a SerCx2 driver (PIO, no DMA). It
publishes a `GUID_DEVINTERFACE_COMPORT` interface (from the firmware's
`"SerCx-FriendlyName"`), which `Windows.Devices.SerialCommunication` finds,
and also creates `COMn` for UARTn in `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`.
Details: the driver's [README](../drivers/serial/rk3xuart/README.md).

## Pin maps

Header pin numbers are the board's. "Windows pin" is the number an
application passes to `GpioController.OpenPin`: pins are numbered in the
order the firmware lists them, from 0 (sequential numbering; the header's
pins sit on four GPIO controllers, so the SoC numbers cannot be used). The
UART pins come last and exist as GPIOs only while "40-pin UART" is off, so
switching the UARTs does not renumber the other pins. Pins not listed are
power, ground, not connected, or used on the board.

All header signals are 3.3 V. The firmware does not change a GPIO pin's
direction, level or pull; a pin keeps the pull it comes out of reset with (a
pull-down on most RK3576 pins), whichever drive mode an application selects,
because rk3xgpio does not program pulls. `GPIO-SupportedDriveModes` is
therefore input and push-pull output only.

### TaishanPi 3M

From LCKFB's 40-pin table (`泰山派3M-RK3576开发板40Pin_v1.3`), checked against
mainline `rk3576-pinctrl.dtsi`.

| Use | Header pins | SoC | Name in Windows |
|---|---|---|---|
| UART4 (M0) | TX 8, RX 10 | GPIO2_D0, GPIO2_D1 | port `UART4` and a COM name |
| UART8 (M1) | TX 37, RX 40 | GPIO2_A6, GPIO2_A7 | port `UART8` and a COM name |
| I2C7 (M1) | SDA 3, SCL 5 | GPIO3_A1, GPIO3_A0 | `I2C7` (the default I2C bus) |
| I2C8 (M2) | SDA 27, SCL 28 | GPIO2_B7, GPIO2_B6 | `I2C8` |
| SPI1 (M1) | MOSI 19, MISO 21, SCLK 23, CS0 24, CS1 26 | GPIO2_C2, C3, C5, C4, C1 | `SPI1`, chip selects 0 and 1 |

| Windows pin | Header pin | SoC | | Windows pin | Header pin | SoC |
|---|---|---|---|---|---|---|
| 0 | 7 | GPIO4_A6 | | 10 | 32 | GPIO2_C7 |
| 1 | 11 | GPIO4_A4 | | 11 | 33 | GPIO4_A7 |
| 2 | 12 | GPIO0_B5 | | 12 | 35 | GPIO0_D2 |
| 3 | 13 | GPIO2_C6 | | 13 | 36 | GPIO2_D7 |
| 4 | 15 | GPIO0_C7 | | 14 | 38 | GPIO3_A2 |
| 5 | 16 | GPIO0_C6 | | 15 | 8 (UART off) | GPIO2_D0 |
| 6 | 18 | GPIO3_A3 | | 16 | 10 (UART off) | GPIO2_D1 |
| 7 | 22 | GPIO2_D6 | | 17 | 37 (UART off) | GPIO2_A6 |
| 8 | 29 | GPIO2_C0 | | 18 | 40 (UART off) | GPIO2_A7 |
| 9 | 31 | GPIO2_B0 | | | | |

The console is on the board's separate debug connector, not on the header.

### CM5-IO

From ArmSoM's CM5-IO V1.1 schematic (sheet 7, 40-pin connector J4) and
mainline `rk3576-pinctrl.dtsi`. Header pins 8 and 10 carry UART0, the debug
console, and are left alone. UART4 (pins 16/18 in its M0 option) is not
offered: the CM5 module's Bluetooth uses UART4. Pin 28 is the fan's PWM when
the board's fan option resistor is fitted, and is left out. Pin 22 is an ADC
input, not a GPIO.

| Use | Header pins | SoC | Name in Windows |
|---|---|---|---|
| UART2 (M1) | RX 3, TX 5 | GPIO4_B4, GPIO4_B5 | port `UART2` and a COM name |
| I2C4 (M3) | SDA 27, SCL 29 | GPIO3_B7, GPIO3_C0 | `I2C4` |
| SPI3 (M0) | MOSI 19, MISO 21, SCLK 23, CS0 24 | GPIO3_A1, A2, A0, A3 | `SPI3`, chip select 0 |

| Windows pin | Header pin | SoC | Note |
|---|---|---|---|
| 0 | 7 | GPIO0_C4 | also the IR receiver's line |
| 1 | 16 | GPIO2_D1 | the red LED |
| 2 | 18 | GPIO2_D0 | the green LED (the firmware's status LED) |
| 3 | 3 (UART off) | GPIO4_B4 | |
| 4 | 5 (UART off) | GPIO4_B5 | |

### ROCK 4D

From Radxa's ROCK 4D 40-pin table (radxa-docs, `rock4d/hardware-use/40-pin-gpio`)
and mainline `rk3576-pinctrl.dtsi`. Pins 8 and 10 are UART0, the debug
console. Pins 27/28 are I2C6 with the board's EEPROM at 0x50 (mainline
`&i2c6`), so they are offered as an I2C bus, not as GPIOs.

| Use | Header pins | SoC | Name in Windows |
|---|---|---|---|
| UART4 (M1) | TX 11, RX 15 | GPIO1_C4, GPIO1_C5 | port `UART4` and a COM name |
| UART7 (M0) | TX 16, RX 18 | GPIO2_B6, GPIO2_B7 | port `UART7` and a COM name |
| I2C8 (M1) | SDA 3, SCL 5 | GPIO1_C7, GPIO1_C6 | `I2C8` (the default I2C bus) |
| I2C6 (M3) | SDA 27, SCL 28 | GPIO4_C7, GPIO4_C6 | `I2C6` (EEPROM at 0x50) |
| SPI1 (M0) | MOSI 19, MISO 21, SCLK 23, CS0 24, CS1 26 | GPIO1_B5, B6, B4, B7, GPIO1_C0 | `SPI1`, chip selects 0 and 1 |

| Windows pin | Header pin | SoC | | Windows pin | Header pin | SoC |
|---|---|---|---|---|---|---|
| 0 | 7 | GPIO1_C3 | | 9 | 36 | GPIO1_D4 |
| 1 | 12 | GPIO1_D1 | | 10 | 37 | GPIO3_A3 |
| 2 | 13 | GPIO2_C0 | | 11 | 38 | GPIO1_D3 |
| 3 | 22 | GPIO2_D7 | | 12 | 40 | GPIO1_D0 |
| 4 | 29 | GPIO3_A2 | | 13 | 11 (UART off) | GPIO1_C4 |
| 5 | 31 | GPIO1_C1 | | 14 | 15 (UART off) | GPIO1_C5 |
| 6 | 32 | GPIO1_D5 | | 15 | 16 (UART off) | GPIO2_B6 |
| 7 | 33 | GPIO1_C2 | | 16 | 18 (UART off) | GPIO2_B7 |
| 8 | 35 | GPIO1_D2 | | | | |

## Install

1. In the firmware's Setup, ACPI / Device Tree: turn on "40-pin UART",
   "40-pin GPIO/I2C/SPI for apps", or both. Save and restart.
2. Install rk3xuart from the CI artifact (`rk3576-woa-uart-arm64`, or the
   signed packages the WinPE workflow puts on its media) the same way as the
   other test-signed drivers ([INSTALL.md](INSTALL.md)):
   `pnputil /add-driver rk3xuart.inf` (stage only), then restart.
3. Check rhproxy is on the system:
   `reg query HKLM\SYSTEM\CurrentControlSet\Services\rhproxy` and
   `dir C:\Windows\INF\rhproxy.inf`. Device Manager should then show
   "Resource Hub proxy device" under System devices, started. Problem code
   51 means one of the controllers it names (GPIO, I2C or SPI) has not
   started.

## Use

UART, with TX wired to RX:

```
serialtest list
serialtest UART4 --sweep
serialtest COM5 --baud 115200 --bytes 65536
```

Windows picks each UART's COM number (one no other device holds, so not
necessarily COM4 for UART4); `serialtest list` shows it.

[serialtest](../tools/serialtest) is in the CI artifact `rk3576-woa-serialtest-arm64`.
Any terminal program that takes a COM name works too.

GPIO, I2C and SPI, from Windows PowerShell 5.1 (`powershell.exe`, not
PowerShell 7, which cannot load WinRT types), with the scripts in
[tools/pin40](../tools/pin40):

```
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -List
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 1 -Count 10
powershell -ExecutionPolicy Bypass -File i2c-scan.ps1 -Bus I2C7
```

Microsoft's GpioTestTool, I2cTestTool, SpiTestTool and MinComm
(Windows-iotcore-samples, `BusTools`) work against the same names.

## Limits

- Baud rates come from a 24 MHz UART clock divided by 16 and a whole
  number, and anything more than 3 % off is refused: 1200 to 115200 and
  500000, 750000, 1500000 work; 230400, 460800 and 921600 do not (7 %, 8.5 %
  and 18 % off). The clock is the firmware's (`clock-frequency` in `_DSD`);
  a later firmware can raise it without a driver change.
- No flow control and no modem lines: the header has TX and RX only.
  Hardware or XON/XOFF flow control is refused; DTR and RTS can be set and
  read back but drive nothing; CTS and DSR read as asserted.
- No DMA: the UARTs are interrupt driven, 64-byte FIFOs, a 4 KiB receive
  buffer in the driver. Fine for the usual header speeds.
- SPI: 8-bit frames only, modes 0 to 3, 3.1 kHz to 50 MHz (rk3xspi).
- No runtime pin muxing: a pin's function is fixed at boot by the two setup
  options. With "40-pin GPIO/I2C/SPI for apps" on, the I2C and SPI pins are
  buses, not GPIOs.

## Test plan

Nothing above has run on hardware. In this order, one board at a time
(TaishanPi 3M first: its firmware support is on `main`):

1. Both options off: boot, and check nothing changed (Device Manager, the
   firmware log has no `Pin40:` lines).
2. "40-pin UART" on: the firmware log shows `Pin40: 2 UART(s)` and
   `AcpiPlatform: 40-pin UART on`. In Windows, `RKCP3006` devices appear;
   after installing rk3xuart they start. `serialtest list` shows `UART4` and
   `UART8` interfaces and their COM names. With a jumper from TX to RX,
   `serialtest UART4 --sweep` passes; then a USB serial adapter on a PC,
   both directions, at 115200.
3. "40-pin GPIO/I2C/SPI for apps" on: `\_SB.RHPX` starts (rhproxy present?
   if not, the edition lacks it: record which edition). `gpio-blink.ps1
   -List` shows 19 pins (15 with the UART option on). An LED or a meter on
   header pin 7 follows `gpio-blink.ps1 -Pin 0`. `i2c-scan.ps1 -Bus I2C7`
   finds a known I2C device on pins 3/5. SpiTestTool loopback with MOSI
   wired to MISO on SPI1.
4. Both on: pins 8, 10, 37, 40 are gone from the GPIO list, the UARTs work.
5. Then the CM5-IO (on its branch) and the ROCK 4D, the same steps; on the
   CM5-IO, check the debug console on pins 8/10 keeps working with both
   options on.
