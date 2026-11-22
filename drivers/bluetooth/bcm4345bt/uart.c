/*
 * uart.c - the RK3576 DesignWare UART, run directly by this driver
 *
 * Windows on Arm has no driver for this UART (no serial.sys, no SerCx2
 * controller for it), so the Bluetooth device's _CRS carries the UART's
 * registers and interrupt and this file is the whole serial layer: 8N1,
 * FIFOs on, hardware RTS/CTS, receive into a ring in the ISR, transmit
 * from a ring on THRE.
 *
 * Written from the 16550 register model and the DesignWare APB UART
 * extensions (USR, SRR, AFCE) as Linux 8250_dw.c and the RK3576 device
 * tree use them. No code is taken from any of them.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcm4345bt.h"

static __forceinline ULONG
Rd(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Regs + Off));
}

static __forceinline VOID
Wr(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Ctx->Regs + Off), Val);
}

/*
 * The DesignWare UART ignores LCR writes while busy (receiving); wait for a
 * gap, but never longer than 200 us: this runs with the interrupt lock held.
 * A line that is never idle (noise from an unpowered module) gets the
 * divisor written anyway, and the attempt is counted.
 */
static VOID
UartWaitNotBusy(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    ULONG i;

    for (i = 0; i < 200; i++) {
        if ((Rd(Ctx, UART_USR) & USR_BUSY) == 0) {
            return;
        }
        KeStallExecutionProcessor(1);
    }
    Ctx->BusyTimeouts++;
}

static VOID
UartProgramDivisor(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Baud)
{
    ULONG div = (Ctx->UartClock + 8 * Baud) / (16 * Baud);

    if (div == 0) {
        div = 1;
    }
    UartWaitNotBusy(Ctx);
    Wr(Ctx, UART_LCR, LCR_DLAB | LCR_8N1);
    Wr(Ctx, UART_DLL, div & 0xFF);
    Wr(Ctx, UART_DLH, (div >> 8) & 0xFF);
    Wr(Ctx, UART_LCR, LCR_8N1);
    (VOID)Rd(Ctx, UART_USR);    /* clears a busy-detect raised by the writes */
    Ctx->Baud = Baud;
}

/* Called with interrupts not yet connected (D0Entry). */
VOID
UartInit(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Baud)
{
    Wr(Ctx, UART_IER, 0);
    Wr(Ctx, UART_SRR, SRR_RFR | SRR_XFR);
    KeStallExecutionProcessor(10);
    UartProgramDivisor(Ctx, Baud);
    Wr(Ctx, UART_FCR, FCR_FIFOE | FCR_RFIFOR | FCR_XFIFOR | FCR_RT_HALF);
    Wr(Ctx, UART_MCR, MCR_RTS | MCR_AFCE);
    (VOID)Rd(Ctx, UART_LSR);
    (VOID)Rd(Ctx, UART_USR);
    {
        ULONG i;
        for (i = 0; i < 256 && (Rd(Ctx, UART_LSR) & LSR_DR); i++) {
            (VOID)Rd(Ctx, UART_RBR);
        }
    }
    Ctx->RxHead = Ctx->RxTail = 0;
    Ctx->TxHead = Ctx->TxTail = 0;
    BT_LOG("UART at %p: %lu baud from %lu Hz, CPR 0x%08lX MCR 0x%02lX", Ctx->Regs,
           Baud, Ctx->UartClock, Rd(Ctx, UART_CPR), Rd(Ctx, UART_MCR) & 0xFF);
}

/*
 * Stop the UART from interrupting, at once and without waiting for
 * anything. Safe at any IRQL up to DIRQL; used on the stop path, where the
 * interrupt has been disabled by the framework already, and before the
 * module is powered off, when its TX line may float.
 */
VOID
UartQuiesce(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Regs == NULL) {
        return;
    }
    Ctx->Ier = 0;
    Wr(Ctx, UART_IER, 0);
    Wr(Ctx, UART_MCR, 0);           /* RTS off: tell the module to stop sending */
    Wr(Ctx, UART_SRR, SRR_RFR | SRR_XFR);
    (VOID)Rd(Ctx, UART_LSR);
    (VOID)Rd(Ctx, UART_USR);
}

/*
 * Change speed between packets. The caller has made sure nothing is in
 * flight (UartWaitTxIdle), so only the divisor is rewritten. The FIFOs are
 * left alone: a reply at the new speed may already be arriving.
 */
