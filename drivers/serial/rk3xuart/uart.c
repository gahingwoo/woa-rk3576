/*
 * uart.c - the DesignWare APB UART itself: line setup, the interrupt, and
 * the SerCx2 PIO receive and transmit callbacks
 *
 * Data path. The ISR empties the receive FIFO into a 4 KiB ring and turns a
 * transmit-empty interrupt into "ready"; the DPC tells SerCx2 (it cannot be
 * told from DIRQL). SerCx2 pulls received bytes with RkuEvtRxReadBuffer and
 * pushes bytes to send with RkuEvtTxWriteBuffer, and asks to be called back
 * when either side can make progress again (the Enable/Cancel ready pairs).
 *
 * Every field the ISR touches is touched elsewhere only with the interrupt
 * lock held. SerCx2 is told about readiness only from the DPC, after the
 * flag that says so was cleared under the lock, so a cancel either wins (and
 * returns TRUE, no callback follows) or loses (returns FALSE, the callback
 * is already on its way); never both.
 *
 * Written from the 16550 register model and the DesignWare APB UART
 * extensions (USR, TFL, RFL, SRR, CPR) as the RK3576 TRM, Linux 8250_dw.c and
 * rk3576.dtsi describe them. No code is taken from any of them.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xuart.h"

ULONG
RkuRead(_In_ PRKU_DEVICE Dev, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Dev->Regs + Off));
}

VOID
RkuWrite(_In_ PRKU_DEVICE Dev, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Dev->Regs + Off), Val);
}

static __forceinline ULONG
RingCount(_In_ PRKU_DEVICE Dev)
{
    return (Dev->RxHead - Dev->RxTail) & (RKU_RX_RING_SIZE - 1);
}

/*
 * Move whatever the receive FIFO holds into the ring, noting line errors.
 * Interrupt lock held. Returns the number of bytes moved.
 */
static ULONG
RkuDrainRxLocked(_In_ PRKU_DEVICE Dev)
{
    ULONG moved = 0;
    ULONG guard;
    ULONG lsr;

    for (guard = 0; guard < 2 * RKU_DEFAULT_FIFO + 16; guard++) {
        lsr = RkuRead(Dev, UART_LSR);
        if (lsr & LSR_OE) {
            Dev->CommErrors |= SERIAL_ERROR_OVERRUN;
            Dev->WaitEvents |= SERIAL_EV_ERR;
            Dev->Overruns++;
        }
        if (lsr & LSR_PE) {
            Dev->CommErrors |= SERIAL_ERROR_PARITY;
            Dev->WaitEvents |= SERIAL_EV_ERR;
        }
        if (lsr & LSR_FE) {
            Dev->CommErrors |= SERIAL_ERROR_FRAMING;
            Dev->WaitEvents |= SERIAL_EV_ERR;
        }
        if (lsr & LSR_BI) {
            Dev->CommErrors |= SERIAL_ERROR_BREAK;
            Dev->WaitEvents |= SERIAL_EV_BREAK;
        }
        if ((lsr & LSR_DR) == 0) {
            break;
        }

        {
            UCHAR c = (UCHAR)RkuRead(Dev, UART_RBR);

            /* a break arrives as one NUL byte with BI set: not data */
            if (lsr & LSR_BI) {
                continue;
            }
            if (RingCount(Dev) == RKU_RX_RING_SIZE - 1) {
                Dev->CommErrors |= SERIAL_ERROR_QUEUEOVERRUN;
                Dev->WaitEvents |= SERIAL_EV_ERR;
                continue;
            }
            Dev->RxRing[Dev->RxHead] = c;
            Dev->RxHead = (Dev->RxHead + 1) & (RKU_RX_RING_SIZE - 1);
            moved++;
        }
    }

    if (moved != 0) {
        Dev->WaitEvents |= SERIAL_EV_RXCHAR;
        if (Dev->RxArmed) {
            Dev->RxArmed = FALSE;
            Dev->RxReadyPending = TRUE;
        }
    }
    return moved;
}

