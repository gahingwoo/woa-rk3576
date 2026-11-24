/*
 * bwl_bringup.c - from a powered SDIO card to running firmware: the
 * attach sequence, the firmware and NVRAM download, the start of the
 * firmware (HT clock, F2, mailbox), and the sdpcm_shared/console reads.
 *
 * The order of the register writes is OpenBSD bwfm's (if_bwfm_sdio.c:
 * bwfm_sdio_attach, bwfm_sdio_preinit, bwfm_sdio_load_microcode,
 * bwfm_sdio_readshared, bwfm_sdio_debug_console), with the F2 ready wait
 * from Linux brcmfmac (brcmf_sdio_firmware_callback).
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

int
BwlAttach(bwl_dev *Dev)
{
    const bwl_core *core;
    u8 b;
    u32 v;
    int r;

    if (Dev->Bounce == NULL || Dev->BounceSize < BWL_BOUNCE_MIN) {
        return BWL_E_SPACE;
    }

    Dev->Step = "block size";
    r = BwlSetBlockSize(Dev, 1, Dev->F1BlockSize);
    if (r == 0) {
        r = BwlSetBlockSize(Dev, 2, Dev->F2BlockSize);
    }
    if (r) {
        return r;
    }

    Dev->Step = "F1 enable";
    r = BwlFuncEnable(Dev, 1, 1, 500);
    if (r) {
        return r;
    }

    /* Force the PLL off and ask for ALP. */
    Dev->Step = "ALP request";
    if (BwlWrite8(Dev, F1_CHIPCLKCSR, CLKCSR_FORCE_HW_CLKREQ_OFF | CLKCSR_ALP_AVAIL_REQ)) {
        return BWL_E_IO;
    }

    r = BwlChipAttach(Dev);
    if (r) {
        return r;
    }

    /* SDIO core rev 12 and later: keep the SDIO interface on (KSO). */
    Dev->Step = "KSO";
    core = BwlChipCore(Dev, CORE_SDIO_DEV, 0);
    if (core->Rev >= 12) {
        if (BwlRead8(Dev, F1_SLEEPCSR, &b)) {
            return BWL_E_IO;
        }
        if (!(b & SLEEPCSR_KSO) && BwlWrite8(Dev, F1_SLEEPCSR, (u8)(b | SLEEPCSR_KSO))) {
            return BWL_E_IO;
        }
    }

    Dev->Step = "WLAN reset";
    if (BwlRead8(Dev, CCCR_BRCM_CARDCTRL, &b) ||
        BwlWrite8(Dev, CCCR_BRCM_CARDCTRL, (u8)(b | CARDCTRL_WLANRESET))) {
        return BWL_E_IO;
    }

    Dev->Step = "PMU reload";
    core = BwlChipPmu(Dev);
    if (BwlRead32(Dev, core->Base + CC_PMUCONTROL, &v) ||
        BwlWrite32(Dev, core->Base + CC_PMUCONTROL,
                   v | (PMUCONTROL_RES_RELOAD << PMUCONTROL_RES_SHIFT))) {
        return BWL_E_IO;
    }

    Dev->Step = "F2 disable";
    r = BwlFuncEnable(Dev, 2, 0, 0);
    if (r) {
        return r;
    }
    if (BwlWrite8(Dev, F1_CHIPCLKCSR, 0)) {
        return BWL_E_IO;
    }
    Dev->ClkState = BWL_CLK_SDONLY;
    Dev->SrCapable = (u8)BwlChipSrCapable(Dev);
    Dev->Step = "attached";
    return BWL_OK;
}

/* Compare chip RAM with Ref, a window at a time, through the bounce buffer. */
static int
Verify(bwl_dev *Dev, u32 Addr, const u8 *Ref, u32 Len)
{
    u32 off = 0;

    while (off < Len) {
        u32 n = Len - off;
        int r;

        if (n > SB_WINDOW_SIZE) {
            n = SB_WINDOW_SIZE;
        }
        /* BwlRamRw reads into the bounce buffer and copies out; read into
         * the upper half of the bounce buffer so both fit. */
        r = BwlRamRw(Dev, Addr + off, Dev->Bounce + SB_WINDOW_SIZE, n, 0);
        if (r) {
            return r;
        }
        if (BwlMemCmp(Dev->Bounce + SB_WINDOW_SIZE, Ref + off, n)) {
            Dev->Detail = Addr + off;
            return BWL_E_VERIFY;
        }
        off += n;
    }
    return BWL_OK;
}