VOID
UartSetBaud(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Baud)
{
    WdfInterruptAcquireLock(Ctx->Interrupt);
    UartProgramDivisor(Ctx, Baud);
    WdfInterruptReleaseLock(Ctx->Interrupt);
}

VOID
UartFlush(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    WdfInterruptAcquireLock(Ctx->Interrupt);
    Wr(Ctx, UART_SRR, SRR_RFR | SRR_XFR);
    Ctx->RxHead = Ctx->RxTail = 0;
    Ctx->TxHead = Ctx->TxTail = 0;
    WdfInterruptReleaseLock(Ctx->Interrupt);
}

/* Only from the ISR or with the interrupt lock held. */
static VOID
UartFillTx(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    ULONG n = 0;

    while (n < UART_TX_BURST && Ctx->TxTail != Ctx->TxHead) {
        Wr(Ctx, UART_THR, Ctx->TxRing[Ctx->TxTail]);
        Ctx->TxTail = (Ctx->TxTail + 1) & (BT_TX_RING - 1);
        n++;
    }
    if (Ctx->TxTail == Ctx->TxHead) {
        Ctx->Ier = (UCHAR)(Ctx->Ier & ~IER_ETBEI);
    } else {
        Ctx->Ier = (UCHAR)(Ctx->Ier | IER_ETBEI);
    }
    Wr(Ctx, UART_IER, Ctx->Ier);
}

/*
 * Queue one H4 frame (type byte, then Data). All or nothing: FALSE when
 * the ring has no room for the whole frame. IRQL <= DISPATCH_LEVEL.
 */
BOOLEAN
UartQueueFrame(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ UCHAR Type,
               _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    ULONG used, i;
    BOOLEAN ok = FALSE;

    WdfInterruptAcquireLock(Ctx->Interrupt);
    used = (Ctx->TxHead - Ctx->TxTail) & (BT_TX_RING - 1);
    if (Length + 1 < BT_TX_RING - 1 - used) {
        Ctx->TxRing[Ctx->TxHead] = Type;
        Ctx->TxHead = (Ctx->TxHead + 1) & (BT_TX_RING - 1);
        for (i = 0; i < Length; i++) {
            Ctx->TxRing[Ctx->TxHead] = Data[i];
            Ctx->TxHead = (Ctx->TxHead + 1) & (BT_TX_RING - 1);
        }
        if (Rd(Ctx, UART_LSR) & LSR_THRE) {
            UartFillTx(Ctx);
        } else {
            Ctx->Ier = (UCHAR)(Ctx->Ier | IER_ETBEI);
            Wr(Ctx, UART_IER, Ctx->Ier);
        }
        ok = TRUE;
    }
    WdfInterruptReleaseLock(Ctx->Interrupt);
    return ok;
}

/* PASSIVE_LEVEL: wait for the ring and the shift register to drain. */
BOOLEAN
UartWaitTxIdle(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG TimeoutMs)
{
    ULONG waited;
    BOOLEAN empty;

    for (waited = 0; waited <= TimeoutMs; waited++) {
        WdfInterruptAcquireLock(Ctx->Interrupt);
        empty = (Ctx->TxHead == Ctx->TxTail) && (Rd(Ctx, UART_LSR) & LSR_TEMT);
        WdfInterruptReleaseLock(Ctx->Interrupt);
        if (empty) {
            return TRUE;
        }
        BtDelayMs(1);
    }
    return FALSE;
}

/*
 * At most BT_ISR_RX_MAX bytes and 16 IIR rounds per interrupt. If the ring
 * is full, or the ISR has run BT_ISR_STORM times since the DPC last
 * drained, receive interrupts are masked (RxThrottled); the DPC unmasks
 * them once the ring has room. A line that never stops delivering bytes,
 * or an interrupt that cannot be cleared, so costs a bounded amount of
 * time per DPC round and cannot hold a CPU at DIRQL.
 */