/*
 * The DesignWare UART ignores writes to LCR (and so to the divisor) while it
 * is receiving: USR.BUSY. Wait for a gap; on a line that never goes idle,
 * flush the receive FIFO every 100 us, which ends "busy" when the FIFO was
 * the reason (the same thing Linux 8250_dw does). Interrupt lock held.
 */
static VOID
RkuWaitIdleLocked(_In_ PRKU_DEVICE Dev)
{
    ULONG i;

    for (i = 0; i < 1000; i++) {
        if ((RkuRead(Dev, UART_USR) & USR_BUSY) == 0) {
            return;
        }
        if ((i % 100) == 99) {
            RkuWrite(Dev, UART_SRR, SRR_RFR);
        }
        KeStallExecutionProcessor(1);
    }
    Dev->BusyIrqs++;
}

static VOID
RkuProgramLocked(_In_ PRKU_DEVICE Dev, _In_ ULONG Divisor)
{
    ULONG tries;

    for (tries = 0; tries < 3; tries++) {
        RkuWaitIdleLocked(Dev);
        RkuWrite(Dev, UART_LCR, Dev->Lcr | LCR_DLAB);
        if (Divisor != 0) {
            RkuWrite(Dev, UART_DLL, Divisor & 0xFF);
            RkuWrite(Dev, UART_DLH, (Divisor >> 8) & 0xFF);
        }
        RkuWrite(Dev, UART_LCR, Dev->Lcr);
        if ((RkuRead(Dev, UART_LCR) & 0xFF) == Dev->Lcr) {
            break;
        }
    }
    (VOID)RkuRead(Dev, UART_USR);   /* clears a busy-detect interrupt the writes raised */
}

static ULONG
RkuDivisor(_In_ ULONG ClockHz, _In_ ULONG Baud, _Out_ PULONG ErrorPermille)
{
    ULONGLONG div;
    ULONGLONG actual;
    ULONGLONG diff;

    div = ((ULONGLONG)ClockHz + 8ULL * Baud) / (16ULL * Baud);
    if (div == 0) {
        div = 1;
    }
    actual = (ULONGLONG)ClockHz / (16ULL * div);
    diff = (actual > Baud) ? actual - Baud : Baud - actual;
    *ErrorPermille = (ULONG)((diff * 1000ULL) / Baud);
    return (ULONG)div;
}

