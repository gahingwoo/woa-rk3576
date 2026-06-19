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
               DWMMC_FIFOTH(2, rxWm, Slot->FifoDepth / 2));

    //
    // Maximum data/response timeout and debounce.
    //
    DwmmcWrite(Slot->Regs, DWMMC_TMOUT, 0xFFFFFFFF);
    DwmmcWrite(Slot->Regs, DWMMC_DEBNCE, 0x00FFFFFF);

    if (Slot->CiuClockHz == 0) {
        Slot->CiuClockHz = RKDWMMC_CIU_CLOCK_HZ;
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

NTSTATUS
DwmmcSetClock(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG FrequencyHz
    )
{
    ULONG div;
    NTSTATUS status;

    //
    // Gate the clock before reprogramming the divider.
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

    //
    // card clock = CIU / (2 * div); div == 0 bypasses the divider (clk = CIU).
    //
    if (FrequencyHz >= Slot->CiuClockHz) {
        div = 0;
    } else {
        div = (Slot->CiuClockHz + (2 * FrequencyHz) - 1) / (2 * FrequencyHz);
        if (div > 0xFF) {
            div = 0xFF;
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

    Slot->CurrentClockHz = (div == 0) ? Slot->CiuClockHz
                                      : (Slot->CiuClockHz / (2 * div));
    return status;
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
