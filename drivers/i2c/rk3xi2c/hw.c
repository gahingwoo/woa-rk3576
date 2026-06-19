/*++

Module Name:

    hw.c

Abstract:

    Rockchip rk3x I2C transfer engine (polled). Translates a list of logical
    I2C messages into the controller's TX/RX/START/STOP register sequence.

    The transfer model and register semantics are verified against the kernel
    driver drivers/i2c/busses/i2c-rk3x.c:
      - TX:   first byte loaded into TXDATA is (addr << 1); MTXCNT triggers send;
              completion = REG_INT_MBTF.
      - RX:   slave address is placed in MRXADDR (the engine appends the R bit in
              RX mode); MRXCNT triggers receive; completion = REG_INT_MBRF; data
              is read from RXDATA. LASTACK makes the engine NACK the final byte.
      - Chunks are <= 32 bytes. START opens each message (a repeated start when
              no STOP was issued since the previous message); STOP ends the list.
      - ACTACK aborts on NACK; REG_INT_NAKRCV reports it.

Environment:

    Kernel mode, PASSIVE_LEVEL (SpbCx dispatches I/O sequentially).

--*/

#include "rk3xi2c.h"

//
// SCL = input / (8 * (CLKDIVL+1 + CLKDIVH+1)).  Pick the total divider for the
// requested bus rate, then split it so the SCL-low half is >= the high half
// (fast-mode wants tLOW > tHIGH). Biasing the *total* up (via ceil) keeps the
// real rate at or below the request.
//
_Use_decl_annotations_
ULONG
Rk3xI2cComputeClkDiv(
    ULONG InputClockHz,
    ULONG BusClockHz
    )
{
    ULONG total;
    ULONG div;
    ULONG divl;
    ULONG divh;

    if (BusClockHz == 0) {
        BusClockHz = RK3XI2C_DEFAULT_BUS_HZ;
    }

    total = (InputClockHz + (BusClockHz * 8) - 1) / (BusClockHz * 8);
    if (total < 2) {
        total = 2;
    }

    div = total - 2;            // because each half contributes (+1)
    divl = div - (div / 2);     // ceil(div/2) -> SCL-low gets the larger share
    divh = div / 2;             // floor(div/2)

    if (divl > 0xFFFF) divl = 0xFFFF;
    if (divh > 0xFFFF) divh = 0xFFFF;

    return ((divh & 0xFFFF) << 16) | (divl & 0xFFFF);
}

_Use_decl_annotations_
VOID
Rk3xI2cHwInit(
    PRK3XI2C_CONTEXT Ctx
    )
{
    //
    // Polled operation: keep all hardware interrupts disabled, the engine off,
    // and the pending register clear.
    //
    Rk3xWrite(Ctx->Regs, RK3X_REG_IEN, 0);
    Rk3xWrite(Ctx->Regs, RK3X_REG_CON, 0);
    Rk3xWrite(Ctx->Regs, RK3X_REG_IPD, RK3X_INT_ALL);
}

//
// Spin-wait until any bit in WaitMask is pending. NACK is always treated as a
// failure. Returns STATUS_IO_TIMEOUT if neither happens in time.
//
static
NTSTATUS
Rk3xWaitIpd(
    _In_ PRK3XI2C_CONTEXT Ctx,
    _In_ ULONG WaitMask,
    _Out_ PULONG IpdOut
    )
{
    LARGE_INTEGER start;
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    ULONG ipd;

    start = KeQueryPerformanceCounter(&freq);

    for (;;) {
        ipd = Rk3xRead(Ctx->Regs, RK3X_REG_IPD);

        if (ipd & RK3X_INT_NAKRCV) {
            *IpdOut = ipd;
            return STATUS_IO_DEVICE_ERROR;          // slave NACK
        }
        if (ipd & WaitMask) {
            *IpdOut = ipd;
            return STATUS_SUCCESS;
        }

        now = KeQueryPerformanceCounter(NULL);
        if (((now.QuadPart - start.QuadPart) * 1000000LL / freq.QuadPart) >
            (LONGLONG)RK3XI2C_XFER_TIMEOUT_US) {
            *IpdOut = ipd;
            return STATUS_IO_TIMEOUT;
        }

        KeStallExecutionProcessor(2);
    }
}

