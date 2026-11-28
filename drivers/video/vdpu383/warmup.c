/*
 * warmup.c - the RK3576 decoder warm-up
 *
 * Rockchip's vendor kernel runs one canned H.264 decode through the link
 * (command list) registers when the decoder probes and every time it
 * resumes: rk3576_workaround_init() and rk3576_workaround_run() in
 * drivers/video/rockchip/mpp/hack/mpp_hack_rk3576.c. No reason is given in
 * the source. Mainline Linux does not do it, and its decoder works on
 * RK3576, so this may well be unnecessary; it is here so that the first
 * Windows test can tell "the block answers" from "the block decodes", and
 * so that it can be turned off to find out.
 *
 * The data and the register sequence are that file's, unchanged:
 *
 *   Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 *   author: Ding Wei <leo.ding@rock-chips.com>
 *   SPDX-License-Identifier: (GPL-2.0+ OR MIT)
 *
 * used here under the GPL-2.0 option. Where the vendor code has an IOMMU
 * address (dec->fix->iova), this has the buffer's physical address: the
 * IOMMU is left with paging off, so the decoder's addresses are physical.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdpu383.h"

#define HACK_DATA_RLC_OFFSET        (0)
#define HACK_DATA_PPS_OFFSET        (HACK_DATA_RLC_OFFSET + 64)
#define HACK_DATA_RPS_OFFSET        (HACK_DATA_PPS_OFFSET + 256)
#define HACK_DATA_RCB_STRMD_OFFSET  (HACK_DATA_RPS_OFFSET + 256)
#define HACK_DATA_RCB_INTRA_OFFSET  (HACK_DATA_RCB_STRMD_OFFSET + 256)
#define HACK_DATA_OUT_OFFSET        (HACK_DATA_RCB_INTRA_OFFSET + 512)
#define HACK_DATA_COLMV_OFFSET      (HACK_DATA_OUT_OFFSET + 1536)

#define HACK_REGS_OFFSET            (4096)
#define HACK_REG_NODE_OFFSET        (HACK_REGS_OFFSET)
#define HACK_REG_SEG0_OFFSET        (HACK_REGS_OFFSET + 32)
#define HACK_REG_SEG1_OFFSET        (HACK_REGS_OFFSET + 256)
#define HACK_REG_SEG2_OFFSET        (HACK_REGS_OFFSET + 512)
#define HACK_REG_DEBUG_OFFSET       (HACK_REGS_OFFSET + 1024)

#define HACK_MAGIC                  (0x76543210)
#define HACK_SIZE                   (2 * PAGE_SIZE)

static const UCHAR HackRlc[32] = {
    0x00, 0x00, 0x01, 0x65,
    0x88, 0x82, 0x0b, 0x01,
    0x2f, 0x08, 0xc5, 0x00,
    0x01, 0x51, 0x78, 0xe0,
    0x00, 0x24, 0xf7, 0x1c,
    0x00, 0x04, 0xcc, 0xeb,
    0x89, 0xd7, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static const UCHAR HackPps[28] = {
    0x40, 0x26, 0x00, 0x10,
    0x04, 0x08, 0x00, 0x08,
    0x80, 0x01, 0x00, 0x00,
    0x00, 0x40, 0x01, 0xd8,
    0x07, 0x7c, 0x7a, 0x00,
    0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00,
};

NTSTATUS
V383WarmupInit(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    PHYSICAL_ADDRESS low, high, skip;
    volatile ULONG *reg;
    ULONG iova;

    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;

    Ctx->WarmVa = MmAllocateContiguousNodeMemory(HACK_SIZE, low, high, skip,
                                                 PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK);
    if (!Ctx->WarmVa) {
        V383_LOG("warm-up buffer allocation failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Ctx->WarmPa = MmGetPhysicalAddress(Ctx->WarmVa);
    iova = (ULONG)Ctx->WarmPa.QuadPart;

    /* PAGE_NOCACHE: Device memory under Windows 10, so aligned accesses
     * only, no RtlZeroMemory or RtlCopyMemory (rk_devmem.h) */
    RkDevZero(Ctx->WarmVa, HACK_SIZE);
    RkDevCopy((PUCHAR)Ctx->WarmVa + HACK_DATA_RLC_OFFSET, HackRlc, sizeof(HackRlc), FALSE);
    RkDevCopy((PUCHAR)Ctx->WarmVa + HACK_DATA_PPS_OFFSET, HackPps, sizeof(HackPps), FALSE);

    /* core registers */
    reg = (volatile ULONG *)((PUCHAR)Ctx->WarmVa + HACK_REGS_OFFSET);
    reg[8]  = 0x00000001;   /* dec_mode */
    reg[13] = 0x0000ffff;   /* core time out */
    reg[16] = 0x00000101;   /* disable error proc */
    reg[20] = 0xffffffff;   /* cabac error low bits */
    reg[21] = 0x3ff3ffff;   /* cabac error high bits */

    reg[64] = HACK_MAGIC;   /* magic number */
    reg[65] = 0x00000000;   /* stream start bit */
    reg[66] = 0x00000020;   /* stream_len */
    reg[67] = 0x000000a8;   /* global_len */
    reg[68] = 0x00000002;   /* hor_virstride */
    reg[69] = 0x00000002;   /* ver_virstride */
    reg[70] = 0x00000040;   /* y_virstride */

    reg[128] = iova + HACK_DATA_RLC_OFFSET;         /* rlc_base */
    reg[129] = iova + HACK_DATA_RPS_OFFSET;         /* rps_base */
    reg[131] = iova + HACK_DATA_PPS_OFFSET;         /* pps_base */
    reg[140] = iova + HACK_DATA_RCB_STRMD_OFFSET;   /* streamd_rcb */
    reg[141] = 0x000000c0;
    reg[148] = iova + HACK_DATA_RCB_INTRA_OFFSET;   /* intra_rcb */
    reg[149] = 0x00000200;
    reg[168] = iova + HACK_DATA_OUT_OFFSET;         /* decout_base */
    reg[216] = iova + HACK_DATA_COLMV_OFFSET;       /* colmv_base */

    /* link node */
    reg = (volatile ULONG *)((PUCHAR)Ctx->WarmVa + HACK_REG_NODE_OFFSET);
    reg[0] = iova + HACK_DATA_OUT_OFFSET;   /* next link node */
    reg[1] = 0x76543212;                    /* table info */
    reg[2] = iova + HACK_REG_DEBUG_OFFSET;  /* debug regs */
    reg[3] = iova + HACK_REG_SEG0_OFFSET;   /* seg0 regs */
    reg[4] = iova + HACK_REG_SEG1_OFFSET;   /* seg1 regs */
    reg[5] = iova + HACK_REG_SEG2_OFFSET;   /* seg2 regs */

    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

