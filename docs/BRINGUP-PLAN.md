# Windows on ARM bring-up plan — RK3576 (ArmSoM CM5-IO)

The order in which to actually get Windows running, and what each step proves.
The point of the ordering is that **every stage is verifiable on its own**, and
the first two need no driver from this repo at all.

Target board: ArmSoM CM5 on the CM5-IO carrier. Same silicon as ROCK 4D, so the
plan transfers; only the board DSDT differs.

## The one hard constraint: pick the Windows build first

RK3576 is **4× Cortex-A72 + 4× Cortex-A53 = ARMv8.0-A** (`rk3576.dtsi`, cpu
nodes). Windows 11 **24H2 and later hard-require ARMv8.1 LSE atomics** and will
crash on ARMv8.0.

| Target | Verdict |
|---|---|
| Windows 10 ARM64 (19045) | safest; use for first bring-up |
| Windows 11 23H2 (22631) / 22H2 (22621) | expected to work — the last ARMv8.0-capable builds |
| Windows 11 24H2+ | **will not boot** — ARMv8.1 requirement |

This is the same ceiling the Raspberry Pi 4 has, and for the same reason: RPi4 is
also Cortex-A72. The Windows-on-Rockchip guides say the RPi build limit does not
apply to RK3588 "as RK3588 meets the minimum CPU requirements of Windows 11 Arm"
— RK3576 does **not** meet them, so the limit does apply here.

The RPi4 precedent is also what de-risks the interrupt controller: RK3576 has
**GIC-400, i.e. GICv2** (`Madt.aslc`), not GICv3, and RPi4 runs WoA on GIC-400.
So GICv2 is not a blocker.

## Stages

### Stage 1 — turn on ACPI in the firmware (verifiable under Linux)

Windows enumerates through ACPI only. The RK3576 EDK2 port already carries a
full static table set (FADT/MADT/GTDT/DBG2/SPCR/MCFG/PPTT/IORT + a CM5-IO DSDT),
but it was excluded from the build. Enabling it means:

- build `AcpiTables.inf` + `RK3576AcpiPlatformDxe.inf` into the platform
- `PcdConfigTableModeDefault = 0x3` — install ACPI **and** the DTB, so one image
  still boots Linux via FDT and is switchable from the front-page menu

An offline audit of the built tables (2026-07-30, ACPICA `acpiexec` + a field
decoder, no hardware) found the DSDT clean — 391 objects, 93 methods, zero AML
errors — and fixed two real defects in the firmware tables:

- **MADT**: every GICC declared `PerformanceInterruptGsiv = 23` (PPI 7). RK3576's
  PMU is wired as per-core **SPIs** (`arm,cortex-a53-pmu` on GIC_SPI 0..3,
  `arm,cortex-a72-pmu` on GIC_SPI 4..7), so the correct GSIVs are 32..39, one per
  core.
- **PPTT**: the A72 L1 caches were wrong — L1D reported 4-way (A72 is 2-way) and
  L1I reported 64KB/4-way ("48KB rounded to 64KB"), where the A72 TRM says
  48KB/3-way. The rounding also silently broke associativity, because
  `RK3576_PPTT_CACHE_NODE_INIT` derives it as `Size / NumberOfSets / 64` without
  parenthesising `Size`.

Verified sound and left alone: FADT has `HW_REDUCED_ACPI` + `PSCI_COMPLIANT`
(SMC, correct for TF-A at EL3); MADT GICD reports version 2; the GICC
`ProcessorPowerEfficiencyClass` is 0 for the A53s and 1 for the A72s, which is
the right way round and is what Windows uses for heterogeneous scheduling; GTDT
timer GSIVs are the standard 26/27/29/30. The two iASL `3129 ResourceTag smaller
than Field` warnings come from `Pcie.asl` macros that are byte-identical to
RK3588's, where Windows runs on them — not our bug.

**Verify on hardware without touching Windows** — this is where most remaining
ACPI bugs die:

1. UEFI Shell: `acpiview -r 2` — every table must parse with no errors
2. Boot Linux with `acpi=force`, check `dmesg` parses FADT/MADT/GTDT and brings
   up all 8 CPUs and the arch timer
3. `ls /sys/firmware/acpi/tables/`

Note `RK3576AcpiPlatformDxe` patches the DSDT at runtime: PCI bus min/max, the
MCFG/IORT config spaces, and `\_SB.PCI0._STA` / `\_SB.PCI1._STA` from the live
ComboPHY mode. On CM5-IO combphy0 = PCIe and combphy1 = USB3, so `PCI1` is
correctly reported absent and Windows will not probe a controller the firmware
never clocked.

### Stage 2 — WinPE smoke test (the decisive step)

Boot WinPE from USB. Nothing else. **No driver from this repo is involved:**

| What it needs | Where it comes from |
|---|---|
| GIC + arch timer + PSCI | MADT / GTDT / FADT (inbox HAL) |
| USB keyboard + the boot USB stick | `Usb3Host0.asl` is already `_HID PNP0D10` → inbox usbxhci |
| Display | UEFI GOP → inbox BasicDisplay |
| Serial debug | DBG2 / SPCR → inbox 16550 |