NTSTATUS
RkuSetBaud(_In_ PRKU_DEVICE Dev, _In_ ULONG Baud)
{
    ULONG div;
    ULONG err;

    if (Baud == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    div = RkuDivisor(Dev->ClockHz, Baud, &err);
    /* more than 3 % off cannot be received by the other end */
    if (div > 0xFFFF || err > 30) {
        RKU_ERR("UART%lu: %lu baud is not reachable from %lu Hz (divisor %lu, %lu.%lu %% off)",
                Dev->UartIndex, Baud, Dev->ClockHz, div, err / 10, err % 10);
        return STATUS_INVALID_PARAMETER;
    }

    WdfInterruptAcquireLock(Dev->Interrupt);
    RkuProgramLocked(Dev, div);
    WdfInterruptReleaseLock(Dev->Interrupt);
    Dev->Baud = Baud;
    RKU_LOG("UART%lu: %lu baud (divisor %lu from %lu Hz, %lu.%lu %% off)",
            Dev->UartIndex, Baud, div, Dev->ClockHz, err / 10, err % 10);
    return STATUS_SUCCESS;
}

NTSTATUS
RkuSetLineControl(_In_ PRKU_DEVICE Dev, _In_ const SERIAL_LINE_CONTROL *Lc)
{
    UCHAR lcr = 0;

    switch (Lc->WordLength) {
    case 5: lcr |= 0; break;
    case 6: lcr |= 1; break;
    case 7: lcr |= 2; break;
    case 8: lcr |= 3; break;
    default: return STATUS_INVALID_PARAMETER;
    }

    switch (Lc->StopBits) {
    case STOP_BIT_1:
        break;
    case STOP_BITS_1_5:     /* the 16550 gives 1.5 with 5 data bits only */
        if (Lc->WordLength != 5) {
            return STATUS_INVALID_PARAMETER;
        }
        lcr |= LCR_STB;
        break;
    case STOP_BITS_2:
        if (Lc->WordLength == 5) {
            return STATUS_INVALID_PARAMETER;
        }
        lcr |= LCR_STB;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    switch (Lc->Parity) {
    case NO_PARITY:    break;
    case ODD_PARITY:   lcr |= LCR_PEN; break;
    case EVEN_PARITY:  lcr |= LCR_PEN | LCR_EPS; break;
    case MARK_PARITY:  lcr |= LCR_PEN | LCR_SP; break;
    case SPACE_PARITY: lcr |= LCR_PEN | LCR_EPS | LCR_SP; break;
    default: return STATUS_INVALID_PARAMETER;
    }

    if (Dev->BreakOn) {
        lcr |= LCR_BC;
    }

    WdfInterruptAcquireLock(Dev->Interrupt);
    Dev->Lcr = lcr;
    RkuProgramLocked(Dev, 0);
    WdfInterruptReleaseLock(Dev->Interrupt);
    Dev->LineControl = *Lc;
    return STATUS_SUCCESS;
}

VOID
RkuSetBreak(_In_ PRKU_DEVICE Dev, _In_ BOOLEAN On)
{
    WdfInterruptAcquireLock(Dev->Interrupt);
    Dev->BreakOn = On;
    if (On) {
        Dev->Lcr |= LCR_BC;
    } else {
        Dev->Lcr &= ~LCR_BC;
    }
    RkuProgramLocked(Dev, 0);
    WdfInterruptReleaseLock(Dev->Interrupt);
}

VOID
RkuSetMcr(_In_ PRKU_DEVICE Dev, _In_ UCHAR Set, _In_ UCHAR Clear)
{
    WdfInterruptAcquireLock(Dev->Interrupt);
    Dev->Mcr = (UCHAR)((Dev->Mcr | Set) & ~Clear);
    RkuWrite(Dev, UART_MCR, Dev->Mcr);
    WdfInterruptReleaseLock(Dev->Interrupt);
}

VOID
RkuPurgeFifos(_In_ PRKU_DEVICE Dev, _In_ BOOLEAN Rx, _In_ BOOLEAN Tx)
{
    ULONG srr = 0;

    if (Rx) {
        srr |= SRR_RFR;
    }
    if (Tx) {
        srr |= SRR_XFR;
    }
    WdfInterruptAcquireLock(Dev->Interrupt);
    if (srr != 0) {
        RkuWrite(Dev, UART_SRR, srr);
    }
    if (Rx) {
        Dev->RxHead = Dev->RxTail = 0;
        (VOID)RkuRead(Dev, UART_LSR);
    }
    WdfInterruptReleaseLock(Dev->Interrupt);
}

/*
 * D0Entry: reset the block and program the line as it was last set. The
 * interrupts stay off until a handle is opened (RkuEvtFileOpen).
 */
VOID
RkuHwInit(_In_ PRKU_DEVICE Dev)
{
    ULONG div;
    ULONG err;
    ULONG cpr;
    ULONG i;

    WdfInterruptAcquireLock(Dev->Interrupt);

    RkuWrite(Dev, UART_IER, 0);
    RkuWrite(Dev, UART_SRR, SRR_UR | SRR_RFR | SRR_XFR);
    KeStallExecutionProcessor(10);

    cpr = RkuRead(Dev, UART_CPR);
    /* CPR[23:16] FIFO_MODE: depth / 16; 0 means no FIFO or no CPR */
    if (((cpr >> 16) & 0xFF) != 0) {
        Dev->FifoDepth = ((cpr >> 16) & 0xFF) * 16;
    }

    RkuWrite(Dev, UART_FCR, FCR_FIFOE | FCR_RFIFOR | FCR_XFIFOR | FCR_RT_QUARTER | FCR_TET_EMPTY);
    div = RkuDivisor(Dev->ClockHz, Dev->Baud, &err);
    RkuProgramLocked(Dev, div);
    RkuWrite(Dev, UART_MCR, Dev->Mcr);

    for (i = 0; i < 2 * RKU_DEFAULT_FIFO && (RkuRead(Dev, UART_LSR) & LSR_DR); i++) {
        (VOID)RkuRead(Dev, UART_RBR);
    }
    (VOID)RkuRead(Dev, UART_LSR);
    (VOID)RkuRead(Dev, UART_MSR);
    (VOID)RkuRead(Dev, UART_USR);
    (VOID)RkuRead(Dev, UART_IIR);

    Dev->RxHead = Dev->RxTail = 0;
    Dev->Ier = 0;

    WdfInterruptReleaseLock(Dev->Interrupt);

    RKU_LOG("UART%lu at %I64x: %lu Hz, FIFO %lu, CPR 0x%08lx UCV 0x%08lx, %lu baud",
            Dev->UartIndex, Dev->RegsPhys.QuadPart, Dev->ClockHz, Dev->FifoDepth,
            cpr, RkuRead(Dev, UART_UCV), Dev->Baud);
}

VOID
RkuHwQuiesce(_In_ PRKU_DEVICE Dev)
{
    if (Dev->Regs == NULL) {
        return;
    }
    WdfInterruptAcquireLock(Dev->Interrupt);
    Dev->Ier = 0;
    RkuWrite(Dev, UART_IER, 0);
    RkuWrite(Dev, UART_SRR, SRR_RFR | SRR_XFR);
    (VOID)RkuRead(Dev, UART_LSR);
    (VOID)RkuRead(Dev, UART_USR);
    Dev->RxArmed = Dev->TxArmed = FALSE;
    Dev->RxReadyPending = Dev->TxReadyPending = FALSE;
    Dev->DrainArmed = FALSE;
    WdfInterruptReleaseLock(Dev->Interrupt);
}

/* ------------------------------------------------------------ interrupt */

_Use_decl_annotations_
BOOLEAN
RkuEvtIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    PRKU_DEVICE dev = RkuGetDevice(WdfInterruptGetDevice(Interrupt));
    BOOLEAN claimed = FALSE;
    ULONG guard;
    ULONG iir;

    UNREFERENCED_PARAMETER(MessageId);

    for (guard = 0; guard < 16; guard++) {
        iir = RkuRead(dev, UART_IIR) & IIR_ID_MASK;
        if (iir == IIR_NONE) {
            break;
        }
        claimed = TRUE;

        switch (iir) {
        case IIR_RLS:
        case IIR_RDA:
        case IIR_TIMEOUT:
            (VOID)RkuDrainRxLocked(dev);
            break;

        case IIR_THRE:
            /* reading IIR acknowledged it */
            dev->Ier &= ~IER_ETBEI;
            RkuWrite(dev, UART_IER, dev->Ier);
            if (dev->TxArmed) {
                dev->TxArmed = FALSE;
                dev->TxReadyPending = TRUE;
            }
            dev->WaitEvents |= SERIAL_EV_TXEMPTY;
            break;

        case IIR_BUSY:
            (VOID)RkuRead(dev, UART_USR);
            dev->BusyIrqs++;
            break;

        case IIR_MSI:
            (VOID)RkuRead(dev, UART_MSR);
            break;

        default:
            /* unknown source: read everything that clears one, then stop */
            (VOID)RkuRead(dev, UART_LSR);
            (VOID)RkuRead(dev, UART_USR);
            (VOID)RkuRead(dev, UART_MSR);
            guard = 16;
            break;
        }
    }

    /* a character can land between the last IIR read and here */
    if (claimed && (RkuRead(dev, UART_LSR) & LSR_DR)) {
        (VOID)RkuDrainRxLocked(dev);
    }

    if (dev->RxReadyPending || dev->TxReadyPending ||
        (dev->WaitEvents & dev->WaitMask) != 0) {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
    return claimed;
}

_Use_decl_annotations_
VOID
RkuEvtDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    PRKU_DEVICE dev = RkuGetDevice(WdfInterruptGetDevice(Interrupt));
    BOOLEAN rx;
    BOOLEAN tx;
    ULONG events;

    UNREFERENCED_PARAMETER(AssociatedObject);

    WdfInterruptAcquireLock(Interrupt);
    rx = dev->RxReadyPending;
    tx = dev->TxReadyPending;
    dev->RxReadyPending = FALSE;
    dev->TxReadyPending = FALSE;
    events = dev->WaitEvents & dev->WaitMask;
    dev->WaitEvents = 0;
    WdfInterruptReleaseLock(Interrupt);

    if (rx) {
        SerCx2PioReceiveReady(dev->PioReceive);
    }
    if (tx) {
        SerCx2PioTransmitReady(dev->PioTransmit);
    }
    if (events != 0) {
        SerCx2CompleteWait(dev->Device, events);
    }
}

/* ------------------------------------------------------------ PIO receive */

_Use_decl_annotations_
ULONG
RkuEvtRxReadBuffer(SERCX2PIORECEIVE PioReceive, PUCHAR Buffer, ULONG Length)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioReceive)->Dev;
    ULONG n = 0;

    WdfInterruptAcquireLock(dev->Interrupt);
    /* take what is in the FIFO now too, so a short read is not a stale one */
    (VOID)RkuDrainRxLocked(dev);
    dev->RxReadyPending = FALSE;
    while (n < Length && dev->RxTail != dev->RxHead) {
        Buffer[n++] = dev->RxRing[dev->RxTail];
        dev->RxTail = (dev->RxTail + 1) & (RKU_RX_RING_SIZE - 1);
    }
    WdfInterruptReleaseLock(dev->Interrupt);
    return n;
}

