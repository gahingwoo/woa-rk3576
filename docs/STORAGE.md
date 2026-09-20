# Storage bring-up — RK3576 (eMMC + SD)

RK3576 exposes three MSHC-family storage controllers, but they are **two
different IP blocks** with very different Windows stories. Getting this right
matters: one needs *no* driver from us, the other needs a custom one.

| Controller | Base | ACPI `_HID` | IP | dtsi compatible |
|-----------|------|-------------|----|-----------------|
| eMMC (SDC3) | 0x2A330000 | `RKCP0D40` | **DWCMSHC** (SDHCI-compatible) | `rockchip,rk3576-dwcmshc` |
| SD card (SDHC) | 0x2A310000 | `RKCPFE2C` | **dw_mmc** (DesignWare MMC, *not* SDHCI) | `rockchip,rk3576-dw-mshc` |
| SDIO | 0x2A320000 | — | dw_mmc | `rockchip,rk3576-dw-mshc` |

## eMMC — the inbox driver binds and cannot work (measured 2026-09-19)

The eMMC controller is a **DWCMSHC**, register-compatible with the SD Host
Controller Standard. Windows does bind its inbox driver to it, and it starts:

    ACPI\RKCP0D40\3   SDA Standard Compliant SD Host Controller
                       Status: Started    Driver: sdbus.inf
                       Resources: mem 0x2A330000 len 0x10000, interrupt 285

`_CID "PNP0D40"` is present in `Emmc.asl` and doing its job. **This section
used to say adding it was the one remaining gap and was WIP. It is done, and it
was not enough.**

**No card ever appears under it.** There is no `SD\`, `MMC\` or `SFFDISK\`
device node, and `ACPI\RKCP0D40\3` has no child at all. Confirmed on a clean
WinPE 22621 built from the ADK with no third-party drivers, whose image
contains the whole stack — `sdbus.sys`, `sdport.sys`, `sdstor.sys`,
`sdstor.inf` are all in `boot.wim`. `sdstor` is absent from the running
registry only because nothing exists for it to bind to.

### Why

Three Rockchip vendor bits hold the controller usable, and **an SDHCI
`SW_RST_ALL` clears all three**:

| Register | Bit | Meaning if cleared |
|---|---|---|
| `EMMC_CTRL` @ 0x52C | 0 `CARD_IS_EMMC` | controller not in eMMC mode |
| `EMMC_CTRL` @ 0x52C | 2 `EMMC_RST_N` | **the card is held in hardware reset** |
| `EMMC_MISC_CON` @ 0x81C | 1 `MISC_INTCLK_EN` | **internal clock off — every command times out** |

(The vendor area base is the u16 at 0xE8 masked with 0xFFF; it is 0x500 here.)

Two independent records of that, neither of them a guess: mainline's
`rk35xx_sdhci_reset()` in `sdhci-of-dwcmshc.c` writes `MISC_INTCLK_EN` back
after every `sdhci_reset()`, and this project's own `DwcSdhciDxe.c` does the
same in `EdkiiSdMmcResetPost`, written from a measured failure on this board.

A standard SDHCI driver resets the controller when it starts. Microsoft's
cannot know about three Rockchip vendor bits, so nothing restores them.

**No ACPI change can fix this** — the reset happens after ExitBootServices,
where firmware has no say. `_DSD`, `_DSM`, `_RMV` and the `_CID` binding are
all irrelevant to it.

### Checked 2026-09-20: RK3588 upstream has nothing to copy

The obvious hope was that `edk2-porting/edk2-rk3588`, which is where our
`Emmc.asl` came from, had already solved this. It has not, and it is worth
recording so nobody spends the search again.

- There is no `PNP0D40` anywhere in that repository, and nothing
  sdport-related. The `_CID` on our node is **ours**, not theirs.
- Their `Emmc.asl` and `Sdhc.asl` use custom HIDs (`RKCP0D40`, `RKCPFE2C`) plus
  `_DSD` `compatible` strings. That is the Linux ACPI-with-DT-bindings route;
  one of their own methods is commented "Used by downstream Linux driver."
- Windows on RK3588 does not boot from eMMC or SD either.

The same comparison did turn up two real defects in our copy, both now fixed in
`edk2-rk3576`:

- `b178307` — the eMMC `_DSD` `compatible` property had three elements. A
  `_DSD` property is a two-element package `{name, value}`; a list of strings
  has to be nested, as `Sdhc.asl` does it.
- `63508ba` — **the `_DSM` clock table was still RK3588's.** RK3588's
  `CCLK_SRC_EMMC` parent is 1200 MHz, so its dividers read 1200/6, /8, /12 and
  /24 for 200, 150, 100 and 50 MHz. Only the register address had been changed
  for RK3576, whose parent is 400 MHz — so the same dividers were actually
  programming 66.7, 33.3, 50 and 16.7 MHz while `_DSM` went on returning the
  RK3588 numbers to the caller. The two slow entries survived the copy because
  they run off the 24 MHz crystal.

### Measured 2026-09-20, with the driver loaded

`rkemmc` binds and starts: `ACPI\RKCP0D40\3` shows `Service: rkemmc`,
`Status: Started`, `oem2.inf`. The hardware-ID match beats the inbox driver's
compatible-ID match on `PNP0D40`, which had only ever been reasoning.

**One row of the table above is wrong.** From the driver's own snapshot, taken
straight after an SDHCI `RESET_ALL`:

```
VendorBitsBefore  0xC  = 0b1100    bit0 CARD_IS_EMMC = 0   cleared
                                   bit2 EMMC_RST_N   = 1   NOT cleared
