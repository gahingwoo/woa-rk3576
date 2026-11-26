/*
 * bwl_sdio.c - register and memory access over SDIO for Broadcom FullMAC
 * chips: CMD52 bytes, CMD53 transfers split into byte and block mode, the
 * function 1 backplane window, chip RAM, the F2 frame FIFO, and the chip
 * clock request.
 *
 * Address decoding, the backplane window and the clock state machine are
 * those of OpenBSD bwfm (if_bwfm_sdio.c: bwfm_sdio_read_1/_4,
 * bwfm_sdio_backplane, bwfm_sdio_ram_read_write, bwfm_sdio_htclk).
 *
 * Copyright (c) 2010-2016 Broadcom Corporation
 * Copyright (c) 2016,2017 Patrick Wildt <patrick@blueri.se>
 * (ISC licence; the full notice is in bwl_regs.h)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bwl.h"
#include "bwl_regs.h"

void
BwlMemCpy(void *Dst, const void *Src, u32 Len)
{
    u8 *d = (u8 *)Dst;
    const u8 *s = (const u8 *)Src;

    while (Len--) {
        *d++ = *s++;
    }
}

void
BwlMemSet(void *Dst, u8 Val, u32 Len)
{
    u8 *d = (u8 *)Dst;

    while (Len--) {
        *d++ = Val;
    }
}

int
BwlMemCmp(const void *A, const void *B, u32 Len)
{
    const u8 *a = (const u8 *)A, *b = (const u8 *)B;

    for (; Len; Len--, a++, b++) {
        if (*a != *b) {
            return *a < *b ? -1 : 1;
        }
    }
    return 0;
}

u32
BwlStrLen(const char *S)
{
    u32 n = 0;

    while (S[n]) {
        n++;
    }
    return n;
}

void
BwlInit(bwl_dev *Dev, const bwl_bus_ops *Ops, void *Ctx, u8 *Bounce, u32 BounceSize)
{
    BwlMemSet(Dev, 0, (u32)sizeof(*Dev));
    Dev->Ops = Ops;
    Dev->Ctx = Ctx;
    Dev->Bounce = Bounce;
    Dev->BounceSize = BounceSize;
    Dev->F1BlockSize = 64;
    Dev->F2BlockSize = 512;
    Dev->MaxBlocks = 32;
    Dev->F2ByteModeOnly = 1;
    Dev->Window = 0xFFFFFFFFu;
    Dev->ClkState = BWL_CLK_NONE;
    /* bwfm and brcmfmac both start one below zero: the first frame is 255. */
    Dev->TxSeq = 0xFF;
    Dev->TxMaxSeq = 0;
    Dev->Step = "init";
}

static void
Delay(bwl_dev *Dev, u32 Us)
{
    Dev->Ops->delay_us(Dev->Ctx, Us);
}

/* 0x000..0x7FF is function 0 (CCCR, FBR); the rest is function 1. */
static unsigned
FnFor(u32 Addr)
{
    return (Addr & ~0x7FFu) == 0 ? 0u : 1u;
}

int
BwlRead8(bwl_dev *Dev, u32 Addr, u8 *Val)
{
    *Val = 0;
    return Dev->Ops->cmd52(Dev->Ctx, FnFor(Addr), Addr, 0, Val) ? BWL_E_IO : BWL_OK;
}

int
BwlWrite8(bwl_dev *Dev, u32 Addr, u8 Val)
{
    return Dev->Ops->cmd52(Dev->Ctx, FnFor(Addr), Addr, 1, &Val) ? BWL_E_IO : BWL_OK;
}

static int
SetWindow(bwl_dev *Dev, u32 Addr)
{
    u32 bar0 = Addr & ~SB_WINDOW_MASK;

    if (Dev->Window == bar0) {
        return BWL_OK;
    }
    /* Invalidate first, so that a failed update is not trusted later. */
    Dev->Window = 0xFFFFFFFFu;
    if (BwlWrite8(Dev, F1_SBADDRLOW, (u8)((bar0 >> 8) & 0x80)) ||
        BwlWrite8(Dev, F1_SBADDRMID, (u8)((bar0 >> 16) & 0xFF)) ||
        BwlWrite8(Dev, F1_SBADDRHIGH, (u8)((bar0 >> 24) & 0xFF))) {
        return BWL_E_IO;
    }
    Dev->Window = bar0;
    return BWL_OK;
}