_Use_decl_annotations_
VOID
RkuEvtRxEnableReady(SERCX2PIORECEIVE PioReceive)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioReceive)->Dev;
    BOOLEAN now;

    WdfInterruptAcquireLock(dev->Interrupt);
    (VOID)RkuDrainRxLocked(dev);
    now = (RingCount(dev) != 0);
    if (now) {
        dev->RxReadyPending = TRUE;
    } else {
        dev->RxArmed = TRUE;
    }
    WdfInterruptReleaseLock(dev->Interrupt);

    if (now) {
        WdfInterruptQueueDpcForIsr(dev->Interrupt);
    }
}

_Use_decl_annotations_
BOOLEAN
RkuEvtRxCancelReady(SERCX2PIORECEIVE PioReceive)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioReceive)->Dev;
    BOOLEAN cancelled;

    WdfInterruptAcquireLock(dev->Interrupt);
    cancelled = dev->RxArmed || dev->RxReadyPending;
    dev->RxArmed = FALSE;
    dev->RxReadyPending = FALSE;
    WdfInterruptReleaseLock(dev->Interrupt);
    return cancelled;
}

/* ------------------------------------------------------------ PIO transmit */

_Use_decl_annotations_
ULONG
RkuEvtTxWriteBuffer(SERCX2PIOTRANSMIT PioTransmit, PUCHAR Buffer, ULONG Length)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioTransmit)->Dev;
    ULONG n = 0;

    WdfInterruptAcquireLock(dev->Interrupt);
    while (n < Length && n < dev->FifoDepth &&
           (RkuRead(dev, UART_USR) & USR_TFNF)) {
        RkuWrite(dev, UART_THR, Buffer[n++]);
    }
    WdfInterruptReleaseLock(dev->Interrupt);
    return n;
}

