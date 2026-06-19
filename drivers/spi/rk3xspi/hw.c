/*++

Module Name:

    hw.c

Abstract:

    Rockchip SPI transfer engine (polled, full-duplex). Verified against the
    kernel driver drivers/spi/spi-rockchip.c.

    Every transfer runs in TR (transmit & receive) mode: each clocked frame
    sends one byte and receives one. Read = clock zeros and keep RX; Write =
    send TX and drop RX; full-duplex = both. CS is asserted via SER for the
    whole transfer and released at the end.

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

#include "rk3xspi.h"

_Use_decl_annotations_
ULONG
Rk3xSpiComputeBaudr(
    ULONG InputClockHz,
    ULONG TargetHz
    )
{
    ULONG baudr;

    if (TargetHz == 0) {
        TargetHz = RK3XSPI_DEFAULT_HZ;
    }

    //
    // sclk = input / BAUDR; round the divider up (so we never exceed the
    // requested rate) and to the next even value (BAUDR must be even).
    //
    baudr = (InputClockHz + TargetHz - 1) / TargetHz;
    if (baudr < RKSPI_BAUDR_MIN) {
        baudr = RKSPI_BAUDR_MIN;
    }
    baudr = (baudr + 1) & ~1u;
    if (baudr > RKSPI_BAUDR_MAX) {
        baudr = RKSPI_BAUDR_MAX;
    }
    return baudr;
}

_Use_decl_annotations_
VOID
Rk3xSpiHwInit(
    PRK3XSPI_CONTEXT Ctx
    )
{
    RkSpiWrite(Ctx->Regs, RKSPI_SSIENR, 0);   // disabled until a transfer
    RkSpiWrite(Ctx->Regs, RKSPI_IMR, 0);      // polled: mask all interrupts
    RkSpiWrite(Ctx->Regs, RKSPI_DMACR, 0);    // no DMA
}

static
BOOLEAN
Rk3xSpiTimedOut(
    _In_ LARGE_INTEGER Start,
    _In_ LARGE_INTEGER Freq
    )
{
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);
    return ((now.QuadPart - Start.QuadPart) * 1000000LL / Freq.QuadPart) >
           (LONGLONG)RK3XSPI_XFER_TIMEOUT_US;
}

_Use_decl_annotations_
NTSTATUS
Rk3xSpiTransfer(
    PRK3XSPI_CONTEXT Ctx,
    PRK3XSPI_TARGET Target,
    PUCHAR Tx,
    PUCHAR Rx,
    ULONG Length
    )
{
    volatile UCHAR *regs = Ctx->Regs;
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER start;
    LARGE_INTEGER freq;
    ULONG cr0;
    ULONG done = 0;

    if (Length == 0) {
        return STATUS_SUCCESS;
    }

    //
    // Configure frame format / mode once (controller must be disabled to write
    // the config registers).
    //
    RkSpiWrite(regs, RKSPI_SSIENR, 0);

    cr0 = RKSPI_CR0_DFS_8BIT | RKSPI_CR0_FRF_SPI | RKSPI_CR0_XFM_TR |
          RKSPI_CR0_OPM_MASTER | RKSPI_CR0_FBM_MSB | RKSPI_CR0_CSM_KEEP;
    if (Target->CpolHigh) {
        cr0 |= RKSPI_CR0_SCPOL;
    }
    if (Target->CphaSecond) {
        cr0 |= RKSPI_CR0_SCPH;
    }
    RkSpiWrite(regs, RKSPI_CTRLR0, cr0);
    RkSpiWrite(regs, RKSPI_BAUDR, Target->Baudr);

    start = KeQueryPerformanceCounter(&freq);

    while (done < Length) {
        ULONG chunk = Length - done;
        ULONG txi = 0;
        ULONG rxi = 0;

        if (chunk > RKSPI_MAX_FRAMES) {
            chunk = RKSPI_MAX_FRAMES;
        }

        RkSpiWrite(regs, RKSPI_SSIENR, 0);
        RkSpiWrite(regs, RKSPI_CTRLR1, chunk - 1);
        RkSpiWrite(regs, RKSPI_SSIENR, RKSPI_SSIENR_ENABLE);
        RkSpiWrite(regs, RKSPI_SER, RKSPI_SER_CS(Target->ChipSelect)); // assert CS + start

        while (rxi < chunk) {
            ULONG sr;

            //
            // Push TX while the FIFO has room and we stay <= FIFO_DEPTH ahead of
            // RX (each TX frame produces an RX frame; keeping the gap bounded
            // prevents RX overflow).
            //
            while (txi < chunk &&
                   (txi - rxi) < RKSPI_FIFO_DEPTH &&
                   !(RkSpiRead(regs, RKSPI_SR) & RKSPI_SR_TF_FULL)) {
                UCHAR b = (Tx != NULL) ? Tx[done + txi] : 0;
                RkSpiWrite(regs, RKSPI_TXDR, b);
                txi += 1;
            }

            //
            // Drain whatever RX is available.
            //
            sr = RkSpiRead(regs, RKSPI_SR);
            while (rxi < chunk && !(sr & RKSPI_SR_RF_EMPTY)) {
                ULONG v = RkSpiRead(regs, RKSPI_RXDR);
                if (Rx != NULL) {
                    Rx[done + rxi] = (UCHAR)v;
                }
                rxi += 1;
                sr = RkSpiRead(regs, RKSPI_SR);
            }

            if (Rk3xSpiTimedOut(start, freq)) {
                status = STATUS_IO_TIMEOUT;
                goto Done;
            }
        }

        //
        // Wait for the shift engine to go idle before the next chunk.
        //
        while (RkSpiRead(regs, RKSPI_SR) & RKSPI_SR_BUSY) {
            if (Rk3xSpiTimedOut(start, freq)) {
                status = STATUS_IO_TIMEOUT;
                goto Done;
            }
        }

        done += chunk;
    }

Done:
    RkSpiWrite(regs, RKSPI_SER, 0);       // release CS
    RkSpiWrite(regs, RKSPI_SSIENR, 0);    // disable
    return status;
}
