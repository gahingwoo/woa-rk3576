# rkemmc — RK3576 eMMC host (DWCMSHC, Windows on ARM)

An `sdport.sys` miniport for the RK3576 **eMMC** controller, a Synopsys
**DWCMSHC** (DesignWare Cores Mobile Storage Host) — device-tree
`rockchip,rk3576-dwcmshc` — published as `ACPI\RKCP0D40`.

## Why a custom driver, when the controller is SDHCI compatible

It is SDHCI compatible, Windows has an inbox SDHCI miniport, and the firmware's
ACPI node carries `_CID PNP0D40` so that driver binds. It binds. It starts. No
card ever appears.

Measured on CM5-IO 2026-09-19: `ACPI\RKCP0D40\3` reaches **Started** on
`sdbus.inf` with no child device.

The cause is three Rockchip vendor bits that an SDHCI `SW_RST_ALL` clears:

| register | bit | consequence when clear |
|---|---|---|
| `EMMC_MISC_CON` (0x81C) | 1 `MISC_INTCLK_EN` | internal clock off — every command times out |
| `EMMC_CTRL` (0x52C) | 2 `EMMC_RST_N` | eMMC held in hardware reset — CMD0 times out |
| `EMMC_CTRL` (0x52C) | 0 `CARD_IS_EMMC` | HS400 data strobe does not work |

No inbox driver restores them, and **no ACPI change can** — the reset is an
MMIO write from inside the host driver. So the host driver has to be ours.
Mainline Linux does the equivalent in `rk35xx_sdhci_reset()`.

## Layering

| File | Confidence | Purpose |
|------|-----------|---------|
| [hw.c](hw.c) | derived from a working EDK2 driver for the same controller | SDHCI engine + Rockchip vendor hooks: reset, vendor-bit restore, CRU clock, DLL, command issue, PIO. |
| [miniport.c](miniport.c) | **template — verify on build** | `SDPORT_*` callbacks mapping sdport ops onto hw.c. |
| [rkemmc_regs.h](rkemmc_regs.h) | SDHCI 3.00 spec + `sdhci-of-dwcmshc.c` | register map + bit defs |

## Two things about this silicon that are not in the SDHCI spec

Both were found from the firmware side of the same controller
(`Silicon/Rockchip/Drivers/DwcSdhciDxe` in the EDK2 tree) and are properties of
the hardware, not of EDK2.

**The SDHCI divider does not work.** Mainline says so outright: the SDCLK
frequency-select bits "are not functional on Rockchip's SDHCI implementation",
and the rate is set through the clock provider. So the divider stays 0
(pass-through) and `CCLK_SRC_EMMC` in the CRU does the dividing. Seeing
`Divisor 0` here is the design, not a fault.

**After changing that rate, the controller must be told to relock.** Stop
SDCLK, wait for Internal Clock Stable, start SDCLK — the equivalent of
`sdhci_enable_clk(host, 0)`, which mainline reaches by falling through to it
after `clk_set_rate()`. Skipping it is not subtle: on the firmware side it
produced a command timeout on the first CMD7 after every speed change, a data
CRC error on the CMD8 behind it, and about five minutes of retries per boot.
Fixed there in `edk2-rk3576` commit `cb31cb4`; `EmmcRestartCardClock` is the
same fix on this side.

## Design (v1)

- **PIO data path.** SDMA and ADMA2 come later; v1 exists to find out whether a
  card enumerates at all.
- **Capped at eMMC high speed (52 MHz), 8-bit.** HS200/HS400 need the DLL
  locked and a tuning pass, neither written nor measured. The driver bypasses
  the DLL below 52 MHz, which is what makes running untuned safe. Claiming
  HS200 would get the card switched into a timing the host cannot sample.
- **No card detect.** The eMMC is soldered; `GetCardDetectState` returns TRUE
  rather than reading an SDHCI `CARD_PRESENT` bit that is not wired to
  anything.
- **The CRU register is mapped directly.** `CCLK_SRC_EMMC` sits outside this
  device's `_CRS`, and an sdport miniport gets a mapped window and no device
  handle, so it cannot evaluate the ACPI `_DSM` that does this job for other
  operating systems. The table here mirrors that `_DSM` exactly — same
  register, same values. If the mapping fails the driver still loads and runs
  at whatever rate the firmware left behind.

## Status — read before trusting it

- **Not yet built.** The `SDPORT_*` signatures and event semantics are owned by
  the WDK `<sdport.h>`; building the SD driver corrected four of them, and this
  one has not had that pass yet.
- **Not yet run on silicon.**
- `StartType` is **demand start** for now. It wants to be boot start
  eventually, but boot start also means any fault here is a boot fault, and
  the first image carrying this driver went 1 good boot to 3 bad where the
  same WinPE without it had gone 5 for 5. Small numbers, and not a clean
  comparison -- the driver-free image was also carrying `bootdebug`, which
  changes timing -- but there is no reason to hold the risk during bring-up.
  PnP starts the driver when it enumerates the device either way.
- The `_DSM` clock table in `Emmc.asl` it mirrors was RK3588's until
  `edk2-rk3576` commit `63508ba`: RK3588's parent is 1200 MHz and RK3576's is
  400 MHz, so the four fast entries were programming 66.7, 33.3, 50 and
  16.7 MHz while reporting 200, 150, 100 and 50. Firmware older than that
  commit will disagree with this driver about the clock.

## Bring-up order

Injected into `boot.wim` alongside `rkdwmmc` and `rk3576gpio`, with test
signing on the media's BCD until they are signed. Unlike `rkdwmmc` this one has
no GPIO dependency.

What to read first, from the registry snapshot at
`HKLM\SYSTEM\CurrentControlSet\Services\rkemmc\Diag`:

- `VendorBitsBefore` / `VendorBitsAfter` — whether the reset really did clear
  `EMMC_CTRL` and whether the restore took. This is the whole premise of the
  driver and it has never been observed directly.
- `ClockStableWaits` / `ClockStableTimeouts` — whether the internal clock
  relocks after a rate change.
- `RequestCalls`, `LastCmdIndex`, `SeenErrStatus` — whether commands went out
  and what the controller said about them.