VendorBitsAfter   0xD              both set after the restore
MiscConAfter      0x3              MISC_INTCLK_EN set
```

`CARD_IS_EMMC` really is wiped by the reset, and restoring it is necessary.
`EMMC_RST_N` is **not** — it survives, and was already 1 before the driver
wrote it. Five resets were recorded and the value is the same each time, so
this is not a one-off. `MISC_INTCLK_EN` is still unknown: the driver records
that register only after setting it.

The clock path checks out. 400 kHz requested, 400 kHz delivered, CRU value
`0xFF00BB00` — mux 2 (xin_24m), divider 60, 24 MHz / 60 = 400 kHz — and
`ClockStableWaits 0`, so the internal clock relocks immediately after the rate
changes.

**What actually blocks it is not the vendor bits.**

```
BusOpCalls     5     last type 2 (SdSetVoltage)
RequestCalls   1     LastCmdIndex 0 (CMD0), LastCmdStatus STATUS_SUCCESS
InterruptCalls 1     LastIntStatus 0x1 (SDHCI_INT_RESPONSE)
SeenErrStatus  0     CmdErrors 0   DataErrors 0
```

CMD0 went out, the controller answered, the interrupt arrived, the event was
reported, nothing errored -- and sdport never asked for anything again. The SD
driver stops in the same shape, three bus operations in and no command at all.

Both miniports had a `RequestDpc` that did nothing, on the assumption that
sdport completes a request from the events the ISR reports. It does not. The
miniport owns the end of the request and has to call `SdPortCompleteRequest`.
A stack waiting forever on a request nobody finished looks exactly like this.

### 2026-09-20, later: what the command trace showed

`RequestDpc` was the stall. With it implemented the eMMC driver went from one
command to eighteen, and sdbus created the card node — `SD\VID_ab&OID_0022&PID_QK11X`,
which matches the CID the firmware reads (`0xAB`, ASCII `"QK11"`). `sdstor`
then refuses it with `CM_PROB_FAILED_START` / `0xC000000D`.

The trace, one DWORD per command
(`[31:24]` seq, `[23:16]` index, `[15:8]` err status, `[7:0]` int status):

| seq | command | result |
|---|---|---|
| 0 | CMD0 | response |
| 1 | CMD8 `SEND_IF_COND` | command timeout — SD-only, expected on eMMC |
| 2 | CMD5 | command timeout — SDIO probe, expected |
| 3 | CMD0 | response |
| 4-6 | CMD1 ×3 | response — `SEND_OP_COND` polling |
| 7 | CMD2 | response — `ALL_SEND_CID` |
| 8 | CMD3 | response — `SET_RCA` |
| 9 | CMD10 | response — `SEND_CID` |
| 10 | CMD9 | response — `SEND_CSD` |
| 11 | CMD7 | response + transfer complete — `SELECT_CARD` |
| 12-13 | CMD8 `SEND_EXT_CSD` ×2 | response + data available + transfer complete |
| 14 | CMD6 `SWITCH` | response + transfer complete |
| **15** | **CMD8** | **nothing at all** |
| 16 | CMD0 | response — sdbus restarts identification |
| 17 | CMD1 | command timeout |

So identification is clean, including two full 512-byte EXT_CSD reads with zero
data errors, and dies on the command after `SWITCH`.

Two readings fit a slot with no status in it, and they want opposite repairs:

- the card ignored the command, or
- **the command was never written to the command register.** `EmmcSendCommand`
  waits for `CMD_INHIBIT`/`DATA_INHIBIT` to clear and returns
  `STATUS_DEVICE_BUSY` without issuing if it gives up.

The second is the likely one and was the standing bug: that wait was **10 ms**.
CMD6 `SWITCH` is R1b and an eMMC holds DAT0 low while it applies the change —
this card's EXT_CSD asks for it, `Partition switching timing 4` and
`Out-of-interrupt busy timing 0xA` — and the spec allows hundreds of
milliseconds. Raised to 500 ms, and the trace now sets bit 31 on a command that
never went out, with `IssueFailures`, `LastBusyPresent` and `LastBusyMask`
beside it. **Not yet run.**

Two other defects fixed on the way, neither of them this stall:

- `EmmcSetPower` wrote `POWER_CONTROL` with the bus-power bit clear before
  writing it set, which power-cycles a soldered eMMC. Guarded, the way
  `sdhci_set_power_noreg` guards it. **The guard alone caused a regression** —
  `SDHCI_RESET_ALL` zeroes that register in hardware, so a cached value made
  the driver skip restoring power and the card stayed off. Eighteen commands
  became one. Linux clears `host->pwr` on its full-reset path; so do we now.
- The voltage was 3.3 V, which `CAPS0 = 0x3A6DC881` says this controller does
  not support. 3.0 V is what it claims and what Linux leaves programmed
  (`POWER_CONTROL 0x0D`).

### The SD slot has not moved

`rkdwmmc` is Started and has issued **no commands at all** across every run:
three bus operations, the last one `SdSetVoltage`, then nothing. The
`RequestDpc` repair cannot help it, because no request is ever made. That is
the next thing to take apart, and it is a different fault from the eMMC's.

### The bus-operation numbering, since it is easy to get wrong

```c
typedef enum _SDPORT_BUS_OPERATION_TYPE {
    SdBusOperationUndefined = 0,
    SdResetHw,              // 1
    SdResetHost,            // 2
    SdSetClock,             // 3
    SdSetVoltage,           // 4
    SdSetBusWidth,          // 5
    SdSetBusSpeed,          // 6
    SdSetSignalingVoltage,  // 7
    SdExecuteTuning         // 8
```

There are **two members before `SdResetHost`**, and a grep for the member
names alone does not show them. Every `BusOpLastType` reading taken before
2026-09-20 in this file was two too low -- "the last operation was
`SdSetBusWidth`" and "the last operation was `SdSetBusSpeed`" were both wrong
by that amount. The driver's own `switch` uses the symbolic names and was
never affected.

What the corrected bus trace says about the eMMC run: the sequence is
`SdResetHw`, `SdResetHost`, `SdSetVoltage(1)`, `SdSetClock(400)`, and then
nothing but `SdResetHost` until the recovery after the failure. **No
`SdSetBusWidth` and no `SdSetBusSpeed` anywhere**, and the clock never leaves
400 kHz. So sdbus is changing something inside the card with CMD6 and never
asking the host to follow -- which makes the SWITCH argument the last unknown.

### Settled 2026-09-20: the SD card-detect edge hypothesis is dead

`Sdhc.asl` describes card detect as `GpioInt(Edge, ActiveBoth, ...)`, and the
standing theory was that a card already in the slot at boot produces no edge,
leaving sdport waiting for an event that had already happened. Tested by
ejecting and reinserting the card inside WinPE, with the driver's snapshot read
before, after the ejection and after the reinsertion:

```
                 before   ejected   reinserted
CardDetectCalls     0        0          0
BusOpCalls          3        3          3
InterruptCalls      0        0          0
list disk        unchanged unchanged  unchanged
```

A real insertion edge changed nothing. The hypothesis is refuted, not
unconfirmed. sdport is not waiting on card detect; it is not running at all,
for the reason above.

### The driver: `drivers/storage/rkemmc`

Written 2026-09-20. An sdport miniport that does the SDHCI reset *and* writes
the three vendor bits back, modelled on `rkdwmmc` and on this project's own
EDK2 driver for the same controller.

Two things it carries over from the firmware side, both properties of the
silicon:

- **The SDHCI divider is non-functional**; the rate comes from `CCLK_SRC_EMMC`
  in the CRU and the divider stays 0. Mainline states this outright.
- **After the CRU rate changes, the controller has to relock** — stop SDCLK,
  wait for Internal Clock Stable, start SDCLK. Skipping it cost a command
  timeout on the first CMD7 after every speed change, a data CRC error on the
  CMD8 behind it, and about five minutes per boot on the firmware side
  (`edk2-rk3576` `cb31cb4`).

Not yet built and not yet run on silicon. What to read first is listed in the
driver's README.

### Ruled out by measurement, do not revisit

- *Base clock broken.* `CAPS0 = 0x3A6DC881`, bits[15:8] = `0xC8` = 200 MHz.
  (mainline sets `SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN` on every dwcmshc variant,
  which is what suggested this; it is conservative here.)
- *No card detect.* `PRESENT_STATE = 0x03F700F0` — CardInserted=1, from the
  real pin (`HOST_CONTROL1 = 0x34`, CD_SigSel=0).
- Also read: `HOST_VERSION 0x0005` (SDHCI 4.20), `POWER_CONTROL 0x0D` (on,
  3.0V), `HOST_CONTROL2 0x380F` (1.8V signalling, HS400).

Those first two were read from a running Linux, which had already fixed
everything up. **Windows sees the post-reset state, which is a different
machine** — a register read from a working OS tells you what that OS made of
the hardware, not what another OS finds.

Still not measured: that Windows actually issues `SRST_ALL`. It is what a
standard SDHCI driver does at start, but it was not observed. Reading
`0x2A330000 + 0x52C` and `+ 0x81C` from inside Windows would settle it.

### So: a DWCMSHC miniport

The fallback this document said it would not write pre-emptively is now the
plan. It needs the standard SDHCI logic plus the Rockchip vendor fixups
re-applied after every reset — the three bits above, and the DLL block at
0x800 for the higher speeds.

## SD card — custom dw_mmc driver (this repo)

The SD card slot is a **dw_mmc** controller. This is the *older* Synopsys
DesignWare Mobile Storage Host IP and is **not** SDHCI register-compatible —
there is **no Windows inbox driver** for it. It needs a custom `sdport.sys`
miniport: [../drivers/storage/rkdwmmc](../drivers/storage/rkdwmmc).

Notable wiring from `Sdhc.asl`:
- MMIO 0x2A310000, GIC SPI 283 (GSIV 283).
- **Card detect via GPIO**, not the controller's CDETECT register:
  `GpioInt (Edge, ActiveBoth, ... "\\_SB.GPI0") { GPIO_PIN_PA7 }`. This is why
  the GPIO driver ([rk3576gpio](../drivers/gpio/rk3576gpio)) must come first.
- 4-bit bus, SDR/DDR50/SDR104 capable per `_DSD`.

## Boot implications

For installing/booting Windows, **eMMC is still the primary target** — 29 GiB,
non-removable, on the CM5 module, and it leaves the NVMe to Fedora, which makes
dual boot and rescue straightforward. But it needs a **driver**, not an ACPI
change; see above. That is the highest-leverage storage action.

The custom **dw_mmc** driver here enables the **removable SD card**: useful for
installation media, as a secondary volume, and as a rescue path.

Both drivers have to load in **WinPE** as well as in the installed system, or
Setup cannot see the disk it is installing to. That means injecting them into
`boot.wim` and enabling test signing on the media's BCD until they are signed.
