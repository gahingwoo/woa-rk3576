# bcm4345bt: Bluetooth on the TaishanPi 3M (Windows on ARM)

The Bluetooth half of the board's Ampak AP6256 module, a Broadcom BCM4345C5
on UART6, for the Windows Bluetooth stack. A KMDF bus driver that runs the
UART itself, powers the controller, loads its patchram and hands the HCI
link to the in-box BthMini.

**Not run on hardware.** It builds in CI; nothing more is known.

## What it binds

`ACPI\RKCP4345`, declared by the TaishanPi 3M firmware (edk2-rk3576,
branch `tspi3m-bt-acpi`) as `\_SB.BTH0`, with in this order:

- `Memory32Fixed (0x2ad90000, 0x100)`: UART6's registers;
- `Interrupt { 114 }`: UART6, level high;
- `GpioIo { 23 }` on `\_SB.GPI1`: BT_REG_ON (GPIO1 PC7);
- `GpioIo { 28 }` on `\_SB.GPI1`: BT_WAKE (GPIO1 PD4).

So it needs [rk3xgpio](../../gpio/rk3xgpio). The firmware muxes the UART6
pins (GPIO2 PD2 to PD5) and sets the UART clock to 24 MHz; with an older
firmware there is no `RKCP4345` device at all.

## How it works

```
ACPI\RKCP4345          bcm4345bt.sys  (this driver, class System)
  BCM4345BT\UART_H4    BthMini.sys    (in-box bth.inf, compatible ID MS_BTHX_BTHMINI)
    BTHENUM\...        the Windows Bluetooth stack and profiles
```

- **Why the driver runs the UART.** Windows on ARM ships no driver for the
  RK3576's DesignWare UART: the 22621 ARM64 image has the SerCx and SerCx2
  class extensions but no `serial.sys` and no 16550 controller driver, and
  `serial.sys` would not handle the 4-byte register stride anyway. So there
  is no COM port to stack on, and Microsoft's SerialHCIBus model (a
  Bluetooth bus driver over a SerCx2 UART) has nothing to sit on. This
  driver does 8N1, FIFOs, hardware RTS/CTS and interrupt-driven receive and
  transmit itself (`uart.c`).
- **Bring-up** (`hci.c`), as Broadcom UART controllers need it and as the
  Linux `hci_bcm` driver does: BT_REG_ON and BT_WAKE high, 100 ms; at
  115200 baud HCI_Reset, Read Local Version, Read Local Name; Download
  Minidriver, every record of the `.hcd`, 250 ms; HCI_Reset again; vendor
  command 0xFC18 to 1500000 baud and the UART follows. If Read BD_ADDR
  returns the BCM4345C5 placeholder (43:45:C5:00:1F:AC) a per-board address
  is written with 0xFC01: `BdAddress` from the registry, else a random one
  that is then stored there.
- **BthX** (`hci.c`, `pdo.c`): one child with the compatible ID
  `MS_BTHX_BTHMINI`; the in-box `bth.inf` binds BthMini to it (checked in
  the 22621 ARM64 image). Its IOCTLs are forwarded to this driver, which
  frames writes as H4 and parses the receive stream into events and ACL.
- **No voice.** `ScoSupport` is `ScoSupportNone`: the module's SCO audio
  goes to its PCM pins (SAI2 on this board), and there is no audio driver
  for that path. Data profiles (A2DP playback, HID, file transfer) are what
  this offers; a headset's microphone and calls will not work.
- **No sleep.** The controller stays powered while the driver is in D0
  (`IsDeviceIdleCapable` FALSE). HOST_WAKE is not used.

## The patchram

The controller needs `BCM4345C5.hcd`, Broadcom firmware that this
repository does not carry and its packages do not include. Without it the
controller runs its ROM firmware: the driver goes on and records
`FirmwareLoaded = 0`.

[install-firmware.ps1](install-firmware.ps1) downloads it on the board,
from a pinned commit, and checks its SHA-256:

| `-Source` | File | Licence |
|---|---|---|
| `ampak` (default) | Ampak's build for the AP6256, "BCM4345C5 Ampak_CL1 UART 37.4 MHz BT 5.0", 45667 bytes, from armbian/firmware | none given; the file Linux loads on this board |
| `rpi` | Raspberry Pi's BCM43456 build for the Pi 400, 55676 bytes, from RPi-Distro/bluez-firmware | Cypress licence: redistribution in object code for use with Cypress parts |

Neither is in linux-firmware. Both use the module's 37.4 MHz crystal, which
is the one on this board (SX1B37.400F1210F30 in the schematic).

It goes to `%SystemRoot%\System32\drivers\brcm\BCM4345C5.hcd`; the
`FirmwarePath` registry value (an NT path) moves it.

## Install

1. Firmware with `\_SB.BTH0` (edk2-rk3576 `tspi3m-bt-acpi`), rk3xgpio
   installed.
2. `powershell -ExecutionPolicy Bypass -File install-firmware.ps1` as
   administrator.
3. `pnputil /add-driver bcm4345bt.inf /install` (trust the CI certificate
   first, as for the other drivers: see [INSTALL](../../../docs/INSTALL.md)).

## Reading what happened

Under the device's hardware key
(`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP4345\0\Device Parameters`):

| Value | Meaning |
|---|---|
| `ResetStatus` | NTSTATUS of the first HCI_Reset at 115200. Not 0: the controller did not answer (power, 32 kHz clock, UART pins) |
| `RomVersion`, `PatchedVersion` | (HCI revision << 16) \| LMP subversion before and after the patchram |
| `ChipName` | the controller's Read Local Name |
| `FirmwareLoaded` | 1 if the patchram was sent and the controller came back |
| `Baud` | the speed it ended at |
| `AddressFromChip` | 0 if the placeholder address was replaced |
| `BringUpStatus` | NTSTATUS of the whole bring-up; the child (and so Bluetooth) exists only when it is 0 |

The driver also logs every step with `DbgPrintEx` (component IHVDRIVER,
prefix `bcm4345bt:`).

## Known gaps

- The 32 kHz sleep clock comes from the HYM8563 RTC's CLKOUT, which is
  battery-backed and which Linux's `rtc-hym8563` can turn off. Nothing on
  the Windows side turns it on. If `ResetStatus` is not 0 on a board where
  Linux Bluetooth works, check CLKOUT (register 0x0D bit 7) first.
- One controller speed: 1500000 baud, the device tree's `max-speed`, which
  the 24 MHz UART clock divides exactly. 3000000 would need a 48 MHz clock.
- Radio on/off from the Windows settings switch is BthMini's; the driver
  does not power the module down for it.

## Licence

GPL-2.0-only. Written from the 16550 and DesignWare UART register model,
the Broadcom HCI vendor commands as the Linux `btbcm`/`hci_bcm` drivers use
them, and Microsoft's BthX documentation and `bthxddi.h`. Microsoft's
SerialHCIBus sample is MS-PL, which cannot be combined with GPL code; it was
read for the shape of the BthX interface and none of its code is used.
