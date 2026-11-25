# rk806pwrkey: the RK806 power key as the Windows power button

KMDF driver for `ACPI\RKCP806B` (`\_SB.I2C1.PWRK`), which edk2-rk3576
publishes only with the setup option **Power button (RK806)** on
(`Silicon/Rockchip/RK3576/AcpiTables/PwrKey.asl`). Not run on hardware yet;
see the test plan below.

On the CM5-IO (SW4) and the ROCK 4D (SW1) the power button goes to the
PMIC's `PWRON` pin, not to the SoC. The TaishanPi 3M has no key on `PWRON`
(its buttons are RECOVERY and RESET), so the firmware does not offer this
device there; its Recovery key is the power button instead, through
[rk3xsaradc](../../adc/rk3xsaradc). The PMIC (RK806, I2C1 address 0x23)
reports a press and a release as `PWRON_FALL` and `PWRON_RISE` in `INT_STS0`
(0x77, bits 0 and 1) and holds its `INT` line, GPIO0_A6, low until the status
is written back (mainline Linux `rk808.h`, `rk8xx-core.c`,
`rk805-pwrkey.c`). The driver:

- at start sets the PMIC interrupt active low (`GPIO_INT_CONFIG` bit 1 = 0),
  masks everything but the two key events (`INT_MSK0` = 0xFC,
  `INT_MSK1` = 0xFF) and clears what is pending (the firmware does the same at
  ExitBootServices);
- on each interrupt (GpioClx through rk3xgpio, a passive-level ISR) reads
  `INT_STS0`/`INT_STS1` over SPB (rk3xi2c) and writes back what was set, which
  releases the line;
- reports a press as HID Generic Desktop / System Control / **System Power
  Down** through vhf.sys, its lower filter. Windows treats that usage as the
  power button: what a press does is set in Control Panel, Power Options,
  "Choose what the power buttons do" (or `powercfg /setacvalueindex scheme_current
  sub_buttons pbuttonaction 3` for shut down).

Holding the button still powers the board off in the PMIC itself; the driver
has no part in that.

Why a driver and not an ACPI-only button (a GED that notifies a PNP0C0C
device): acknowledging the interrupt needs I2C, which AML can only do through
a GenericSerialBus operation region that nothing on these boards has used,
and a level interrupt nobody acknowledges keeps the line low. See
`PwrKey.asl`.

If the PMIC does not answer at start, the device fails with code 10 rather
than leave an interrupt line that nobody can clear. If it stops answering
later, the ISR backs off 100 ms per interrupt instead of spinning a CPU.

## Install

Needs rk3xi2c and rk3xgpio. With the CI's test-signed package (the WinPE
workflow signs it): `pnputil /add-driver rk806pwrkey.inf /install`.

## Test plan (board)

1. Firmware: setup, ACPI page, **Power button (RK806)** on, ACPI mode. The
   serial log says "power button on: GPIO0_A6 GPIO in, pull-up, level 1" and
   "installing the PwrKey SSDT".
2. Windows: Device Manager shows "Rockchip RK806 power key" with a
   "HID-compliant system controller" under it.
3. Power Options: set the power button to **Shut down**; a short press shuts
   Windows down (and the board powers off, Rk806I2cPowerOffDxe). Set it to
   **Do nothing**: a press does nothing and nothing else breaks.
4. A short tap and a 2 s press both work; a long press (about 6 s) still cuts
   the power at once, as before.
5. Debug output (DbgView or KD): "power key pressed (n)" per press, no
   repeated "cannot read or clear INT_STS0".
