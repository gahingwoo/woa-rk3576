# eeprom.exe

Reads the board's I2C EEPROM through [rkeeprom](../../drivers/misc/rkeeprom).
Run as administrator.

    eeprom info              size, and the board information if the format is known
    eeprom dump [FILE]       hex dump, or the raw 2 KiB to FILE
    eeprom read OFF LEN      hex dump of LEN bytes at OFF

`info` looks at the start of every 256-byte block for:

- `RADX`: Radxa's ROCK EEPROM, the Raspberry Pi HAT layout (header, then
  atoms with a CRC-16 each), atoms 1 (vendor and product strings, the product
  string holding the serial number), 4 (BOM revision) and 5 (PCB revision and
  up to ten MAC addresses), as Radxa's U-Boot (`drivers/misc/radxa-i2c-eeprom.c`,
  branch `next-dev-v2024.10`) reads them (it reads 256 bytes from 0x100);
- `R-Pi`: a Raspberry Pi HAT EEPROM, atom 1.

Radxa has not published the layout and its U-Boot notes that the data on
boards does not always match it, so a field that looks wrong is shown as it
is, with the CRC result. Anything else (including a blank part, all 0xFF) is
reported as such with a hex dump.

There is no write command.

## Test plan (ROCK 4D)

1. Firmware with the EEPROM declared (edk2-rk3576 `main`); install rkeeprom.
2. `eeprom info`: 2048 bytes in 8 blocks; either the decoded Radxa record
   (vendor "Radxa", the serial number, a MAC that matches the one printed on
   the board or seen by Linux) or "no known format" with a dump.
3. `eeprom dump rock4d-eeprom.bin` and compare with Linux
   (`cat /sys/bus/i2c/devices/6-0050/eeprom > linux.bin`): identical.
