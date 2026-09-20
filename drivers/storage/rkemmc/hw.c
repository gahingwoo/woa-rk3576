/*++

Module Name:

    hw.c

Abstract:

    DWCMSHC register engine for the RK3576 eMMC: the standard SDHCI operations
    plus the Rockchip vendor handling that the inbox driver does not do.

    Three things here are specific to this silicon and are the reason the inbox
    SDHCI driver cannot drive it:

    1. An SDHCI SW_RST_ALL clears EMMC_MISC_CON[1], EMMC_CTRL[2] and
       EMMC_CTRL[0].  They have to be written back before the controller will
       answer anything.  Mainline does the equivalent in rk35xx_sdhci_reset().

    2. The card clock does not come from the SDHCI divider.  Mainline states it
       plainly in sdhci-of-dwcmshc.c: the SDCLK frequency-select bits "are not
       functional on Rockchip's SDHCI implementation", and the rate is set
       through the clock provider.  So the divider stays 0 and the CRU does the
       dividing.

    3. Because of (2), changing the rate means changing a clock the controller
       is already running on.  It then has to be told to relock: stop SDCLK,
       wait for Internal Clock Stable, start SDCLK.  Mainline reaches the same
       place by falling through to sdhci_enable_clk(host, 0) after
       clk_set_rate().  Skipping it is not subtle -- on the firmware side of
       this same controller it produced a command timeout on the first CMD7
       after every speed change, then a data CRC error on the CMD8 behind it,
       and about five minutes of retries per boot.

Environment:

    Kernel mode.

--*/

#include "rkemmc.h"

//
// A short spin, used where the eMMC or SDHCI specs call for a settle time.
// KeStallExecutionProcessor is safe at any IRQL and these waits are all in
// the tens to hundreds of microseconds.
//
#define EmmcStall(_us)  KeStallExecutionProcessor(_us)

/*++

Routine Description:

    Write back the vendor bits that an SDHCI software reset clears.

    Called after every reset, including the ones sdport asks for.  Writing them
    when they are already set is harmless, so there is no read-modify-decide
    here -- the cost of getting that wrong is a controller that never answers,
    and the cost of an extra MMIO write is nothing.

--*/
VOID
EmmcApplyVendorBits(
    _In_ PRKEMMC_SLOT Slot
    )
{
    ULONG misc;
    ULONG ctrl;

    //
    // Internal clock first: with MISC_INTCLK_EN clear nothing else takes
    // effect, including the reset of this sequence.
    //
    misc = EmmcRead32(Slot, DWCMSHC_EMMC_MISC_CON);
    EmmcWrite32(Slot, DWCMSHC_EMMC_MISC_CON, misc | MISC_CON_INTCLK_EN);

    //
    // CARD_IS_EMMC tells the controller this port is an eMMC rather than an
    // SD card, which is what enables the data strobe path.  RST_N released
    // takes the card out of hardware reset; the eMMC spec wants at least
    // 200 us before the first command after that.
    //
    ctrl = EmmcRead32(Slot, DWCMSHC_EMMC_CTRL);
    g_RkDiag.VendorBitsBefore = ctrl;

    ctrl |= (EMMC_CTRL_CARD_IS_EMMC | EMMC_CTRL_RST_N);
    EmmcWrite32(Slot, DWCMSHC_EMMC_CTRL, ctrl);

    //
    // Command Conflict Check off.  The firmware driver for this controller
    // does the same; with it on, the controller reports conflicts on commands
    // that are in fact fine.
    //
    EmmcWrite32(Slot, DWCMSHC_HOST_CTRL3, 0);

    EmmcStall(RKEMMC_RST_N_SETTLE_US);

    g_RkDiag.VendorBitsAfter = EmmcRead32(Slot, DWCMSHC_EMMC_CTRL);
    g_RkDiag.MiscConAfter    = EmmcRead32(Slot, DWCMSHC_EMMC_MISC_CON);
}

/*++

Routine Description:

    Wait for a software reset bit to clear.

--*/
static
NTSTATUS
EmmcWaitReset(
    _In_ PRKEMMC_SLOT Slot,
    _In_ UCHAR Mask
    )
{
    ULONG i;

    //
    // The SDHCI spec does not bound this; 100 ms is what Linux allows.
    //
    for (i = 0; i < 100000; i++) {
        if ((EmmcRead8(Slot, SDHCI_SOFTWARE_RESET) & Mask) == 0) {
            return STATUS_SUCCESS;
        }
        EmmcStall(1);
    }

    RkLog(RK_DBG_ERROR, "reset 0x%02x did not clear\n", Mask);
    return STATUS_IO_TIMEOUT;
}

