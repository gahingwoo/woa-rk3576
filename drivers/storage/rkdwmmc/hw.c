/*++

Module Name:

    hw.c

Abstract:

    dw_mmc register engine for the RK3576 SD slot. Verified against the kernel
    driver drivers/mmc/host/dw_mmc.c:
      - reset:  CTRL |= RESET|FIFO_RESET|DMA_RESET, poll until self-clear.
      - clock:  disable CLKENA, set CLKDIV (clk = CIU / (2*div), div 0 = bypass),
                push via the "update clock registers only" command (CMD_UPD_CLK),
                re-enable CLKENA, push again.
      - command: write CMDARG then CMD|CMD_START; START self-clears once the CIU
                accepts the command. Completion/data arrive as interrupts.
      - PIO:    BLKSIZ/BYTCNT set the transfer; data is moved through the FIFO
                data register (offset 0x100 or 0x200 by VERID).

Environment:

    Kernel mode.

--*/

#include "rkdwmmc.h"
#include "sip.h"

#define DWMMC_DEFAULT_FIFO_DEPTH    256

//
// Spin until every bit in Mask reads 0 at register Off, or TimeoutUs elapses.
//
static
NTSTATUS
DwmmcPollClear(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG Off,
    _In_ ULONG Mask,
    _In_ ULONG TimeoutUs
    )
{
    LARGE_INTEGER start;
    LARGE_INTEGER freq;
    LARGE_INTEGER now;

    start = KeQueryPerformanceCounter(&freq);

    while (DwmmcRead(Slot->Regs, Off) & Mask) {
        now = KeQueryPerformanceCounter(NULL);
        if (((now.QuadPart - start.QuadPart) * 1000000LL / freq.QuadPart) >
            (LONGLONG)TimeoutUs) {
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(2);
    }
    return STATUS_SUCCESS;
}

ULONG
DwmmcFifoOffset(
    _In_ volatile UCHAR *Regs
    )
{
    ULONG verid = READ_REGISTER_ULONG((volatile ULONG *)(Regs + DWMMC_VERID));
    return ((verid & 0xFFFF) < (DWMMC_VERID_240A & 0xFFFF))
               ? DWMMC_DATA_OFFSET_LEGACY
               : DWMMC_DATA_OFFSET_V2;
}

NTSTATUS
DwmmcResetAll(
    _In_ PRKDWMMC_SLOT Slot
    )
{
    ULONG ctrl = DwmmcRead(Slot->Regs, DWMMC_CTRL);
    ctrl |= DWMMC_CTRL_ALL_RESET;
    DwmmcWrite(Slot->Regs, DWMMC_CTRL, ctrl);

    //
    // The three reset bits self-clear when the reset completes.
    //
    return DwmmcPollClear(Slot, DWMMC_CTRL, DWMMC_CTRL_ALL_RESET, 100000);
}

VOID
DwmmcInitController(
    _In_ PRKDWMMC_SLOT Slot
    )
{
    ULONG ctrl;
    ULONG rxWm;

    Slot->FifoOffset = DwmmcFifoOffset(Slot->Regs);
    if (Slot->FifoDepth == 0) {
        Slot->FifoDepth = DWMMC_DEFAULT_FIFO_DEPTH;
    }

    (VOID)DwmmcResetAll(Slot);

    //
    // Power the slot, clear and mask all interrupts (sdport's Interrupt callback
    // manages delivery), then turn on the global interrupt enable in PIO mode.
    //
    DwmmcWrite(Slot->Regs, DWMMC_PWREN, 1);
    DwmmcWrite(Slot->Regs, DWMMC_RINTSTS, 0xFFFFFFFF);
    DwmmcWrite(Slot->Regs, DWMMC_INTMASK, 0);

    ctrl = DwmmcRead(Slot->Regs, DWMMC_CTRL);
    ctrl |= DWMMC_CTRL_INT_ENABLE;
    ctrl &= ~(DWMMC_CTRL_USE_IDMAC | DWMMC_CTRL_DMA_ENABLE);   // PIO
    DwmmcWrite(Slot->Regs, DWMMC_CTRL, ctrl);

    //
    // FIFO watermarks at the half-full mark; msize burst = 8.
    //
    rxWm = (Slot->FifoDepth / 2) - 1;
    DwmmcWrite(Slot->Regs, DWMMC_FIFOTH,
               DWMMC_FIFOTH_VALUE(2, rxWm, Slot->FifoDepth / 2));

    //
    // Maximum data/response timeout and debounce.
    //
    DwmmcWrite(Slot->Regs, DWMMC_TMOUT, 0xFFFFFFFF);
    DwmmcWrite(Slot->Regs, DWMMC_DEBNCE, 0x00FFFFFF);

    //
    // RK3576 gates the internal memory clock automatically; the kernel driver
    // sets this in dw_mci_rockchip_init and the phase logic below assumes it.
    //
    DwmmcWrite(Slot->Regs, DWMMC_MISC_CON,
               ((ULONG)DWMMC_MISC_MEM_CLK_AUTOGATE << 16) |
               DWMMC_MISC_MEM_CLK_AUTOGATE);

    //
    // Probe the SiP clock service once. A GET is harmless and tells us whether
    // BL31 implements the SD/MMC service at all; if it does not, DwmmcSetClock
    // falls back to the controller divider.
    //
    {
        ULONG ciuHz = 0;
        NTSTATUS status;

        status = RkSipSdmmcClockRateGet(
                     (ULONG_PTR)Slot->RegsPhysical.QuadPart,
                     RK_SIP_SDMMC_CLOCK_ID_MSHC_CIU,
                     &ciuHz);

        if (NT_SUCCESS(status) && ciuHz >= RKDWMMC_CLKGEN_DIV) {
            Slot->SipClockAvailable = TRUE;
            Slot->CiuClockHz = ciuHz / RKDWMMC_CLKGEN_DIV;
        } else {
            Slot->SipClockAvailable = FALSE;
            Slot->CiuClockHz = RKDWMMC_CIU_CLOCK_FALLBACK_HZ;
            RkLog(RK_DBG_ERROR,
                  "SiP SD/MMC clock service unavailable (0x%08X); "
                  "using the controller divider, high-speed modes will be "
                  "unreliable\n",
                  status);
        }
    }
}

NTSTATUS
DwmmcUpdateClockRegs(
    _In_ PRKDWMMC_SLOT Slot
    )
{
    //
    // "Update clock registers only" — no card command is issued. START
    // self-clears when the CIU has latched the new clock settings.
    //
    DwmmcWrite(Slot->Regs, DWMMC_CMD,
               DWMMC_CMD_START | DWMMC_CMD_UPD_CLK | DWMMC_CMD_PRV_DAT_WAIT);

    return DwmmcPollClear(Slot, DWMMC_CMD, DWMMC_CMD_START, 100000);
}

//
// The card clock rate is set in the CRU, not with the controller's own divider.
// The RK35xx divider cannot cover the range the card needs on its own, and a
// Windows miniport has no clock framework, so EL3 is asked to program
// cclk_src_sdmmc0 (see sip.h). The CRU is set to 2*F because of the fixed
// divide-by-two in the Rockchip mmc clock path, and the controller's own
// divider is then left at bypass so the card sees exactly what the CRU emits.
//
// If the SiP service is missing (older BL31) we fall back to the divider. That
// path enumerates a card but cannot be trusted above default speed, which is
// why it warns.
//
NTSTATUS
DwmmcSetClock(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG FrequencyHz
    )
{
    ULONG div;
    ULONG actualHz;
    NTSTATUS status;

    //
    // Gate the clock before changing the rate, so the card never sees an
    // intermediate or out-of-spec clock.
    //
    DwmmcWrite(Slot->Regs, DWMMC_CLKENA, 0);
    status = DwmmcUpdateClockRegs(Slot);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (FrequencyHz == 0) {
        Slot->CurrentClockHz = 0;
        return STATUS_SUCCESS;
    }

    div = 0;
    actualHz = FrequencyHz;

    if (Slot->SipClockAvailable) {
        status = RkSipSdmmcClockRateSet(
                     (ULONG_PTR)Slot->RegsPhysical.QuadPart,
                     RK_SIP_SDMMC_CLOCK_ID_MSHC_CIU,
                     FrequencyHz * RKDWMMC_CLKGEN_DIV);

        if (NT_SUCCESS(status)) {
            ULONG ciuHz = 0;

            //
            // Read back what EL3 could actually reach; the CRU divider is
            // integer, so the result is at or below what we asked for.
            //
            if (NT_SUCCESS(RkSipSdmmcClockRateGet(
                               (ULONG_PTR)Slot->RegsPhysical.QuadPart,
                               RK_SIP_SDMMC_CLOCK_ID_MSHC_CIU,
                               &ciuHz)) &&
                ciuHz >= RKDWMMC_CLKGEN_DIV) {
                actualHz = ciuHz / RKDWMMC_CLKGEN_DIV;
            }

            Slot->CiuClockHz = actualHz;
        } else {
            RkLog(RK_DBG_ERROR,
                  "SiP clock rate set failed (0x%08X); falling back to CLKDIV\n",
                  status);
            Slot->SipClockAvailable = FALSE;
        }
    }

    if (!Slot->SipClockAvailable) {
        //
        // Fallback: card clock = CIU / (2 * div), div == 0 bypasses.
        //
        if (Slot->CiuClockHz == 0) {
            Slot->CiuClockHz = RKDWMMC_CIU_CLOCK_FALLBACK_HZ;
        }

        if (FrequencyHz >= Slot->CiuClockHz) {
            div = 0;
            actualHz = Slot->CiuClockHz;
        } else {
            div = (Slot->CiuClockHz + (2 * FrequencyHz) - 1) / (2 * FrequencyHz);
            if (div > 0xFF) {
                div = 0xFF;
            }
            actualHz = Slot->CiuClockHz / (2 * div);
        }
    }

    DwmmcWrite(Slot->Regs, DWMMC_CLKDIV, div);
    DwmmcWrite(Slot->Regs, DWMMC_CLKSRC, 0);
    status = DwmmcUpdateClockRegs(Slot);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Enable a continuously-running clock (low-power gating off).
    //
    DwmmcWrite(Slot->Regs, DWMMC_CLKENA, DWMMC_CLKENA_ENABLE);
    status = DwmmcUpdateClockRegs(Slot);

    Slot->CurrentClockHz = actualHz;
    return status;
}

//
// Drive / sample phase. On RK3576 these are controller-internal registers
// rather than CRU phase clocks, so no world switch is involved — see the layout
// note in rkdwmmc_regs.h. The degree-to-delay arithmetic mirrors
// rockchip_mmc_set_internal_phase in the kernel driver so that a phase set here
// means the same thing it does under Linux.
//
NTSTATUS
DwmmcSetPhase(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ BOOLEAN Sample,
    _In_ ULONG Degrees
    )
{
    ULONG rate = Slot->CurrentClockHz;
    ULONG nineties;
    ULONG remainder;
    ULONG delay;
    ULONG delayNum;
    ULONG field;

    //
    // The fine delay is a fixed number of picoseconds, so converting a phase
    // angle into delay elements needs the current card clock.
    //
    if (rate == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    Degrees %= 360;
    nineties = Degrees / 90;
    remainder = Degrees % 90;

    //
    // delay = remainder / (360 * rate * delay_element), arranged to stay inside
    // 32 bits: 10000000 is PSECS_PER_SEC / 10000 / 10.
    //
    delay = 10000000UL * remainder;
    delay = (delay + (((rate / 1000) * 36 *
                       (DWMMC_PHASE_DELAY_ELEMENT_PSEC / 10)) / 2)) /
            ((rate / 1000) * 36 * (DWMMC_PHASE_DELAY_ELEMENT_PSEC / 10));

    delayNum = (delay > 255) ? 255 : delay;

    field = (delayNum != 0) ? DWMMC_PHASE_DELAY_SEL : 0;
    field |= (delayNum << DWMMC_PHASE_DELAYNUM_SHIFT) &
             DWMMC_PHASE_DELAYNUM_MASK;
    field |= nineties & DWMMC_PHASE_DEGREE_MASK;

    DwmmcWrite(Slot->Regs,
               Sample ? DWMMC_TIMING_CON1 : DWMMC_TIMING_CON0,
               (DWMMC_PHASE_FIELD_MASK << 16) |
               ((field << DWMMC_PHASE_FIELD_SHIFT) & DWMMC_PHASE_FIELD_MASK));

    return STATUS_SUCCESS;
}

ULONG
DwmmcGetPhase(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ BOOLEAN Sample
    )
{
    ULONG rate = Slot->CurrentClockHz;
    ULONG raw;
    ULONG degrees;
    ULONG delayNum;

    raw = DwmmcRead(Slot->Regs,
                    Sample ? DWMMC_TIMING_CON1 : DWMMC_TIMING_CON0);
    raw >>= DWMMC_PHASE_FIELD_SHIFT;

    degrees = (raw & DWMMC_PHASE_DEGREE_MASK) * 90;

    if ((raw & DWMMC_PHASE_DELAY_SEL) != 0 && rate != 0) {
        ULONG factor = (DWMMC_PHASE_DELAY_ELEMENT_PSEC / 10) * 36 *
                       (rate / 10000);

        delayNum = (raw & DWMMC_PHASE_DELAYNUM_MASK) >>
                   DWMMC_PHASE_DELAYNUM_SHIFT;
        degrees += (delayNum * factor + 500000) / 1000000;
    }

    return degrees % 360;
}

VOID
DwmmcSetBusWidth(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG WidthBits
    )
{
    ULONG ctype;

    if (WidthBits == 8) {
        ctype = DWMMC_CTYPE_8BIT;
    } else if (WidthBits == 4) {
        ctype = DWMMC_CTYPE_4BIT;
    } else {
        ctype = DWMMC_CTYPE_1BIT;
    }

    DwmmcWrite(Slot->Regs, DWMMC_CTYPE, ctype);
}

VOID
DwmmcSetBlockConfig(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG BlockSize,
    _In_ ULONG BlockCount
    )
{
    DwmmcWrite(Slot->Regs, DWMMC_BLKSIZ, BlockSize);
    DwmmcWrite(Slot->Regs, DWMMC_BYTCNT, BlockSize * BlockCount);
}

NTSTATUS
DwmmcSendCommand(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG CmdRegister,
    _In_ ULONG Argument
    )
{
    DwmmcWrite(Slot->Regs, DWMMC_CMDARG, Argument);
    DwmmcWrite(Slot->Regs, DWMMC_CMD,
               CmdRegister | DWMMC_CMD_START | DWMMC_CMD_USE_HOLD_REG);

    //
    // START self-clears once the CIU accepts the command. CMD_DONE and any data
    // events are then delivered through the interrupt path.
    //
    return DwmmcPollClear(Slot, DWMMC_CMD, DWMMC_CMD_START, 100000);
}

ULONG
DwmmcFifoCount(
    _In_ PRKDWMMC_SLOT Slot
    )
{
    return DWMMC_STATUS_FIFO_COUNT(DwmmcRead(Slot->Regs, DWMMC_STATUS));
}

//
// Drain up to Bytes from the RX FIFO. Reads only what the FIFO currently holds;
// the caller (interrupt/DPC) re-enters on the next RXDR / DATA_OVER event.
//
ULONG
DwmmcReadFifo(
    _In_ PRKDWMMC_SLOT Slot,
    _Out_writes_bytes_(Bytes) PUCHAR Dst,
    _In_ ULONG Bytes
    )
{
    ULONG offset = 0;

    while (offset < Bytes && DwmmcFifoCount(Slot) > 0) {
        ULONG word = DwmmcRead(Slot->Regs, Slot->FifoOffset);
        ULONG chunk = Bytes - offset;
        if (chunk > 4) {
            chunk = 4;
        }
        RtlCopyMemory(Dst + offset, &word, chunk);
        offset += chunk;
    }

    return offset;
}

//
// Push up to Bytes into the TX FIFO, stopping if the FIFO fills. Returns the
// number of bytes written.
//
ULONG
DwmmcWriteFifo(
    _In_ PRKDWMMC_SLOT Slot,
    _In_reads_bytes_(Bytes) PUCHAR Src,
    _In_ ULONG Bytes
    )
{
    ULONG offset = 0;

    while (offset < Bytes && DwmmcFifoCount(Slot) < Slot->FifoDepth) {
        ULONG word = 0;
        ULONG chunk = Bytes - offset;
        if (chunk > 4) {
            chunk = 4;
        }
        RtlCopyMemory(&word, Src + offset, chunk);
        DwmmcWrite(Slot->Regs, Slot->FifoOffset, word);
        offset += chunk;
    }

    return offset;
}
