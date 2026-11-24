> Historical copy, 2026-11-21: docs/WIRELESS.md as it was before the 2026-11-24 rewrite,
> unchanged except that its links into this repository now point at commit
> f8c3a47, where they resolve. It may not match main; the current page is [dev/wireless.md](../dev/wireless.md).

# Wi-Fi and Bluetooth

Neither works under Windows on either board, and nothing for them is built.
This page records what it would take, as assessed on 2026-11-21 from the
device trees, this repository, worproject's repositories and public sources.
Nothing below has been tried on a board.

## The hardware

| | TaishanPi 3M | CM5-IO (CM5 module) |
|---|---|---|
| Module | AMPAK AP6256 (Broadcom BCM43456) | Synaptics SYN43752 (Wi-Fi 6) |
| Wi-Fi bus | SDIO, `sdio: mmc@2a320000` (`rockchip,rk3576-dw-mshc`), 4-bit | SDIO, the same controller |
| Wi-Fi power | 3.3 V switch GPIO0 PD1 (active low), WL_REG_ON GPIO1 PC6, host wake GPIO0 PB0 | WL_REG_ON GPIO1 PC6 |
| 32 kHz LPO | HYM8563 RTC clock output (I2C2 0x51) | HYM8563 |
| Bluetooth bus | UART6 (uart6m1, RTS/CTS), `brcm,bcm4345c5`, max 1.5 Mbaud | UART4 (uart4m1, RTS/CTS) |
| Bluetooth GPIOs | shutdown GPIO1 PC7, host wake GPIO0 PB1, device wake GPIO1 PD4 | BT power GPIO1 PC7 (not in the mainline tree) |

The firmware declares none of it in ACPI and powers none of it: Linux
sequences the Wi-Fi supply, reset and LPO clock through `mmc-pwrseq`.

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

## Bluetooth: feasible, about two months

Windows' Bluetooth stack runs over a transport driver, and for a UART-attached
controller that is a serial HCI bus driver on top of a SerCx2 UART controller
driver. The pieces:

1. **A Broadcom HCI UART transport.** worproject/cywbtserialbus (MS-PL), a
   fork of Microsoft's `bluetooth/serialhcibus` sample, already does what
   these chips need: HCI reset, Download_Minidriver (0xFC2E) and the `.hcd`
   patch download, and the baud rate change (0xFC18). It runs the Pi 4's
   CYW43455 ("working" in worproject's table). It binds `ACPI\BCM2EA6` and
   opens the UART through a resource hub connection. `BCM4345C5.hcd` for the
   AP6256 is published in RPi-Distro/bluez-firmware; a SYN43752 patch file
   would have to be found. Adapting it (ID, file names, 1.5 or 3 Mbaud): about
   1 week.
2. **A SerCx2 driver for the RK3576 UART** (DesignWare APB UART, 16550-like,
   32-bit registers, `reg-shift 2`, automatic RTS/CTS). Neither this
   repository nor worproject/Rockchip-Windows-Drivers has one, and Windows has
   no inbox SerCx2 driver for it. The closest base is ms-iot's SerCx2 PL011
   driver (MIT), with its hardware layer replaced by a 16550 one. About 3 to 5
   weeks, and it is reusable for any other UART peripheral.
3. **Firmware.** Power the module and drive BT_REG_ON (GPIO1 PC7) high before
   ExitBootServices, start the HYM8563's 32 kHz output (register 0x0D bit 7),
   and describe `\_SB.URT6` (TaishanPi 3M, 0x2AD90000) or `\_SB.URT4` (CM5-IO,
   0x2AD70000) with a Bluetooth child that has a `UARTSerialBusV2` resource
   with flow control, after the Raspberry Pi tianocore `Uart.asl` `BTH0`.
   cywbtserialbus drives no GPIOs, so the firmware has to leave the chip
   powered. About 1 week.
4. Bring-up and debugging: 1 to 2 weeks.

Total: about 6 to 9 person-weeks, mostly the UART driver.

Recommendation: if wireless is wanted, do Bluetooth first, on the TaishanPi
3M (its chip is the Pi 4's family and its patch file is public), in this
order: UART6 loopback under the new SerCx2 driver, then firmware power-up and
the LPO clock, then HCI reset with the ROM firmware, then the `.hcd`
download.