int
BwlDownload(bwl_dev *Dev, const u8 *Fw, u32 FwLen, const u8 *Nvram, u32 NvramLen, int Verify_)
{
    u32 nvaddr;
    int r;

    if (Dev->RamSize == 0 || Dev->ClkState == BWL_CLK_NONE) {
        return BWL_E_STATE;
    }
    r = BwlFwImageCheck(Fw, FwLen, Dev->RamSize);
    if (r) {
        return r;
    }
    if (NvramLen == 0 || (NvramLen & 3) != 0 || FwLen + NvramLen > Dev->RamSize) {
        return BWL_E_SPACE;
    }
    nvaddr = Dev->RamBase + Dev->RamSize - NvramLen;

    Dev->AlpOnly = 1;
    Dev->Step = "ALP clock";
    BwlClkCtl(Dev, BWL_CLK_AVAIL);
    if (Dev->ClkState != BWL_CLK_AVAIL) {
        Dev->AlpOnly = 0;
        return BWL_E_TIMEOUT;
    }

    /* BwlRamRw copies out of the caller's buffer; it is never written. */
    Dev->Step = "firmware write";
    r = BwlRamRw(Dev, Dev->RamBase, (u8 *)Fw, FwLen, 1);
    if (r == 0 && Verify_) {
        Dev->Step = "firmware verify";
        r = Verify(Dev, Dev->RamBase, Fw, FwLen);
    }
    if (r == 0) {
        Dev->Step = "nvram write";
        r = BwlRamRw(Dev, nvaddr, (u8 *)Nvram, NvramLen, 1);
    }
    if (r == 0 && Verify_) {
        Dev->Step = "nvram verify";
        r = Verify(Dev, nvaddr, Nvram, NvramLen);
    }
    if (r == 0) {
        Dev->Step = "cpu release";
        r = BwlChipSetActive(Dev, BwlGetLe32(Fw));
    }
    BwlClkCtl(Dev, BWL_CLK_SDONLY);
    Dev->AlpOnly = 0;
    if (r == 0) {
        Dev->Step = "downloaded";
    }
    return r;
}

int
BwlStart(bwl_dev *Dev, u32 F2TimeoutMs)
{
    u8 clk, b;
    int r;

    if (!Dev->FwRunning) {
        return BWL_E_STATE;
    }
    Dev->Step = "HT clock";
    BwlClkCtl(Dev, BWL_CLK_AVAIL);
    if (Dev->ClkState != BWL_CLK_AVAIL) {
        return BWL_E_TIMEOUT;
    }
    if (BwlRead8(Dev, F1_CHIPCLKCSR, &clk) ||
        BwlWrite8(Dev, F1_CHIPCLKCSR, (u8)(clk | CLKCSR_FORCE_HT))) {
        return BWL_E_IO;
    }

    Dev->Step = "mailbox version";
    if (BwlSdioCoreWrite(Dev, SDCORE_TOSBMAILBOXDATA, SDPCM_PROT_VERSION << SDPCM_PROT_VERSION_SHIFT)) {
        return BWL_E_IO;
    }

    /* The firmware raises F2 ready once it has initialised. */
    Dev->Step = "F2 ready";
    r = BwlFuncEnable(Dev, 2, 1, F2TimeoutMs);
    if (r) {
        return r;
    }

    Dev->Step = "interrupt mask";
    if (BwlSdioCoreWrite(Dev, SDCORE_HOSTINTMASK, INTSTATUS_HMB_SW_MASK | INTSTATUS_CHIPACTIVE) ||
        BwlWrite8(Dev, F1_WATERMARK, F2_WATERMARK_DEFAULT)) {
        return BWL_E_IO;
    }

    Dev->Step = "save/restore";
    if (Dev->SrCapable) {
        if (BwlRead8(Dev, F1_WAKEUPCTRL, &b) ||
            BwlWrite8(Dev, F1_WAKEUPCTRL, (u8)(b | WAKEUPCTRL_HTWAIT)) ||
            BwlWrite8(Dev, CCCR_BRCM_CARDCAP, CARDCAP_CMD14_SUPPORT | CARDCAP_CMD14_EXT) ||
            BwlWrite8(Dev, F1_CHIPCLKCSR, CLKCSR_FORCE_HT)) {
            return BWL_E_IO;
        }
        Dev->SrEnabled = 1;
    } else if (BwlWrite8(Dev, F1_CHIPCLKCSR, clk)) {
        return BWL_E_IO;
    }
    Dev->Step = "started";
    return BWL_OK;
}