int
BwlXfer(bwl_dev *Dev, unsigned Fn, u32 Addr, int Incr, int Write, u8 *Buf, u32 Len)
{
    u32 bs = Fn == 2 ? Dev->F2BlockSize : Dev->F1BlockSize;
    int allowBlock = !(Fn == 2 && Dev->F2ByteModeOnly);
    u32 maxBlocks = Dev->MaxBlocks;

    if (bs == 0 || bs > 2048 || (Len & 3) != 0 || maxBlocks == 0) {
        return BWL_E_STATE;
    }
    if (maxBlocks > 511) {
        maxBlocks = 511;
    }
    while (Len) {
        u32 n;
        int block;

        if (allowBlock && Len >= bs) {
            u32 blocks = Len / bs;

            if (blocks > maxBlocks) {
                blocks = maxBlocks;
            }
            n = blocks * bs;
            block = 1;
        } else {
            /* Byte mode carries at most one block, and at most 512 bytes. */
            n = Len;
            if (n > bs) {
                n = bs;
            }
            if (n > 512) {
                n = 512;
            }
            block = 0;
        }
        if (Dev->Ops->cmd53(Dev->Ctx, Fn, Addr, Incr, Write, block, Buf, n)) {
            return BWL_E_IO;
        }
        Buf += n;
        Len -= n;
        if (Incr) {
            Addr += n;
        }
    }
    return BWL_OK;
}

int
BwlRead32(bwl_dev *Dev, u32 Addr, u32 *Val)
{
    u8 *b = Dev->Bounce;
    int r;

    *Val = 0;
    r = SetWindow(Dev, Addr);
    if (r) {
        return r;
    }
    r = BwlXfer(Dev, 1, (Addr & SB_WINDOW_MASK) | SB_ACCESS_4B, 1, 0, b, 4);
    if (r) {
        return r;
    }
    *Val = BwlGetLe32(b);
    return BWL_OK;
}

int
BwlWrite32(bwl_dev *Dev, u32 Addr, u32 Val)
{
    u8 *b = Dev->Bounce;
    int r;

    r = SetWindow(Dev, Addr);
    if (r) {
        return r;
    }
    BwlPutLe32(b, Val);
    return BwlXfer(Dev, 1, (Addr & SB_WINDOW_MASK) | SB_ACCESS_4B, 1, 1, b, 4);
}

int
BwlRamRw(bwl_dev *Dev, u32 Addr, u8 *Data, u32 Len, int Write)
{
    u32 off = 0;
    int r;

    while (Len) {
        u32 sb = Addr + off;
        u32 sd = sb & SB_WINDOW_MASK;
        u32 n = SB_WINDOW_SIZE - sd;
        u32 padded;

        if (n > Len) {
            n = Len;
        }
        padded = (n + 3) & ~3u;
        if (padded > Dev->BounceSize) {
            return BWL_E_SPACE;
        }
        r = SetWindow(Dev, sb);
        if (r) {
            return r;
        }
        if (Write) {
            BwlMemCpy(Dev->Bounce, Data + off, n);
            BwlMemSet(Dev->Bounce + n, 0, padded - n);
        }
        r = BwlXfer(Dev, 1, sd | SB_ACCESS_4B, 1, Write, Dev->Bounce, padded);
        if (r) {
            return r;
        }
        if (!Write) {
            BwlMemCpy(Data + off, Dev->Bounce, n);
        }
        off += n;
        Len -= n;
    }
    return BWL_OK;
}

int
BwlFrameRw(bwl_dev *Dev, u8 *Data, u32 Len, int Write)
{
    const bwl_core *cc = BwlChipCore(Dev, CORE_CHIPCOMMON, 0);
    u32 base = cc ? cc->Base : CHIP_BASE;
    int r;

    r = SetWindow(Dev, base);
    if (r) {
        return r;
    }
    /* bwfm: writes increment, reads hit the FIFO at a fixed address. */
    return BwlXfer(Dev, 2, (base & SB_WINDOW_MASK) | SB_ACCESS_4B, Write ? 1 : 0, Write, Data, Len);
}

