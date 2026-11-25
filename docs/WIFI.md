# Wi-Fi on the TaishanPi 3M (plan and status)

Status, 2026-11-24: W1 (firmware) is on edk2-rk3576 `main`; W2 (SD host
side) and W3 (the bring-up driver, `drivers/net/bcmwlsdio`) are written and
build, on branch `tspi3m-wifi-driver`. **None of them has been run on a
board.** The design from W3 on, the driver model (Ethernet-style adapter
first, then WiFiCx) and the W2/W3 board session are in
[docs/dev/wifi.md](dev/wifi.md), which replaces the W3 to W7 plan below.

This page replaces the "do not start it" verdict in [WIRELESS.md](history/2026-11-21-wireless-assessment.md)
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

#### First board run, 2026-11-25: bugcheck 0xD1 in sdport

TaishanPi 3M, Windows 11 22631, firmware `ec96848` with the option on. The
firmware probe passed (CMD5 R4 `0x30FFFF00`, 3 functions, CCCR rev `0x43`).
With `dwcmshc` from `cm5io-wifi-bt` and `bcmwlsdio` at Stage 0 staged,
Windows stopped at boot: `DRIVER_IRQL_NOT_LESS_OR_EQUAL`, `sdport.sys`
10.0.22621.608, a read of address 8 at IRQL 2. No dump.

Where it faulted, from `sdport.sys` 10.0.22621.608 and its public PDB (the
fault address ends in `B94`, and sections are page aligned, so the RVA ends
in `B94` too; the only load of `[xN, #8]` at such an RVA is this one):

    SdhostCompleteRequest+0x8c   ldr x0, [x10, #8]

`SdhostCompleteRequest` runs in the completion DPC that `SdPortCompleteRequest`
queues. For a request completed with an error it takes the slot from
`Request+0xb8`, a field sdport keeps past the public `SDPORT_REQUEST`, and
reads the slot's FDO extension (`slot+8`), which every real slot has. It was
NULL: the request given to sdport was not one sdport owns, or no longer was.

What in `dwcmshc` can do that (branch `dwcmshc-sdio-d1` closes each):

- its own local requests (CMD12 in the data reset, CMD13, tuning) live on
  the stack. After an error or a timeout `OutstandingRequest` kept pointing
  at the dead frame, and `LocalRequestDpc` (queued from the ISR, possibly
  running after the local command ended) and the busy work item both
  completed whatever `OutstandingRequest` was, to sdport once
  `LocalRequestPending` had dropped;
- the busy work item completed `OutstandingRequest` at the time it ran, not
  the request it was queued for;
- nothing stopped a request being completed to sdport twice;
- `MshcSdioDiagKick` queued a work item from `ToggleEvents`, which sdport
  calls at DIRQL (its ISR masks the card interrupt through it, and
  `SdhostDisableEvents` goes through `KeSynchronizeExecution`).
  `IoQueueWorkItem` is DISPATCH_LEVEL at most.

Which of these fired on the board is not known yet: the first three need
the breadcrumbs below, the last is a plain IRQL violation that Driver
Verifier would have caught. The fix: only the request sdport handed to
`IssueRequest` is ever given to `SdPortCompleteRequest`, once, behind a
compare-exchange; a local request is retired before its frame goes away and
the local DPC only touches the local request still on the bus; the busy work
item carries its own request; breadcrumb updates are never queued above
DISPATCH_LEVEL. `.github/scripts/check-sdport-miniport.py` (CI, `checks`)
fails the build if a DIRQL callback reaches a DISPATCH-only routine or a
second `SdPortCompleteRequest` call appears.

`SdioDiag` version 2 adds `RefusedCompletions`, `LastRefusedCmd`,
`LastRefusedStatus` (a completion the guard refused: the old code would have
given it to sdport), `LocalTimeouts`, `StaleLocalDpcs` and `DeferredKicks`.

Two packages, one service: an older `dwcmshc` package that binds only the SD
slot (`ACPI\RKCPFE2C`) shares the service name. Only one `dwcmshc.sys` image
is loaded for both controllers, so if the SD slot starts first on the old
package's image, the SDIO controller runs old code that knows nothing about
it. Remove the old package before testing, and check with `sc qc dwcmshc`
that the image path is the new package's DriverStore folder.

#### Second board run, 2026-11-25: the SD slot alone, and teardown

The `SdioDiag` key did not exist after the 0xD1 and the service's image
path was the old package's: the 0xD1 came from the old image driving the
SDIO controller. Then, with SDIO off and no card: the new package staged on
the SD slot (`ConfigFlags 0x20`) made Windows reboot-loop, and removing the
old package while Windows ran (`pnputil /delete-driver ... /uninstall`)
killed it too. No stop code was captured.

Both are a running `dwcmshc` instance being torn down (the reinstall
restarts the device on every boot until it succeeds), and the teardown was
broken in every build, old and new. sdport 22621.608 calls the miniport's
`Cleanup` first in `SdhostFdoFreeDeviceResources` and disconnects the
controller and card-detect interrupts only after it returns. `MshcCleanup`:

- freed the platform info entry the ISR and card-detect path read through
  `PlatformInfo`, and walked the list through the freed entry
  (`Entry = Entry->Flink` after `ExFreePoolWithTag`);
- deleted the slot's IFR log while the ISR could still log to it;
- left the controller's interrupts enabled;
- freed the busy work item without knowing whether it was queued;
- ran `WPP_CLEANUP` when the last controller went away, leaving an image
  that stays loaded (a restart, or the other controller) logging through a
  dead WPP context.

Now: each slot keeps its own copy of the platform info; `Cleanup` first
sets `Shutdown`, masks and acknowledges the controller, then frees, walking
the list safely; every callback returns at once on `Shutdown`; the log falls
back to the driver's default log; the busy work item is freed only when
idle; WPP is torn down in a `DriverUnload` chained in front of sdport's. The
CI check also covers this order now. Two instances (SD slot and SDIO)
share only the platform list (under its mutex) and the driver-wide log.

### W3 and later

Moved to [docs/dev/wifi.md](dev/wifi.md#stages). The stages there are W3
(SDIO access, chip identification, firmware download; code done), W4
(control channel and events, interrupt driven, scan), W5 (WPA2-PSK
connect and the data path, as an Ethernet-style adapter) and W6 (WiFiCx,
so that Windows' own Wi-Fi settings work). The data path that was W6 here
is part of W5 there, and W7 here is W6 there.