Media: use the **Windows on Raspberry** imager and select a **Raspberry Pi 3/4**
device type — not the RK3588 recipe — because of the ARMv8.0 ceiling above.

Reaching a WinPE command prompt means the Windows kernel runs on RK3576. From
there everything left is peripheral work.

First boot, reduce variables:

- **M.2 slot empty.** The PCIe link-training fix (retry LTSSM + ComboPHY
  quirks) is not yet hardware-tested; an empty root port is harmless, a
  half-initialised one is not.
- Consider **A72-only** for the very first attempt. Windows heterogeneous
  scheduling across A53+A72 has no precedent here (RPi4 is homogeneous), and
  A53+A72 is the one thing this platform does that RPi4 does not. Mask the four
  A53 GICC entries in `Madt.aslc` to `0` (drop `EFI_ACPI_6_0_GIC_ENABLED`),
  confirm boot, then re-enable all 8.

### Stage 3 — install to eMMC

**The inbox SDHCI driver is not enough.** `Emmc.asl` publishes `_HID "RKCP0D40"`
plus `_CID "PNP0D40"` and the Microsoft SD-clock `_DSM`, which looks like it
should let inbox SDHCI bind — but on RK35xx it does not work, for reasons that
cannot be expressed through ACPI at all:

- the SDHCI **clock divider is non-functional**; the rate must be changed at the
  CRU instead
- the PHY **delay lines (DLL) must be reconfigured per bus-speed mode**,
  especially for HS200/HS400
- HS400 needs a vendor-specific setting

This is why worproject shipped a custom sdport miniport (`dwcsdhc`) for the
RK3588 eMMC rather than using the inbox driver — and `dwcsdhc` binds
`ACPI\RKCP0D40`, exactly the HID our firmware already publishes.

Do **not** plan the system volume on NVMe or SATA. The reason is sharper than
"storport is broken": RK35xx has **non-cache-coherent DMA**, and the inbox
`stornvme.sys` / `storahci.sys` assume coherency and hang. Patched builds exist
in the reference repo's `drivers/storage_fix`.

### Stage 3a — the BL31 prerequisite nobody documents

A Windows storage miniport has no clock, regulator or pinctrl framework, so the
RK35xx drivers delegate all of it to **EL3 via a Rockchip SiP SMC service**:

| SMC ID | Service |
|---|---|
| `0x82000020` / `21` | SDMMC clock rate get / set |
| `0x82000022` / `23` | SDMMC clock **phase** get / set (drive + sample = tuning) |
| `0x82000024` / `25` | regulator voltage get / set (1.8V/3.3V signalling) |
| `0x82000026` / `27` | regulator enable get / set |

These are **not upstream TF-A**. Upstream `plat/rockchip/common` only implements
`SIP_SVC_CALL_COUNT/UID/VERSION` plus `RK_SIP_SCMI_AGENT0` (`0x82000010`), and
the RK3576 platform shipped `RK_PLAT_SIP_NUM_CALLS = 0`.

**Implemented (2026-07-30)** in the RK3576 BL31, in
`plat/rockchip/rk3576/drivers/sdmmc/rk3576_sdmmc_sip.c`:

- **clock rate get/set** drives the CRU composites for `cclk_src_sdmmc0`
  (`CLKSEL_CON(105)`), `cclk_src_sdio` (`CLKSEL_CON(104)`) and `cclk_src_emmc`
  (`CLKSEL_CON(89)`), selecting among gpll/cpll/xin24m and never rounding the
  card clock *up*. Field layout is taken from the mainline
  `drivers/clk/rockchip/clk-rk3576.c`, so EL3 and Linux agree on every bit.
  xin24m matters: it is the only parent that can reach the 400 kHz init clock.
- **clock phase** deliberately returns "not supported" — see below.
- **regulators** answer for what the CM5 boards actually have: `vmmc` is
  `vcc_3v3_s3` (fixed 3.3 V, `regulator-always-on`) so enable is reported
  already-on and never switched; `vqmmc` is `vccio_sd_s0` (RK806 pldo-reg5),
  where 3.3 V succeeds and 1.8 V is refused because BL31 has no I²C master for
  the PMIC yet. Refusing is the honest answer: the caller then stays on
  high-speed instead of believing it switched to UHS signalling.

**Clock phase is not an SMC on RK3576.** RK3568/RK3588 expose `ciu-drive` and
`ciu-sample` as CRU phase clocks, which is why the RK3588 driver asks EL3 for
them. RK3576 sets `internal_phase` in `dw_mmc-rockchip.c` and keeps the phase in
the *controller's own* window — `TIMING_CON0` at 0x130 (drive), `TIMING_CON1` at
0x134 (sample), field `[11:1]` = degree/delaynum/delay_sel. The driver already
maps that window, so it programs the phase itself (`DwmmcSetPhase` in
`storage/rkdwmmc/hw.c`) with the same 60 ps-per-element arithmetic the kernel
uses.

