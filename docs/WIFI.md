# Wi-Fi on the TaishanPi 3M (plan and status)

Status, 2026-11-22: W1 (firmware) and W2 (SD host side) are written and
build. **Neither has been run on a board.** W3 to W6 are not started.

This page replaces the "do not start it" verdict in [WIRELESS.md](WIRELESS.md)
for one board, the TaishanPi 3M. The estimate there was for a complete WiFiCx
driver. The plan here gets a usable link sooner by putting the data path
first and leaving the Windows Wi-Fi UI until last.

## The hardware

| | |
|---|---|
| Module | AMPAK AP6256: Broadcom BCM43456 (chip 0x4345 rev 9), FullMAC |
| Bus | SDIO, 4-bit, `sdio: mmc@2a320000`, `rockchip,rk3576-dw-mshc` |
| Controller | Synopsys DesignWare MSHC (**dw_mmc**, not SDHCI). This is the same IP as the SD slot at 0x2a310000, so `drivers/sd/dwcmshc` already drives it |
| Pins | sdmmc1m0: D0..D3 GPIO1 PB4..PB7, CMD PC0, CLK PC1, function 2 |
| Card clock | CRU CLKSEL_CON104 (`cclk_src_sdio`, mux [7:6], div [5:0]), GATE_CON42 bits 11/12, SOFTRST_CON42 bit 12 |
| Interrupt | GIC SPI 252 (GSIV 284). The SDIO card interrupt is RINTSTS/INTMASK bit 24 (Rockchip `sdio_irq = 8`) |
| WL_REG_ON | GPIO1 PC6, active high (`sdio_pwrseq` `reset-gpios`, active low, 200 ms post-power-on delay) |
| HOST_WAKE | GPIO0 PB0, level high (out-of-band interrupt, used later) |
| VBAT | always on. The Q9 switch on GPIO0 PD1 (`vcc_3v3_wl`) is not fitted, see the firmware's `Tspi3mBluetooth.c` |
| 32 kHz LPO | HYM8563 CLKOUT, which the firmware already keeps on for Bluetooth |
| Firmware files | `brcmfmac43456-sdio.bin`, `.clm_blob` and the AP6256 NVRAM `.txt`. None of them can be shipped here; an install script downloads them on the board, as `bcm4345bt` does for its patchram |

Sources: mainline Linux `rk3576.dtsi`, `rk3576-lckfb-tspi-3m.dts`,
`clk-rk3576.c`, `rst-rk3576.c`, `dw_mmc-rockchip.c`, `dw_mmc.c`, and brcmfmac.

## Architecture

```
  Windows Wi-Fi UI / netsh wlan          (W7, WiFiCx, last)
  NDIS            ┐
  bcmwlsdio.sys   ┤ KMDF function driver for SD\VID_02D0&PID_A9BF (W3 onwards)
                  │   SDIO bus layer: CMD52/53 through the SDBUS interface,
                  │   backplane window, firmware/NVRAM/CLM download
                  │   BCDC: firmware commands (dcmd) and events, 802.3 data
  sdbus.sys       ┤ inbox: card init, CIS, one PDO per card, card interrupts
  sdport.sys      ┤ inbox
  dwcmshc.sys     ┘ ours: the dw_mmc miniport, now also the SDIO controller
  ACPI \_SB.SDIO  RKCPFE2D, only when "SDIO Wi-Fi" is on in firmware setup
```

**Host side.** Windows' SD stack already speaks SDIO: sdport and sdbus do
CMD5, CMD3, CMD7, read the CIS and create a child device named after the CIS
vendor and device IDs. The function driver talks to the card through the
documented SDBUS interface in `ntddsd.h` (`SdBusOpenInterface`,
`SdBusSubmitRequest` with `SDCMD_IO_RW_DIRECT` / `SDCMD_IO_RW_EXTENDED`,
`SDP_FUNCTION_INT_ENABLE` and an interrupt callback). The INF already marks
CMD52 and CMD53 as valid for SDIO. What the miniport lacked was the SDIO
controller's clock register, a card that is always present, the card
interrupt, and continuous card clock. W2 adds those.

**Wi-Fi side.** The BCM43456 is FullMAC: the firmware does 802.11, scanning,
association and the WPA2 4-way handshake (with `sup_wpa=1`, the in-firmware
supplicant). The host sends firmware commands (`C_SET_SSID`, `wsec`,
`wpa_auth`, `wsec_pmk`, `escan`) and gets events and **802.3 frames**. So the
first useful driver does not need any Windows Wi-Fi interface at all:

- **W3 to W6: an NDIS 6 Ethernet miniport.** The network, the passphrase and
  the country come from the device's registry parameters (set by a small
  script); the driver joins at start-up and Windows sees a wired adapter with
  a link. This is the shortest path to a working connection, and everything
  in it (SDIO layer, firmware download, BCDC, events) is what a WiFiCx driver
  needs anyway.
- **W7 (later): WiFiCx.** Windows 11's Wi-Fi class extension (headers are in
  WDK 26100: `wificx/`, `dot11wificxintf.h`). It brings the Wi-Fi UI, scan
  lists, profiles and WPA3. Microsoft publishes no WiFiCx sample driver that
  I could find, so this part is mostly built from the documentation. WDI is
  older and not worth starting now.

**References that can be used.** No open-source Windows driver for a
Broadcom FullMAC SDIO chip exists that I could find. worproject and ms-iot have
none; the Windows 10 IoT Raspberry Pi 3 driver was a closed ARM32 binary.

- **OpenBSD `bwfm`** (`sys/dev/ic/bwfm.c`, `sys/dev/sdmmc/if_bwfm_sdio.c`),
  Patrick Wildt, **ISC licence**. It is the best model: one file per layer,
  about a tenth of brcmfmac's size, SDIO with CMD52/53 firmware download,
  BCDC, scan, join and the in-firmware supplicant. ISC permits redistribution
  under GPL-2.0, keeping the copyright notice.
- **Linux brcmfmac**, ISC licence (Broadcom). The authority for details:
  `sdio.c` (the SDIO state machine, the F2 watermark, the save/restore and KSO
  bits on the 43456), `bcdc.c`, `fwil.c`, `cfg80211.c` (the join sequence).
- FreeBSD `narqo/freebsd-brcmfmac`, a port of the Linux driver, also ISC.

The firmware blobs are Cypress/Infineon (redistributable in object form only
with their parts) and the NVRAM comes from vendor trees. They stay out of
this repository; `install-firmware.ps1` will fetch them by pinned commit and
SHA-256, as `bcm4345bt` does.

**Effort.** W1 and W2: a few days, done here but untested. W3: 1 to 2 weeks.
W4: 1 week. W5: 1 to 2 weeks. W6 (NDIS data path, flow control, credit
handling): 2 to 3 weeks. That is about 2 months to a WPA2 link on the
Ethernet-style driver. W7 (WiFiCx, UI, roaming, WPA3, power management): a
further 2 to 4 months.

## Milestones

Each one is small enough for one board session and has a pass/fail reading.
Everything that touches the board is off by default: the firmware option
"SDIO Wi-Fi (experimental)" is off, and with it off the firmware never powers
the module and Windows sees no SDIO controller, so nothing changes for
anyone who does not turn it on.

### W1: the firmware powers the chip and exposes the slot

edk2-rk3576 branch `tspi3m-wifi-acpi`. New setup option *ACPI / Device Tree
→ SDIO Wi-Fi (experimental)* (`PcdAcpiSdioWifi`, only on the TaishanPi 3M,
off by default). When on, in ACPI mode:

- `Tspi3mWifi.c` sets the sdmmc1m0 pins, `cclk_src_sdio` = 24 MHz, ungates and
  resets the controller, raises WL_REG_ON, waits 200 ms, then probes the card
  by polling at 400 kHz: CMD0, CMD5, CMD3, CMD7, CCCR, the CIS MANFID tuple,
  function 1 on, the ALP clock, and the chip ID at 0x18000000 by CMD53. It
  then drops and raises WL_REG_ON so Windows finds a chip as after power-on,
  and leaves the controller with its clock off and interrupts masked.
- `Wifi.asl` (SSDT "RKWIFI") declares `\_SB.SDIO`, HID `RKCPFE2D`, with the
  registers, the interrupt and `_DSD` (`bus-width 4`, `non-removable`,
  `cap-sdio-irq`, `no-1-8-v`, `max-frequency` 50 MHz for bring-up).

Board test (serial console, 1500000 8N1):

1. Flash the image, enter setup, turn the option on, save and reset.
2. Pass: the log has `TSPI-3M Wi-Fi: CMD5 R4 0x...` with 2 functions,
   `CIS vendor 0x02d0 device 0x....`, `chip ID 0x....4345: chip 0x4345 (17221) rev 9`
   and `probe PASS`. Write down the device ID: 0xa9bf is expected (it is
   what the BCM43455 reports) and has not been seen on this board yet.
3. Fail: `probe FAIL at <step>`. At "CMD5" the module is not answering
   (REG_ON, LPO, power, or pins); the other steps name the register.
4. UEFI Shell `acpiview -s SSDT` lists "RKWIFI".

### W2: Windows enumerates the card

woa-rk3576 branch `tspi3m-wifi`. `dwcmshc` now binds `ACPI\RKCPFE2D` as well
and, on that controller only (recognised by its address 0x2a320000):

