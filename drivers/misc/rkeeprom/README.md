# rkeeprom: read-only access to an I2C EEPROM

KMDF driver for `ACPI\RKCP2416`, an I2C EEPROM the firmware declares. Today
that is the ROCK 4D's BL24C16F on I2C6 (`\_SB.I2C6.EEP0`, rk3576-rock-4d.dts
`eeprom@50`, "belling,bl24c16f", "atmel,24c16", read-only), on
edk2-rk3576 `main`. Not run on hardware yet.

A 24C16 is 2 KiB in eight 256-byte blocks, each on its own I2C address
(0x50 to 0x57); the firmware lists the eight addresses in `_CRS` in order,
and the driver reads block `n` through the `n`th connection: the word address
written, then the data read, 32 bytes per SPB sequence.

**Read-only.** The only bytes written to the part are word addresses; there
is no write path in the driver or the tool, and the write-protect pin
(EEPROM_WP, GPIO2_C5 through Q2 on the ROCK 4D) is not touched.

`\\.\RKEEPROM0` (`rkeeprom_ioctl.h`) is for [tools/eeprom](../../../tools/eeprom);
the INF gives it to SYSTEM and Administrators only, since the part may hold
serial numbers and MAC addresses.