`rkdwmmc` has been rebased accordingly: it probes the service once at init, sets
the CRU to `2 * F` (the fixed divide-by-two in the Rockchip mmc clock path,
`RK3288_CLKGEN_DIV`) and leaves its own divider at bypass, and routes
`SdSetVoltage` / `SdSetSignalingVoltage` to the regulator calls. If BL31 does not
answer, it falls back to its own divider and says so — a card enumerates, but
high-speed modes are not to be trusted.

### Stage 4 — drivers: start from the proven ones

Do **not** start by debugging this repo's framework glue. Every driver in
worproject's RK3588 set binds the **exact ACPI `_HID` our firmware already
publishes**:

| Reference driver | Binds | Our ACPI |
|---|---|---|
| `dwcsdhc` (sdport) | `ACPI\RKCP0D40` | `Emmc.asl` |
| `dwcmshc` (sdport) | `ACPI\RKCPFE2C` | `Sdhc.asl` |
| `dwc_eqos` (NetAdapterCx) | `ACPI\RKCP6543` | `Gmac0.asl` |
| `rk3xgpio` (GpioClx) | `ACPI\RKCP3002` | `Gpio.asl` |
| `rk3xi2c` (SpbCx) | `ACPI\RKCP3001` | `I2c.asl` |

The GMAC match goes further than the HID: `dwc_eqos` reads its DMA tuning from
`_DSD` (`snps,pbl`, `snps,fixed-burst`, `snps,mixed-burst`, `snps,axi-config`),
an `AXIC` package (`snps,wr_osr_lmt`, `snps,rd_osr_lmt`, `snps,blen`) and a
TX-clock `_DSM` whose GUID is `d637828d-556c-4829-966a-237072f00ff1` — our
`Gmac0.asl` provides all three, with that same GUID. So these drivers are
**drop-in candidates against our tables**, already proven on silicon.

That reframed the job: rather than deriving the class-extension ABI from scratch
in `storage/rkdwmmc/miniport.c` and `net/dwmac/netadapter.c`, those files were
diffed against the working equivalents and against the WDK headers themselves,
keeping only what is genuinely RK3576-specific (register windows, GRF bits, clock
IDs). SPI stays ours — RK3588 has no SPI driver at all.

Two divergences that diff already found and that are now fixed:

- **`dwmac` was mapping SDGMAC_GRF directly** at a hardcoded `0x26038000` to
  reselect the RGMII TX clock on every speed change. That window is not in the
  device's `_CRS`, firmware owns it, and firmware already publishes exactly this
  operation as the TX-clock `_DSM` that `dwc_eqos` calls — two writers of a
  hiword-masked register is a race. Replaced with a `_DSM` evaluation
  (`net/dwmac/acpi_dsm.c`) and the mapping removed.
- **`rkdwmmc` owned the card clock locally**; see Stage 3a.

Still open from the same diff: `dwc_eqos` reads its DMA tuning from `_DSD` and
the `AXIC` package (`snps,pbl`, `snps,fixed-burst`, `snps,wr_osr_lmt`,
`snps,blen`, …), which our firmware publishes and our `dwmac` currently ignores
in favour of hardcoded values.

Build natively on ARM64 Windows (VS2022 ARM64 + WDK 26100), enable test-signing,
install one at a time and stop at the first bugcheck. See
[BUILDING.md](BUILDING.md). Order, weakest dependency first:
**GPIO → I²C → SPI → SD → GMAC**.

Two more platform workarounds exist in the reference repo that we would
otherwise have hit blind: `usbehci_nointerlocked` and `whea_shutdown_fix`.

### Stage 5 — what stays unfinished

| Area | Status |
|---|---|
| Display | GOP framebuffer only — no acceleration, no mode changes ([DISPLAY.md](DISPLAY.md)) |
| Audio | blocked on ACPI, but less so than assumed: the reference repo has a working `pl330dma` driver plus `rk3xi2sbus`/`csaudiork3x`/codecs, so only the SAI ACPI enumeration is genuinely missing ([AUDIO.md](AUDIO.md)) |
| GPU / NPU / VPU | out of scope |
| PCIe / NVMe | firmware link training untested; inbox storport broken on Rockchip |

## Risks worth watching

1. **OP-TEE BL32 memory.** The secure-world carve-out must be marked reserved in
   the EFI memory map. Linux tolerates a sloppy map; Windows does not, so this
   class of bug only shows up at Stage 2.
2. **Heterogeneous cores.** See the A72-only note in Stage 2.
3. **Linux-only ACPI IDs.** The CM5-IO DSDT enumerates the RK806 PMIC under
   `PRP0001` and the HYM8563 RTC under `HYMB0001`. Both are Linux conventions;
   Windows will show them as unknown devices in Device Manager. Harmless — no
   Windows driver is planned for either, and the PMIC is firmware's job.
