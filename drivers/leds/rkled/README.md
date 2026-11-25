# rkled: the board's LEDs

KMDF driver for `ACPI\RKCP1ED0` (`\_SB.LEDS`), which edk2-rk3576 publishes
only with the setup option **Board LEDs for the OS** on, from the board's
`AcpiTables/Leds.asl`. Not run on hardware yet.

Each `GpioIo` in `_CRS` is one LED, driven through rk3xgpio. What it shows:

| Trigger | Value | What |
|---|---|---|
| off | 0 | off |
| on | 1 | on |
| heartbeat | 2 | two short flashes a second (Linux's `heartbeat` trigger at no load) |
| disk activity | 3 | on for 100 ms after any read or write on `PhysicalDrive0..3` (`IOCTL_DISK_PERFORMANCE`) |

The firmware's `_DSD` gives the default per LED, from the board's device tree:

| Board | LED | Pin | Default |
|---|---|---|---|
| TaishanPi 3M | green (`led-0`, heartbeat) | GPIO0_A0, active low | heartbeat |
| CM5-IO | green (status) | GPIO2_D0 | heartbeat |
| CM5-IO | red (fault) | GPIO2_D1 | off |
| ROCK 4D | green (power) | GPIO0_B4 | on |
| ROCK 4D | blue (user, heartbeat) | GPIO0_C4, active low | heartbeat |

(The CM5-IO and ROCK 4D tables are on those boards' firmware branches.)

To change one, as administrator, then restart the device or the board:

    reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP1ED0\0\Device Parameters" /v Trigger0 /t REG_DWORD /d 3 /f
    pnputil /restart-device "ACPI\RKCP1ED0\0"

During boot the firmware's StatusLedDxe blinks the status LED and turns it off
at ExitBootServices; the driver takes the LEDs from there, and turns them all
off when it stops (shutdown, disable, update).

## Test plan (board)

1. Option on; Device Manager shows "Rockchip RK3576 board LEDs" started.
2. The heartbeat LED flashes twice a second; the "on" LED is lit.
3. `Trigger0` = 3, restart the device, copy a large file: the LED follows the
   disk; idle, it stays dark.
4. Shut down: the LEDs go off before the power does.