BOOLEAN
BtEvtInterruptIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(WdfInterruptGetDevice(Interrupt));
    BOOLEAN claimed = FALSE;
    ULONG iir, lsr, loops, taken = 0;

    UNREFERENCED_PARAMETER(MessageID);

    if (ctx->Regs == NULL) {
        return FALSE;
    }
    ctx->IsrCount++;

    for (loops = 0; loops < 16; loops++) {
        iir = Rd(ctx, UART_IIR) & IIR_ID_MASK;
        if (iir == IIR_NONE) {
            break;
        }
        claimed = TRUE;
        switch (iir) {
        case IIR_BUSY:
            (VOID)Rd(ctx, UART_USR);
            break;
        case IIR_MSI:
            (VOID)Rd(ctx, UART_MSR);
            break;
        case IIR_THRE:
            UartFillTx(ctx);
            break;
        default:            /* RLS, RDA, character timeout */
            break;
        }
        while (taken < BT_ISR_RX_MAX) {
            ULONG next;
            lsr = Rd(ctx, UART_LSR);
            if (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI)) {
                ctx->LineErrors++;
            }
            if ((lsr & LSR_DR) == 0) {
                break;
            }
            next = (ctx->RxHead + 1) & (BT_RX_RING - 1);
            if (next == ctx->RxTail) {
                ctx->RxOverflow++;
                break;      /* ring full: leave the rest in the FIFO */
            }
            ctx->RxRing[ctx->RxHead] = (UCHAR)Rd(ctx, UART_RBR);
            ctx->RxHead = next;
            ctx->RxBytes++;
            taken++;
        }
        if (taken >= BT_ISR_RX_MAX ||
            ((ctx->RxHead + 1) & (BT_RX_RING - 1)) == ctx->RxTail) {
            break;
        }
    }

    if (!claimed && (Rd(ctx, UART_LSR) & LSR_DR)) {
        claimed = TRUE;     /* data with no IIR reason: still ours */
    }
    if (claimed) {
        ctx->IsrSinceDpc++;
        if (((ctx->RxHead + 1) & (BT_RX_RING - 1)) == ctx->RxTail ||
            ctx->IsrSinceDpc > BT_ISR_STORM) {
            /* Mask receive until the DPC has drained. */
            ctx->Ier = (UCHAR)(ctx->Ier & ~(IER_ERBFI | IER_ELSI));
            Wr(ctx, UART_IER, ctx->Ier);
            if (!ctx->RxThrottled) {
                ctx->RxThrottled = TRUE;
                ctx->ThrottleCount++;
            }
        }
        WdfInterruptQueueDpcForIsr(Interrupt);
    } else {
        ctx->IsrUnclaimed++;
    }
    return claimed;
}

/*
 * Moves at most BT_DPC_RX_MAX bytes per run; if more are waiting it queues
 * itself again instead of looping, so one run is short however busy the
 * line is.
 */
VOID
BtEvtInterruptDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext((WDFDEVICE)AssociatedObject);
    ULONG n = 0;
    BOOLEAN more;

    WdfSpinLockAcquire(ctx->PacketLock);
    WdfInterruptAcquireLock(ctx->Interrupt);
    while (ctx->RxTail != ctx->RxHead && n < BT_DPC_RX_MAX) {
        ctx->RxStage[n++] = ctx->RxRing[ctx->RxTail];
        ctx->RxTail = (ctx->RxTail + 1) & (BT_RX_RING - 1);
    }
    more = (ctx->RxTail != ctx->RxHead);
    ctx->IsrSinceDpc = 0;
    if (ctx->RxThrottled && !more && ctx->InterruptsOn) {
        ctx->RxThrottled = FALSE;
        ctx->Ier = (UCHAR)(ctx->Ier | IER_ERBFI | IER_ELSI);
        Wr(ctx, UART_IER, ctx->Ier);
    }
    WdfInterruptReleaseLock(ctx->Interrupt);
    if (n != 0) {
        BtParseBytes(ctx, ctx->RxStage, n);
    }
    WdfSpinLockRelease(ctx->PacketLock);

    BtServiceReads(ctx);
    BtServiceWrites(ctx);
    if (more) {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
}

NTSTATUS
BtEvtInterruptEnable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE AssociatedDevice)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(AssociatedDevice);

    UNREFERENCED_PARAMETER(Interrupt);
    /* Called with the interrupt lock held. */
    ctx->Ier = IER_ERBFI | IER_ELSI;
    ctx->RxThrottled = FALSE;
    ctx->IsrSinceDpc = 0;
    ctx->InterruptsOn = TRUE;
    Wr(ctx, UART_IER, ctx->Ier);
    return STATUS_SUCCESS;
}

NTSTATUS
BtEvtInterruptDisable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE AssociatedDevice)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(AssociatedDevice);

    UNREFERENCED_PARAMETER(Interrupt);
    ctx->InterruptsOn = FALSE;
    UartQuiesce(ctx);
    return STATUS_SUCCESS;
}
