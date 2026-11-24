# gt9271: Goodix GT9271 touch screen (Windows on ARM)

English | [简体中文](README.zh-CN.md)

The touch controller of LCKFB's 10.1 inch 800x1280 MIPI DSI screen for the
TaishanPi 3M. A KMDF driver that reads the controller over I2C and presents
it to Windows as a HID touch screen through the Virtual HID Framework (VHF).

## What it binds

`ACPI\RKCP9271`, declared by the TaishanPi 3M firmware (edk2-rk3576) as
`\_SB.I2C0.TCH0`:

- `I2cSerialBusV2 (0x5d, 400 kHz, \_SB.I2C0)`, so it needs
  [rk3xi2c](../../i2c/rk3xi2c);
- `GpioInt (Edge, ActiveLow, Exclusive, PullUp) { 21 }` on `\_SB.GPI0`
  (GPIO0_C5), so it needs [rk3xgpio](../../gpio/rk3xgpio).

The firmware returns `_STA = 0` unless it found the controller at boot, so
with no screen attached Windows sees no touch device at all. With an older
firmware there is no device either. The device is hidden by default: the
DSDT has `Name (TCHP, 0)` and `_STA` is 0 until `TCHP` is 1. The firmware's
TouchKeysDxe changes that one byte in the installed DSDT to 1 at ReadyToBoot,
and only when the controller answered its probe (edk2-rk3576 `8a526ab`); if
it does not, the device stays hidden.

## How it works

```
ACPI\RKCP9271   gt9271.sys  (this driver; class HIDClass)
                vhf.sys     (in-box lower filter, from the INF)
  HID\...       hidclass + in-box touch stack: "HID-compliant touch screen"
```

- **I2C**: register address sent big-endian, then the read, as one SPB
  sequence. Status at 0x814E (bit 7 buffer ready, bits 3:0 points), points
  from 0x814F, 8 bytes each. After every frame 0x814E is written back to 0;
  the controller does not refill its buffer until then. Nothing else is ever
  written: the configuration area (0x8047 onward) is only read, for the log.
- **Interrupt**: falling edge, the controller's factory configuration. The
  ISR runs at passive level (`PassiveHandling`) and does the I2C itself.
  While a finger is down the driver also looks at the controller every
  100 ms, so that a missed edge cannot leave a contact stuck; it never lifts
  a contact on a timeout. With no interrupt in `_CRS` it polls every 10 ms.
- **No reset**. The GT9xx reset sequence also selects the I2C address from
  the level of the interrupt line; the firmware leaves the controller
  running at 0x5d and that is the state this driver uses.
- **HID**: one Digitizer / Touch Screen collection. Input report 1 has ten
  finger collections (Tip Switch, Contact Identifier, X 0..799, Y 0..1279,
  physical 135.6 x 217.0 mm) plus Scan Time and Contact Count. Feature report
  2 is Contact Count Maximum = 10. No certification (THQA) report: optional
  since Windows 10, and without it Windows shows "Windows Touch Support"
  rather than "Full Windows Touch Support".
- **Lifts**: a contact that disappears from a frame is sent once more with
  the tip switch clear at its last position. A frame never carries more than
  ten contacts; when lifts plus new contacts would, the lifts go first in a
  frame of their own.

Coordinates are the panel's native portrait ones, which is the orientation
the Windows desktop has on this screen. The [README](../../../README.md)'s screenshot shows the
desktop rotated to landscape with 10 touch points reported; whether the touch
coordinates follow the rotation is not recorded.

## Status

**Works on the TaishanPi 3M** with the ci firmware: ten touch points
([README](../../../README.md#status), [features](../../../docs/features.md#touch-taishanpi-3m-ci-firmware)).
If it does not start, the log
lines (`DbgPrintEx`, IHV driver component) start with `gt9271:`.

Written from the Goodix GT9xx register description and Microsoft's VHF, SPB
and touch screen HID documentation. No code from the Linux goodix driver.
