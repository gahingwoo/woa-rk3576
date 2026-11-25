# Wi-Fi and Bluetooth

English | [简体中文](wireless.zh-CN.md)

Status on 2026-11-24:

- Bluetooth works on the TaishanPi 3M with [bcm4345bt](../../drivers/bluetooth/bcm4345bt)
  (since 2026-11-22): pairing and file transfer, no voice (SCO). The driver
  runs UART6 itself instead of a separate SerCx2 UART driver, and hands the
  HCI link to the in-box BthMini; how is in its README.
- Bluetooth on the CM5-IO: not written. Its module is a different chip on
  UART4.
- Wi-Fi: not working on either board, and nothing is built. The assessment
  below still holds.
- ROCK 4D (AIC8800D80 on USB, a different chip): a bring-up driver is on
  branch `aic8800-usb`, not tried on a board; see [aic8800.md](aic8800.md).
  Its WiFiCx mode (Windows' own Wi-Fi) is on branch `aic8800-wificx`; see
  [wificx.md](wificx.md).

The assessment as written on 2026-11-21, before Bluetooth worked, is in
[history](../history/2026-11-21-wireless-assessment.md); its Bluetooth plan
(a SerCx2 driver plus cywbtserialbus) was not the way it was done.

## The hardware

| | TaishanPi 3M | CM5-IO (CM5 module) |
|---|---|---|
| Module | AMPAK AP6256 (Broadcom BCM43456) | Synaptics SYN43752 (Wi-Fi 6) |
| Wi-Fi bus | SDIO, `sdio: mmc@2a320000` (`rockchip,rk3576-dw-mshc`), 4-bit | SDIO, the same controller |
| Wi-Fi power | 3.3 V switch GPIO0 PD1 (active low), WL_REG_ON GPIO1 PC6, host wake GPIO0 PB0 | WL_REG_ON GPIO1 PC6 |
| 32 kHz LPO | HYM8563 RTC clock output (I2C2 0x51) | HYM8563 |
| Bluetooth bus | UART6 (uart6m1, RTS/CTS), `brcm,bcm4345c5`, max 1.5 Mbaud | UART4 (uart4m1, RTS/CTS) |
| Bluetooth GPIOs | shutdown GPIO1 PC7, host wake GPIO0 PB1, device wake GPIO1 PD4 | BT power GPIO1 PC7 (not in the mainline tree) |

For Wi-Fi the firmware declares nothing in ACPI and powers nothing: Linux
sequences the Wi-Fi supply, reset and LPO clock through `mmc-pwrseq`. For
Bluetooth on the TaishanPi 3M the firmware declares `\_SB.BTH0` (`RKCP4345`)
with UART6, BT_REG_ON and BT_WAKE, muxes the UART pins and sets its clock.

## Wi-Fi: not feasible at a reasonable size

- No Windows driver exists for these chips on ARM64. worproject's Raspberry
  Pi driver table lists the Pi 3B+/4's CYW43455 (the same family) as having
  no driver. Windows 10 IoT Core had a closed ARM32-only Broadcom SDIO driver
  for the Pi 3; it does not load on ARM64. Microsoft's own Pi BSP
  (ms-iot/rpi-iotcore) has no Wi-Fi driver.
- Writing one means a WDI or WiFiCx miniport for a FullMAC chip over SDIO:
  the SDIO bus layer, firmware, NVRAM and CLM download, the BCDC/fwil command
  and event protocol, and mapping scan, connect, PMK and roaming onto
  WiFiCx. Linux's brcmfmac (ISC licence, usable as a reference) is about
  35,000 lines. The firmware does the WPA supplicant work.
- This repository's SD miniport (`drivers/sd/dwcmshc`) does not implement
  SDIO card interrupts (it always reports none), so that would be extra work,
  or the out-of-band host-wake GPIO would have to be used.
- The firmware blobs (Cypress/Infineon for the AP6256, Synaptics for the
  SYN43752) are redistributable in object form only, and AP6256 NVRAM files
  come from vendor BSPs with unclear terms.

Rough size: 6 to 9 person-months to scan, connect with WPA2 and roam on one
chip, then 1 to 2 more for WPA3, power management and hardening; the second
chip is a further bring-up.

Recommendation: do not start it. Use Ethernet, which works on both boards.
A USB Wi-Fi adapter is only an answer if its exact hardware ID has a Windows
ARM64 driver (Microsoft Update Catalog, ARM64 filter, or the inbox `net*.inf`
of the 23H2 image); none was confirmed during this assessment.