_Use_decl_annotations_
VOID
RkuEvtTxEnableReady(SERCX2PIOTRANSMIT PioTransmit)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioTransmit)->Dev;
    BOOLEAN now;

    WdfInterruptAcquireLock(dev->Interrupt);
    now = (RkuRead(dev, UART_USR) & USR_TFNF) != 0 &&
          RkuRead(dev, UART_TFL) < dev->FifoDepth / 2;
    if (now) {
        dev->TxReadyPending = TRUE;
    } else {
        dev->TxArmed = TRUE;
        dev->Ier |= IER_ETBEI;
        RkuWrite(dev, UART_IER, dev->Ier);
    }
    WdfInterruptReleaseLock(dev->Interrupt);

    if (now) {
        WdfInterruptQueueDpcForIsr(dev->Interrupt);
    }
}

_Use_decl_annotations_
BOOLEAN
RkuEvtTxCancelReady(SERCX2PIOTRANSMIT PioTransmit)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioTransmit)->Dev;
    BOOLEAN cancelled;

    WdfInterruptAcquireLock(dev->Interrupt);
    cancelled = dev->TxArmed || dev->TxReadyPending;
    dev->TxArmed = FALSE;
    dev->TxReadyPending = FALSE;
    dev->Ier &= ~IER_ETBEI;
    RkuWrite(dev, UART_IER, dev->Ier);
    WdfInterruptReleaseLock(dev->Interrupt);
    return cancelled;
}