VOID
V383WarmupFree(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    if (Ctx->WarmVa) {
        MmFreeContiguousMemory(Ctx->WarmVa);
        Ctx->WarmVa = NULL;
    }
}

VOID
V383WarmupRun(
    _In_ PDEVICE_CONTEXT Ctx,
    _Out_ VDPU383_WARMUP_RESULT *R
    )
{
    ULONG i, irq = 0;

    RtlZeroMemory(R, sizeof(*R));
    if (!Ctx->WarmVa)
        return;
    R->ran = 1;
    R->buffer_dma = (ULONG)Ctx->WarmPa.QuadPart;

    /* disable hardware irq */
    V383Wr(Ctx, 0x58, 0x00008000);

    /* set link registers */
    V383Wr(Ctx, 0x54, 0x0007ffff);                                  /* ip time out */
    V383Wr(Ctx, 0x00, 0x00010001);                                  /* ccu mode */
    V383Wr(Ctx, 0x04, (ULONG)Ctx->WarmPa.QuadPart + HACK_REG_NODE_OFFSET);   /* addr */
    V383Wr(Ctx, 0x08, 0x00000001);                                  /* link mode */
    V383Wr(Ctx, 0x18, 0x00000001);                                  /* link en */

    /* enable hardware */
    __dsb(_ARM64_BARRIER_SY);
    V383Wr(Ctx, 0x0c, 0x01);                                        /* config link */

    /* wait hardware: readl_relaxed_poll_timeout_atomic(0x48, irq_val, irq_val, 1, 500) */
    for (i = 0; i < 500; i++) {
        irq = V383Rd(Ctx, 0x48);
        if (irq)
            break;
        KeStallExecutionProcessor(1);
    }
    R->polls = i;
    R->irq = irq;
    R->timed_out = irq ? 0 : 1;
    R->status = V383Rd(Ctx, 0x4c);

    /* clear irq and status */
    V383Wr(Ctx, 0x48, 0xffff0000);
    V383Wr(Ctx, 0x4c, 0xffff0000);

    /* reset register */
    V383Wr(Ctx, 0x00, 0x00000000);          /* ccu mode */
    V383Wr(Ctx, 0x08, 0x00000000);          /* link mode */
    V383Wr(Ctx, 0x18, 0x00000000);          /* link en */

    /* enable irq */
    V383Wr(Ctx, 0x58, 0x00000000);
    KeStallExecutionProcessor(5);

    V383_LOG("warm-up: %s, irq 0x%08x status 0x%08x after %u polls",
             R->timed_out ? "TIMEOUT" : "done", R->irq, R->status, R->polls);
}