int
BwlReadShared(bwl_dev *Dev)
{
    u8 b[SHARED_SIZE];
    u32 shaddr, addr;
    int r;

    shaddr = Dev->RamBase + Dev->RamSize - 4;
    if (Dev->RamBase == 0 && Dev->SrCapable) {
        shaddr -= Dev->SrSize;
    }
    r = BwlRamRw(Dev, shaddr, b, 4, 0);
    if (r) {
        return r;
    }
    addr = BwlGetLe32(b);
    /* Still the NVRAM length token: the firmware has not written it yet. */
    if (addr == 0 || ((~addr >> 16) & 0xFFFFu) == (addr & 0xFFFFu)) {
        return BWL_E_STATE;
    }
    if (addr < Dev->RamBase || addr >= Dev->RamBase + Dev->RamSize) {
        Dev->Detail = addr;
        return BWL_E_FORMAT;
    }
    r = BwlRamRw(Dev, addr, b, SHARED_SIZE, 0);
    if (r) {
        return r;
    }
    Dev->SharedAddr = addr;
    Dev->Console.Addr = BwlGetLe32(b + SHARED_CONSOLE_ADDR);
    if (Dev->Console.Addr == 0) {
        return BWL_OK;
    }
    r = BwlRamRw(Dev, Dev->Console.Addr, b, 0x14, 0);
    if (r) {
        return r;
    }
    Dev->Console.LogBuf = BwlGetLe32(b + CONSOLE_LOG_BUF);
    Dev->Console.LogBufSize = BwlGetLe32(b + CONSOLE_LOG_BUFSZ);
    Dev->Console.LogIdx = BwlGetLe32(b + CONSOLE_LOG_IDX);
    return BWL_OK;
}

u32
BwlConsoleRead(bwl_dev *Dev, char *Out, u32 Cap)
{
    u32 size = Dev->Console.LogBufSize, idx, start, n, i, len = 0;
    u8 *buf = Dev->Bounce + SB_WINDOW_SIZE;
    u8 b[4];

    if (Cap == 0) {
        return 0;
    }
    Out[0] = 0;
    if (Dev->Console.Addr == 0 || Dev->Console.LogBuf == 0 || size == 0 || size > SB_WINDOW_SIZE) {
        return 0;
    }
    if (BwlRamRw(Dev, Dev->Console.Addr + CONSOLE_LOG_IDX, b, 4, 0)) {
        return 0;
    }
    idx = BwlGetLe32(b) % size;
    Dev->Console.LogIdx = idx;
    if (BwlRamRw(Dev, Dev->Console.LogBuf, buf, size, 0)) {
        return 0;
    }
    /* The ring's oldest byte is at idx. Keep the newest Cap-1 bytes. */
    n = size;
    if (n > Cap - 1) {
        n = Cap - 1;
    }
    start = (idx + size - n) % size;
    for (i = 0; i < n; i++) {
        u8 c = buf[(start + i) % size];

        if (c == 0) {
            continue;
        }
        Out[len++] = (char)((c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7F)) ? c : '?');
    }
    Out[len] = 0;
    return len;
}