int
BwlFuncEnable(bwl_dev *Dev, unsigned Fn, int Enable, u32 TimeoutMs)
{
    u8 v, bit = (u8)(1u << Fn);
    u32 i;

    if (BwlRead8(Dev, CCCR_IOEX, &v)) {
        return BWL_E_IO;
    }
    v = (u8)(Enable ? (v | bit) : (v & ~bit));
    if (BwlWrite8(Dev, CCCR_IOEX, v)) {
        return BWL_E_IO;
    }
    if (!Enable) {
        return BWL_OK;
    }
    for (i = 0; i <= TimeoutMs; i++) {
        if (BwlRead8(Dev, CCCR_IORX, &v)) {
            return BWL_E_IO;
        }
        if (v & bit) {
            return BWL_OK;
        }
        Delay(Dev, 1000);
    }
    return BWL_E_TIMEOUT;
}

int
BwlSetBlockSize(bwl_dev *Dev, unsigned Fn, u16 Size)
{
    u32 fbr = FBR_BASE(Fn);

    if (BwlWrite8(Dev, fbr + FBR_BLKSIZE0, (u8)Size) ||
        BwlWrite8(Dev, fbr + FBR_BLKSIZE1, (u8)(Size >> 8))) {
        return BWL_E_IO;
    }
    if (Fn == 1) {
        Dev->F1BlockSize = Size;
    } else if (Fn == 2) {
        Dev->F2BlockSize = Size;
    }
    return BWL_OK;
}

int
BwlSdioCoreRead(bwl_dev *Dev, u32 Reg, u32 *Val)
{
    const bwl_core *c = BwlChipCore(Dev, CORE_SDIO_DEV, 0);

    if (c == NULL) {
        *Val = 0;
        return BWL_E_NOCHIP;
    }
    return BwlRead32(Dev, c->Base + Reg, Val);
}

int
BwlSdioCoreWrite(bwl_dev *Dev, u32 Reg, u32 Val)
{
    const bwl_core *c = BwlChipCore(Dev, CORE_SDIO_DEV, 0);

    if (c == NULL) {
        return BWL_E_NOCHIP;
    }
    return BwlWrite32(Dev, c->Base + Reg, Val);
}

/* bwfm_sdio_htclk */
static void
HtClk(bwl_dev *Dev, int On)
{
    u8 req, clk = 0;
    u32 i;

    if (Dev->SrEnabled) {
        Dev->ClkState = On ? BWL_CLK_AVAIL : BWL_CLK_SDONLY;
        return;
    }
    if (!On) {
        Dev->ClkState = BWL_CLK_SDONLY;
        (void)BwlWrite8(Dev, F1_CHIPCLKCSR, 0);
        return;
    }
    req = (u8)(Dev->AlpOnly ? CLKCSR_ALP_AVAIL_REQ : CLKCSR_HT_AVAIL_REQ);
    if (BwlWrite8(Dev, F1_CHIPCLKCSR, req)) {
        return;
    }
    for (i = 0; i < 5000; i++) {
        if (BwlRead8(Dev, F1_CHIPCLKCSR, &clk)) {
            return;
        }
        if (CLK_AV(clk, Dev->AlpOnly)) {
            Dev->ClkState = BWL_CLK_AVAIL;
            return;
        }
        Delay(Dev, 1000);
    }
    Dev->Detail = clk;
}

/* bwfm_sdio_clkctl, without the pending (interrupt-driven) path. */
void
BwlClkCtl(bwl_dev *Dev, int NewState)
{
    if (Dev->ClkState == NewState) {
        return;
    }
    switch (NewState) {
    case BWL_CLK_AVAIL:
        if (Dev->ClkState == BWL_CLK_NONE) {
            Dev->ClkState = BWL_CLK_SDONLY;
        }
        HtClk(Dev, 1);
        break;
    case BWL_CLK_SDONLY:
        if (Dev->ClkState == BWL_CLK_NONE) {
            Dev->ClkState = BWL_CLK_SDONLY;
        } else if (Dev->ClkState == BWL_CLK_AVAIL) {
            HtClk(Dev, 0);
        }
        break;
    case BWL_CLK_NONE:
        if (Dev->ClkState == BWL_CLK_AVAIL) {
            HtClk(Dev, 0);
        }
        Dev->ClkState = BWL_CLK_NONE;
        break;
    default:
        break;
    }
}