/*
 * Drain: SerCx2 wants to know when the last bit has left the shifter (before
 * a line change, or to complete a flush). LSR.TEMT says so; it raises no
 * interrupt of its own, so poll it every millisecond.
 */
_Use_decl_annotations_
VOID
RkuEvtTxDrainFifo(SERCX2PIOTRANSMIT PioTransmit)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioTransmit)->Dev;

    WdfInterruptAcquireLock(dev->Interrupt);
    dev->DrainArmed = TRUE;
    WdfInterruptReleaseLock(dev->Interrupt);
    WdfTimerStart(dev->DrainTimer, WDF_REL_TIMEOUT_IN_MS(1));
}

_Use_decl_annotations_
BOOLEAN
RkuEvtTxCancelDrainFifo(SERCX2PIOTRANSMIT PioTransmit)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioTransmit)->Dev;
    BOOLEAN cancelled;

    WdfInterruptAcquireLock(dev->Interrupt);
    cancelled = dev->DrainArmed;
    dev->DrainArmed = FALSE;
    WdfInterruptReleaseLock(dev->Interrupt);
    (VOID)WdfTimerStop(dev->DrainTimer, FALSE);
    return cancelled;
}

_Use_decl_annotations_
VOID
RkuEvtDrainTimer(WDFTIMER Timer)
{
    PRKU_DEVICE dev = RkuGetDevice(WdfTimerGetParentObject(Timer));
    BOOLEAN done = FALSE;

    WdfInterruptAcquireLock(dev->Interrupt);
    if (dev->DrainArmed &&
        (RkuRead(dev, UART_LSR) & LSR_TEMT) != 0 &&
        RkuRead(dev, UART_TFL) == 0) {
        dev->DrainArmed = FALSE;
        done = TRUE;
    }
    if (!dev->DrainArmed && !done) {
        WdfInterruptReleaseLock(dev->Interrupt);
        return;                     /* cancelled */
    }
    WdfInterruptReleaseLock(dev->Interrupt);

    if (done) {
        SerCx2PioTransmitDrainFifoComplete(dev->PioTransmit);
    } else {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1));
    }
}

_Use_decl_annotations_
VOID
RkuEvtTxPurgeFifo(SERCX2PIOTRANSMIT PioTransmit, ULONG BytesAlreadyTransmittedToHardware)
{
    PRKU_DEVICE dev = RkuGetPioContext(PioTransmit)->Dev;
    ULONG left;

    WdfInterruptAcquireLock(dev->Interrupt);
    left = RkuRead(dev, UART_TFL);
    RkuWrite(dev, UART_SRR, SRR_XFR);
    WdfInterruptReleaseLock(dev->Interrupt);

    if (left > BytesAlreadyTransmittedToHardware) {
        left = BytesAlreadyTransmittedToHardware;
    }
    SerCx2PioTransmitPurgeFifoComplete(PioTransmit, left);
}