static
NTSTATUS
Rk3xDoWrite(
    _In_ PRK3XI2C_CONTEXT Ctx,
    _In_ PRK3X_I2C_MSG Msg
    )
{
    ULONG processed = 0;
    BOOLEAN firstChunk = TRUE;
    NTSTATUS status;
    ULONG ipd;

    do {
        ULONG cnt = 0;
        ULONG w;

        //
        // Fill up to 32 bytes into the 8 TX words. The very first byte of the
        // message is the (addr << 1) write address; the rest is payload.
        //
        for (w = 0; w < 8; w += 1) {
            ULONG val = 0;
            ULONG b;

            for (b = 0; b < 4; b += 1) {
                UCHAR byte;

                if (firstChunk && cnt == 0) {
                    byte = (UCHAR)((Msg->Address & 0x7F) << 1);   // write
                } else if (processed < Msg->Length) {
                    byte = Msg->Buffer[processed];
                    processed += 1;
                } else {
                    break;
                }

                val |= (ULONG)byte << (b * 8);
                cnt += 1;
            }

            Rk3xWrite(Ctx->Regs, RK3X_REG_TXDATA_BASE + (w * 4), val);

            if (processed >= Msg->Length) {
                break;
            }
        }

        //
        // CON (mode + START) is written only for the first chunk. The hardware
        // auto-clears START after the start condition, and continuation chunks
        // just push more bytes via MTXCNT — matching i2c-rk3x.c.
        //
        if (firstChunk) {
            Rk3xWrite(Ctx->Regs, RK3X_REG_CON,
                      RK3X_CON_EN | RK3X_CON_MOD(RK3X_MOD_TX) |
                      RK3X_CON_ACTACK | RK3X_CON_START);
        }
        Rk3xWrite(Ctx->Regs, RK3X_REG_MTXCNT, cnt);

        status = Rk3xWaitIpd(Ctx, RK3X_INT_MBTF, &ipd);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Rk3xWrite(Ctx->Regs, RK3X_REG_IPD, RK3X_INT_MBTF);

        firstChunk = FALSE;

    } while (processed < Msg->Length);

    return STATUS_SUCCESS;
}

static
NTSTATUS
Rk3xDoRead(
    _In_ PRK3XI2C_CONTEXT Ctx,
    _In_ PRK3X_I2C_MSG Msg
    )
{
    ULONG processed = 0;
    BOOLEAN firstChunk = TRUE;
    NTSTATUS status;
    ULONG ipd;

    //
    // The engine sends the slave address from MRXADDR (R bit appended in RX
    // mode). MRXRADDR is unused (we do plain reads, not register mode).
    //
    Rk3xWrite(Ctx->Regs, RK3X_REG_MRXADDR,
              ((ULONG)(Msg->Address & 0x7F) << 1) | RK3X_MRXADDR_VALID(0));
    Rk3xWrite(Ctx->Regs, RK3X_REG_MRXRADDR, 0);

    while (processed < Msg->Length) {
        ULONG remaining = Msg->Length - processed;
        ULONG chunk = (remaining > RK3X_FIFO_MAX) ? RK3X_FIFO_MAX : remaining;
        ULONG con = RK3X_CON_EN | RK3X_CON_MOD(RK3X_MOD_RX) | RK3X_CON_ACTACK;
        ULONG i;
        ULONG word = 0;

        if (firstChunk) {
            con |= RK3X_CON_START;          // (repeated) start
        }
        if (remaining <= RK3X_FIFO_MAX) {
            con |= RK3X_CON_LASTACK;         // NACK the final byte of the message
        }

        Rk3xWrite(Ctx->Regs, RK3X_REG_CON, con);
        Rk3xWrite(Ctx->Regs, RK3X_REG_MRXCNT, chunk);

        status = Rk3xWaitIpd(Ctx, RK3X_INT_MBRF, &ipd);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Rk3xWrite(Ctx->Regs, RK3X_REG_IPD, RK3X_INT_MBRF);

        for (i = 0; i < chunk; i += 1) {
            if ((i & 3) == 0) {
                word = Rk3xRead(Ctx->Regs, RK3X_REG_RXDATA_BASE + ((i / 4) * 4));
            }
            Msg->Buffer[processed + i] = (UCHAR)(word >> ((i & 3) * 8));
        }

        processed += chunk;
        firstChunk = FALSE;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Rk3xI2cTransfer(
    PRK3XI2C_CONTEXT Ctx,
    ULONG ClkDiv,
    PRK3X_I2C_MSG Msgs,
    ULONG MsgCount,
    PULONG BytesTransferred
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG total = 0;
    ULONG m;
    ULONG con;
    ULONG ipd;

    *BytesTransferred = 0;

    //
    // Program the bus clock and start from a clean, quiet state.
    //
    Rk3xWrite(Ctx->Regs, RK3X_REG_IEN, 0);
    Rk3xWrite(Ctx->Regs, RK3X_REG_CLKDIV, ClkDiv);
    Rk3xWrite(Ctx->Regs, RK3X_REG_IPD, RK3X_INT_ALL);

    for (m = 0; m < MsgCount; m += 1) {
        PRK3X_I2C_MSG msg = &Msgs[m];

        if (msg->Read) {
            status = Rk3xDoRead(Ctx, msg);
        } else {
            status = Rk3xDoWrite(Ctx, msg);
        }

        if (!NT_SUCCESS(status)) {
            break;
        }
        total += msg->Length;
    }

    //
    // Always issue STOP and quiesce the engine, even on error, so the bus is
    // left idle for the next request.
    //
    con = Rk3xRead(Ctx->Regs, RK3X_REG_CON);
    con &= ~RK3X_CON_START;
    con |= RK3X_CON_EN | RK3X_CON_STOP;
    Rk3xWrite(Ctx->Regs, RK3X_REG_CON, con);
    (VOID)Rk3xWaitIpd(Ctx, RK3X_INT_STOP, &ipd);
    Rk3xWrite(Ctx->Regs, RK3X_REG_IPD, RK3X_INT_ALL);
    Rk3xWrite(Ctx->Regs, RK3X_REG_CON, 0);

    if (NT_SUCCESS(status)) {
        *BytesTransferred = total;
    }
    return status;
}