NTSTATUS
EmmcResetAll(
    _In_ PRKEMMC_SLOT Slot
    )
{
    NTSTATUS status;

    g_RkDiag.ResetCalls++;

    EmmcWrite8(Slot, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
    status = EmmcWaitReset(Slot, SDHCI_RESET_ALL);

    //
    // Unconditionally, even if the wait timed out: if the controller is in a
    // bad state the vendor bits are exactly what it needs to come back, and
    // leaving them clear guarantees it will not.
    //
    EmmcApplyVendorBits(Slot);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // A RESET_ALL also clears the interrupt enables and the clock setup, so
    // the caller has to rebuild them.  EmmcInitController does that.
    //
    return STATUS_SUCCESS;
}

NTSTATUS
EmmcResetCmdDat(
    _In_ PRKEMMC_SLOT Slot,
    _In_ UCHAR Mask
    )
{
    //
    // CMD and DAT resets do not clear the vendor block -- only RESET_ALL
    // does -- so this one is the plain SDHCI sequence.
    //
    EmmcWrite8(Slot, SDHCI_SOFTWARE_RESET, Mask);
    return EmmcWaitReset(Slot, Mask);
}

/*++

Routine Description:

    Restart the card clock: stop SDCLK, wait for the controller's internal
    clock to relock, start SDCLK again.  The equivalent of
    sdhci_enable_clk(host, 0) in Linux.

    This is required after the CRU rate changes underneath a running
    controller.  See the note at the top of this file.

--*/
static
VOID
EmmcRestartCardClock(
    _In_ PRKEMMC_SLOT Slot
    )
{
    USHORT clk;
    ULONG  i;

    clk = EmmcRead16(Slot, SDHCI_CLOCK_CONTROL);
    clk &= (USHORT)~SDHCI_CLOCK_CARD_EN;
    EmmcWrite16(Slot, SDHCI_CLOCK_CONTROL, clk);

    clk |= SDHCI_CLOCK_INT_EN;
    EmmcWrite16(Slot, SDHCI_CLOCK_CONTROL, clk);

    //
    // Linux allows 150 ms.  Poll in 10 us steps and record how long it took:
    // if a card stops enumerating after a speed change, whether the clock
    // relocked is the first thing worth knowing and there is no other way to
    // find out after the fact.
    //
    for (i = 0; i < 15000; i++) {
        if (EmmcRead16(Slot, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) {
            break;
        }
        EmmcStall(10);
    }

    g_RkDiag.ClockStableWaits = i;
    if (i == 15000) {
        g_RkDiag.ClockStableTimeouts++;
        RkLog(RK_DBG_ERROR, "internal clock never reported stable\n");
    }

    clk = EmmcRead16(Slot, SDHCI_CLOCK_CONTROL);
    EmmcWrite16(Slot, SDHCI_CLOCK_CONTROL, clk | SDHCI_CLOCK_CARD_EN);
}

/*++

Routine Description:

    Program CCLK_SRC_EMMC in the CRU and return the rate actually obtained.

    The table mirrors the _DSM in the firmware's Emmc.asl: same register, same
    values.  Parent rates are 400 MHz for the two PLL muxes and 24 MHz for the
    crystal, both confirmed against the firmware's own clock calls on hardware.

--*/
static
ULONG
EmmcProgramCru(
    _In_ PRKEMMC_SLOT Slot,
    _In_ ULONG FrequencyHz
    )
{
    ULONG value;
    ULONG actual;

    if (Slot->CruClkSel == NULL) {
        //
        // Nothing was mapped, so the rate is whatever the firmware left
        // behind.  Report that rather than a number we did not set.
        //
        return Slot->CurrentClockHz;
    }

    if (FrequencyHz >= CRU_EMMC_PARENT_24M) {
        ULONG div;

        //
        // Round the divider up so the card is never clocked faster than asked.
        //
        div = (CRU_EMMC_PARENT_400M + FrequencyHz - 1) / FrequencyHz;
        if (div < 1)  { div = 1; }
        if (div > 64) { div = 64; }

        value  = CRU_EMMC_WRITE_MASK | CRU_EMMC_MUX_GPLL_400M | CRU_EMMC_DIV(div);
        actual = CRU_EMMC_PARENT_400M / div;
    } else {
        ULONG div;

        div = (CRU_EMMC_PARENT_24M + FrequencyHz - 1) / FrequencyHz;
        if (div < 1)  { div = 1; }
        if (div > 64) { div = 64; }

        value  = CRU_EMMC_WRITE_MASK | CRU_EMMC_MUX_XIN_24M | CRU_EMMC_DIV(div);
        actual = CRU_EMMC_PARENT_24M / div;
    }

    WRITE_REGISTER_ULONG((PULONG)Slot->CruClkSel, value);

    g_RkDiag.ClockCruValue = value;
    return actual;
}

NTSTATUS
EmmcSetClock(
    _In_ PRKEMMC_SLOT Slot,
    _In_ ULONG FrequencyHz
    )
{
    ULONG actual;

    if (FrequencyHz == 0) {
        USHORT clk = EmmcRead16(Slot, SDHCI_CLOCK_CONTROL);
        EmmcWrite16(Slot, SDHCI_CLOCK_CONTROL, clk & (USHORT)~SDHCI_CLOCK_CARD_EN);
        Slot->CurrentClockHz = 0;
        return STATUS_SUCCESS;
    }

    if (FrequencyHz > RKEMMC_MAX_CLOCK_HZ) {
        FrequencyHz = RKEMMC_MAX_CLOCK_HZ;
    }

    g_RkDiag.ClockSetCalls++;
    g_RkDiag.ClockRequestedHz = FrequencyHz;

    //
    // Divider 0 on purpose: pass the CRU clock straight through.  Mainline
    // says the SDCLK frequency-select bits do not work on this controller, so
    // anything other than 0 here would be a guess about hardware that is
    // documented not to respond.
    //
    {
        USHORT clk = EmmcRead16(Slot, SDHCI_CLOCK_CONTROL);
        clk &= (USHORT)~0xFF00;     // SDCLK Frequency Select [15:8]
        clk &= (USHORT)~0x00C0;     // upper divider bits [7:6]
        EmmcWrite16(Slot, SDHCI_CLOCK_CONTROL, clk);
    }

    actual = EmmcProgramCru(Slot, FrequencyHz);
    Slot->CurrentClockHz = actual;
    g_RkDiag.ClockActualHz = actual;

    //
    // The DLL.  At or below 52 MHz it is bypassed and the sampling window is
    // wide enough not to need tuning; above that it has to lock, and this
    // driver does not go there yet (RKEMMC_MAX_CLOCK_HZ).  The values are the
    // vendor kernel's for RK3576.
    //
    EmmcWrite32(Slot, DWCMSHC_EMMC_DLL_CTRL, DLL_CTRL_BYPASS | DLL_CTRL_START);
    EmmcWrite32(Slot, DWCMSHC_EMMC_DLL_RXCLK, DLL_RXCLK_ORI_GATE);
    EmmcWrite32(Slot, DWCMSHC_EMMC_DLL_TXCLK, 0);
    EmmcWrite32(Slot, DWCMSHC_EMMC_DLL_CMDOUT, 0);
    EmmcWrite32(Slot, DWCMSHC_EMMC_DLL_STRBIN,
                DLL_DLYENA | DLL_STRBIN_DELAY_NUM_SEL |
                (RK3576_NONDLL_STRBIN_DELAY << DLL_STRBIN_DELAY_NUM_OFS));

    //
    // The rate just changed underneath a running controller.  Relock.
    //
    EmmcRestartCardClock(Slot);

    RkLog(RK_DBG_INFO, "clock req=%u actual=%u stable_waits=%u\n",
          FrequencyHz, actual, g_RkDiag.ClockStableWaits);

    return STATUS_SUCCESS;
}

VOID
EmmcSetBusWidth(
    _In_ PRKEMMC_SLOT Slot,
    _In_ ULONG WidthBits
    )
{
    UCHAR ctrl = EmmcRead8(Slot, SDHCI_HOST_CONTROL);

    ctrl &= (UCHAR)~(SDHCI_CTRL_4BITBUS | SDHCI_CTRL_8BITBUS);

    if (WidthBits == 8) {
        ctrl |= SDHCI_CTRL_8BITBUS;
    } else if (WidthBits == 4) {
        ctrl |= SDHCI_CTRL_4BITBUS;
    }

    EmmcWrite8(Slot, SDHCI_HOST_CONTROL, ctrl);
    Slot->BusWidthBits = WidthBits;
}

VOID
EmmcSetPower(
    _In_ PRKEMMC_SLOT Slot,
    _In_ BOOLEAN On,
    _In_ BOOLEAN Voltage18
    )
{
    UCHAR power;

    if (!On) {
        EmmcWrite8(Slot, SDHCI_POWER_CONTROL, 0);
        Slot->PowerValue = 0;
        Slot->PowerProgrammed = TRUE;
        return;
    }

    //
    // 3.0 V, not 3.3 V.  The capability register on this board reads
    // 0x3A6DC881, whose voltage bits say 3.0 V supported and both 3.3 V and
    // 1.8 V not -- and Linux leaves POWER_CONTROL at 0x0D, which is 3.0 V with
    // the bus-power bit set.  Programming a voltage the controller does not
    // claim is not a safe default.
    //
    // Voltage18 is therefore ignored: this board has no 1.8 V VDD rail for the
    // eMMC and GetSlotCapabilities does not offer one.
    //
    UNREFERENCED_PARAMETER(Voltage18);

    power = (UCHAR)(SDHCI_POWER_300 | SDHCI_POWER_ON);

    //
    // Do nothing if it is already there.  This guard is the whole point of
    // the function.
    //
    // The write below deliberately clears the bus-power bit first, which
    // powers the card off; on a soldered eMMC that is a power cycle.  The card
    // returns to idle and forgets the RCA it was given, so every addressed
    // command afterwards goes unanswered -- no response, no error, nothing.
    //
    // That is exactly what the command trace showed on CM5-IO 2026-09-20.
    // Identification ran cleanly to CMD6 SWITCH -- CID, CSD and two EXT_CSD
    // reads all good, zero data errors -- and then the next CMD8 recorded no
    // interrupt status and no error status at all.  sdport had called
    // SdSetVoltage in between, and this function power-cycled a card that was
    // already powered at the voltage being asked for.
    //
    // Linux guards the same register the same way: sdhci_set_power_noreg
    // returns early when host->pwr already equals the requested value.
    //
    if (Slot->PowerProgrammed && (Slot->PowerValue == power)) {
        return;
    }

    EmmcWrite8(Slot, SDHCI_POWER_CONTROL, (UCHAR)(power & ~SDHCI_POWER_ON));
    EmmcWrite8(Slot, SDHCI_POWER_CONTROL, power);

    Slot->PowerValue = power;
    Slot->PowerProgrammed = TRUE;

    //
    // Toggling power clears SDCLK_ENABLE on this controller implementation --
    // the firmware driver documents the same behaviour and re-adds the bit.
    // Do the full relock rather than just setting the bit back, because the
    // internal clock has to be stable before the next command either way.
    //
    EmmcRestartCardClock(Slot);
}

VOID
EmmcSetUhsMode(
    _In_ PRKEMMC_SLOT Slot,
    _In_ USHORT Mode
    )
{
    USHORT ctrl2 = EmmcRead16(Slot, SDHCI_HOST_CONTROL2);

    ctrl2 &= (USHORT)~SDHCI_CTRL_UHS_MASK;
    ctrl2 |= (Mode & SDHCI_CTRL_UHS_MASK);
    EmmcWrite16(Slot, SDHCI_HOST_CONTROL2, ctrl2);

    if ((Mode & SDHCI_CTRL_UHS_MASK) == SDHCI_CTRL_HS400) {
        ULONG ctrl = EmmcRead32(Slot, DWCMSHC_EMMC_CTRL);
        EmmcWrite32(Slot, DWCMSHC_EMMC_CTRL, ctrl | EMMC_CTRL_CARD_IS_EMMC);
    }
}

VOID
EmmcInitController(
    _In_ PRKEMMC_SLOT Slot
    )
{
    //
    // Reset clears the vendor block; EmmcResetAll puts it back.
    //
    (VOID)EmmcResetAll(Slot);

    //
    // Maximum data timeout.  The SDHCI counter is coarse and a short value
    // here shows up as spurious data timeouts on a slow card.
    //
    EmmcWrite8(Slot, SDHCI_TIMEOUT_CONTROL, 0x0E);

    //
    // Enable status for everything we act on, and signal (the interrupt line)
    // for the same set.  sdport gates which events it wants through
    // ToggleEvents; this is the superset.
    //
    {
        USHORT normal = SDHCI_INT_RESPONSE | SDHCI_INT_DATA_END |
                        SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL |
                        SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE;
        USHORT errors = 0xFFFF;

        EmmcWrite16(Slot, SDHCI_INT_ENABLE, normal);
        EmmcWrite16(Slot, SDHCI_ERR_INT_ENABLE, errors);
        EmmcWrite16(Slot, SDHCI_SIGNAL_ENABLE, normal);
        EmmcWrite16(Slot, SDHCI_ERR_SIGNAL_ENABLE, errors);
    }

    //
    // Card power on at 3.3 V, then the identification clock.  The eMMC on
    // CM5-IO is on a fixed rail, so POWER_CONTROL is mostly a formality --
    // but the SDCLK restart it triggers is not.
    //
    EmmcSetPower(Slot, TRUE, FALSE);
    (VOID)EmmcSetClock(Slot, 400000);
    EmmcSetBusWidth(Slot, 1);

    g_RkDiag.HostVersion  = EmmcRead16(Slot, SDHCI_HOST_VERSION);
    g_RkDiag.Capabilities = EmmcRead32(Slot, SDHCI_CAPABILITIES);
}

/*++

Routine Description:

    Issue one command.  The caller has already built the COMMAND and
    TRANSFER_MODE register values and programmed BLOCK_SIZE/BLOCK_COUNT for a
    data command.

--*/
NTSTATUS
EmmcSendCommand(
    _In_ PRKEMMC_SLOT Slot,
    _In_ USHORT CommandReg,
    _In_ USHORT TransferMode,
    _In_ ULONG Argument,
    _In_ BOOLEAN HasData
    )
{
    ULONG mask = SDHCI_CMD_INHIBIT;
    ULONG i;

    //
    // A command that uses the data line, or one with a busy response, also
    // needs DAT idle.
    //
    if (HasData || ((CommandReg & 0x03) == SDHCI_CMD_RESP_SHORT_BUSY)) {
        mask |= SDHCI_DATA_INHIBIT;
    }

    //
    // Bounded, and short: if the line is still busy after 10 ms something is
    // wrong and reporting it beats blocking sdport's request pump.
    //
    for (i = 0; i < 10000; i++) {
        if ((EmmcRead32(Slot, SDHCI_PRESENT_STATE) & mask) == 0) {
            break;
        }
        EmmcStall(1);
    }

    if (i == 10000) {
        RkLog(RK_DBG_ERROR, "CMD%u: line busy, present=0x%08x\n",
              (CommandReg >> 8) & 0x3F, EmmcRead32(Slot, SDHCI_PRESENT_STATE));
        return STATUS_DEVICE_BUSY;
    }

    EmmcWrite32(Slot, SDHCI_ARGUMENT, Argument);
    EmmcWrite16(Slot, SDHCI_TRANSFER_MODE, TransferMode);
    EmmcWrite16(Slot, SDHCI_COMMAND, CommandReg);

    return STATUS_SUCCESS;
}

/*++

Routine Description:

    PIO.  Move as much as the controller currently has room for, or data for,
    and return how many bytes moved.  Callers track the running total.

    The buffer-data port is 32 bits wide; a tail shorter than a word is
    handled a byte at a time so an odd block size cannot walk off the end of
    the caller's buffer.

--*/
ULONG
EmmcReadBuffer(
    _In_ PRKEMMC_SLOT Slot,
    _Out_writes_bytes_(Bytes) PUCHAR Dst,
    _In_ ULONG Bytes
    )
{
    ULONG moved = 0;

    while (moved + sizeof(ULONG) <= Bytes) {
        if ((EmmcRead32(Slot, SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE) == 0) {
            return moved;
        }
        *(ULONG UNALIGNED *)(Dst + moved) = EmmcRead32(Slot, SDHCI_BUFFER);
        moved += sizeof(ULONG);
    }

    if (moved < Bytes) {
        ULONG word;
        ULONG tail = Bytes - moved;
        ULONG j;

        if ((EmmcRead32(Slot, SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE) == 0) {
            return moved;
        }
        word = EmmcRead32(Slot, SDHCI_BUFFER);
        for (j = 0; j < tail; j++) {
            Dst[moved + j] = (UCHAR)(word >> (8 * j));
        }
        moved = Bytes;
    }

    return moved;
}

ULONG
EmmcWriteBuffer(
    _In_ PRKEMMC_SLOT Slot,
    _In_reads_bytes_(Bytes) PUCHAR Src,
    _In_ ULONG Bytes
    )
{
    ULONG moved = 0;

    while (moved + sizeof(ULONG) <= Bytes) {
        if ((EmmcRead32(Slot, SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE) == 0) {
            return moved;
        }
        EmmcWrite32(Slot, SDHCI_BUFFER, *(ULONG UNALIGNED *)(Src + moved));
        moved += sizeof(ULONG);
    }

    if (moved < Bytes) {
        ULONG word = 0;
        ULONG tail = Bytes - moved;
        ULONG j;

        if ((EmmcRead32(Slot, SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE) == 0) {
            return moved;
        }
        for (j = 0; j < tail; j++) {
            word |= ((ULONG)Src[moved + j]) << (8 * j);
        }
        EmmcWrite32(Slot, SDHCI_BUFFER, word);
        moved = Bytes;
    }

    return moved;
}