- takes the card clock from CLKSEL_CON104 instead of the SD slot's CON105;
- reports the card present (`non-removable`) and ignores card-detect;
- passes the SDIO card interrupt (bit 24) to sdport, masking it until sdport
  turns it back on, as the Microsoft sdhc sample does;
- never enables card-clock auto-gating, which would block DAT1 interrupts;
- flags CMD52 as an abort only when it writes CCCR ABORT, as mainline does;
- writes breadcrumbs to
  `HKLM\SYSTEM\CurrentControlSet\Services\dwcmshc\SdioDiag`, flushed:
  `BusOps`, `CardDetectCalls`, `Commands`, `Cmd5Count`, `Cmd5Resp`,
  `Cmd3Rca`, `Cmd52Count`, `Cmd53Count`, `LastCmd`, `LastArg`, `LastResp0`,
  `CmdErrors`, `DataErrors`, `LastErrorRintsts`, `SdioInterrupts`,
  `CardIntEnables`, `ClockKhz`, `BusWidth`, `CardClockHz`.

The SD slot's behaviour is unchanged.

Board test (full Windows on the eMMC, firmware from W1 with the option on):

1. Stage the CI-built, test-signed `dwcmshc` with `pnputil /add-driver
   dwcmshc.inf`, then a clean restart (the `bcm4345bt` protocol).
2. Pass: `pnputil /enum-devices /connected /class SDHost` shows
   `ACPI\RKCPFE2D\1` started on dwcmshc, and
   `pnputil /enum-devices /connected` shows a new device whose hardware IDs
   begin `SD\VID_02D0&PID_A9BF` (code 28, no driver: expected). Record its
   full hardware and compatible ID list; the function driver's INF matches it.
3. `reg query HKLM\SYSTEM\CurrentControlSet\Services\dwcmshc\SdioDiag`:
   `Cmd5Resp` has bit 31 (ready) and functions = 2 in bits 30:28,
   `Cmd3Rca` is non-zero, `Cmd52Count` is in the tens, `CmdErrors` 0,
   `BusWidth` 4, `ClockKhz` 50000 or 25000.
4. Fail readings: no `SdioDiag` key means dwcmshc did not bind (check the
   SSDT and `pnputil`); `Commands` > 0 with `Cmd5Resp` 0 means the card does
   not answer under Windows although it did in W1 (clock, pins or the REG_ON
   cycle); `CmdErrors` > 0 gives `LastErrorRintsts` and `LastCmd`.

### W3: firmware download, chip alive

New KMDF driver `drivers/net/bcmwlsdio`, binding the hardware ID from W2,
default disabled by a registry DWORD `Enable` = 0 under its service
`Parameters`. Steps, following bwfm and brcmfmac: block sizes for F1 (64) and
F2 (512), F2 watermark, ALP then HT clock, find the ARM CR4 and SOCRAM cores
in the backplane, hold the ARM in reset, write `brcmfmac43456-sdio.bin` to RAM
by CMD53, write the NVRAM at the top of RAM with its length token, release
the ARM, wait for HT and `F2 ready`, upload the CLM blob (`clmload`), read
`ver`.

Board test: `install-firmware.ps1` fetches the three files; set `Enable` = 1;
restart. Pass: breadcrumb `FwState` reaches "running" and `FwVersion` holds
the firmware's `ver` string (for example "wl0: ... version 7.45.x"). Fail:
`FwState` gives the last step reached.

### W4: scan

`escan` on all channels, results from the `E_ESCAN_RESULT` events. Board
test: with `Enable` = 2 (scan only), the driver writes the strongest five
SSIDs with channel and RSSI to the registry. Pass: the home network is in the
list.

### W5: WPA2 connect

Join with the in-firmware supplicant (`sup_wpa` 1, `wsec` AES, `wpa_auth`
WPA2-PSK, `wsec_pmk` from the passphrase, `C_SET_SSID`), SSID and passphrase
from the registry. Board test: pass is `E_SET_SSID` success and
`E_PSK_SUP` "keyed" in the breadcrumbs, and the access point lists the
board's MAC.

### W6: data path

NDIS 6 Ethernet miniport over BCDC: TX with the BCDC header and firmware
credits, RX from F2 on the card interrupt. Board test: the adapter gets a
DHCP lease, `ping` to the gateway, then a 100 MB download (a sustained
3 to 5 MB/s is a fair first target at 50 MHz, 4-bit).

### W7 and later

WiFiCx, so Windows' own Wi-Fi UI and profiles work; UHS SDR104 with tuning
(the bus is capped at 50 MHz until then); out-of-band HOST_WAKE; power save.
