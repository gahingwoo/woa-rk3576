/*
 * bwl_fw.c - the three firmware files: the firmware image (trailer
 * version, sanity), the NVRAM text (converted to the binary form the
 * firmware reads at the top of RAM) and the CLM blob (cut into "clmload"
 * chunks).
 *
 * BwlNvramConvert follows OpenBSD bwfm's bwfm_nvram_convert and
 * BwlClmChunk its bwfm_process_blob (bwfm.c).
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
BwlNvramConvert(const u8 *Src, u32 SrcLen, u8 *Dst, u32 DstCap)
{
    u32 i, count = 0, len = 0, pad, token;
    int skip = 0;

    if (DstCap < SrcLen + BWL_NVRAM_SLACK) {
        return BWL_E_SPACE;
    }
    for (i = 0; i < SrcLen; i++) {
        u8 c = Src[i];

        if (c == '\n') {
            if (count > 0) {
                Dst[len++] = 0;
            }
            count = 0;
            skip = 0;
            continue;
        }
        if (skip) {
            continue;
        }
        if (c == '#' && count == 0) {
            skip = 1;
            continue;
        }
        if (c == '\r') {
            continue;
        }
        /* Leading blanks would make "  key=v" a different key. */
        if ((c == ' ' || c == '\t') && count == 0) {
            continue;
        }
        if (c == 0) {
            return BWL_E_FORMAT;
        }
        Dst[len++] = c;
        count++;
    }
    if (count > 0) {
        Dst[len++] = 0;     /* last line without a newline */
    }
    if (len == 0) {
        return BWL_E_FORMAT;
    }
    /* bwfm: pad so that at least one NUL follows, to a 4-byte boundary. */
    pad = ((len + 1 + 3) & ~3u) - len;
    BwlMemSet(Dst + len, 0, pad);
    len += pad;
    token = (len / 4) & 0xFFFFu;
    token |= (~token) << 16;
    BwlPutLe32(Dst + len, token);
    len += 4;
    return (int)len;
}

static int
IsVerChar(u8 c)
{
    return (c >= '0' && c <= '9') || c == '.';
}

int
BwlFwImageVersion(const u8 *Img, u32 Len, char *Out, u32 Cap)
{
    /* "Version: " (4345 family trailer) or "Version=" (<FW-TAG> line, 43752) */
    static const char tag[] = "Version: ";
    static const char tag2[] = "Version=";
    const u32 tlen = (u32)sizeof(tag) - 1, tlen2 = (u32)sizeof(tag2) - 1;
    u32 i, n, t;

    if (Cap == 0) {
        return BWL_E_SPACE;
    }
    Out[0] = 0;
    if (Len < tlen2) {
        return BWL_E_FORMAT;
    }
    /* The trailer is a text line at the very end; scan backwards. */
    for (i = Len - tlen2 + 1; i-- > 0;) {
        if (Img[i] != 'V') {
            continue;
        }
        if (i + tlen <= Len && BwlMemCmp(Img + i, tag, tlen) == 0) {
            t = tlen;
        } else if (BwlMemCmp(Img + i, tag2, tlen2) == 0) {
            t = tlen2;
        } else {
            continue;
        }
        if (i + t >= Len || !IsVerChar(Img[i + t])) {
            continue;
        }
        for (n = 0; i + t + n < Len && IsVerChar(Img[i + t + n]) && n + 1 < Cap; n++) {
            Out[n] = (char)Img[i + t + n];
        }
        Out[n] = 0;
        return BWL_OK;
    }
    return BWL_E_FORMAT;
}

int
BwlFwImageCheck(const u8 *Img, u32 Len, u32 RamSize)
{
    if (Img == NULL || Len < 4096) {
        return BWL_E_FORMAT;
    }
    if (RamSize != 0 && Len >= RamSize) {
        return BWL_E_SPACE;
    }
    /* The first word is the CR4's reset vector: a branch, never 0 or ~0. */
    if (BwlGetLe32(Img) == 0 || BwlGetLe32(Img) == 0xFFFFFFFFu) {
        return BWL_E_FORMAT;
    }
    return BWL_OK;
}

int
BwlVerString(const char *Ver, u32 Len, char *Out, u32 Cap)
{
    static const char tag[] = "version ";
    const u32 tlen = (u32)sizeof(tag) - 1;
    u32 i, n;

    if (Cap == 0) {
        return BWL_E_SPACE;
    }
    Out[0] = 0;
    for (i = 0; i + tlen < Len && Ver[i]; i++) {
        if (BwlMemCmp(Ver + i, tag, tlen) == 0 && IsVerChar((u8)Ver[i + tlen])) {
            for (n = 0; i + tlen + n < Len && IsVerChar((u8)Ver[i + tlen + n]) && n + 1 < Cap; n++) {
                Out[n] = Ver[i + tlen + n];
            }
            Out[n] = 0;
            return BWL_OK;
        }
    }
    return BWL_E_FORMAT;
}

int
BwlClmCheck(const u8 *Blob, u32 Len)
{
    /* Every CLM blob seen so far starts with "BLOB". */
    if (Blob == NULL || Len < 16 || BwlMemCmp(Blob, "BLOB", 4) != 0) {
        return BWL_E_FORMAT;
    }
    return BWL_OK;
}

int
BwlClmChunk(const u8 *Blob, u32 Len, u32 Off, u8 *Out, u32 Cap, u32 *Consumed)
{
    u32 n;
    u16 flag = DLOAD_FLAG_HANDLER_VER;

    *Consumed = 0;
    if (Off >= Len) {
        return BWL_E_STATE;
    }
    n = Len - Off;
    if (n > BWL_DLOAD_MAX) {
        n = BWL_DLOAD_MAX;
    }
    if (Cap < DLOAD_HDR_LEN + n) {
        return BWL_E_SPACE;
    }
    if (Off == 0) {
        flag = (u16)(flag | DLOAD_FLAG_BEGIN);
    }
    if (Off + n == Len) {
        flag = (u16)(flag | DLOAD_FLAG_END);
    }
    /* struct bwfm_dload_data: flag, type, len, crc, data[] */
    BwlPutLe16(Out + 0, flag);
    BwlPutLe16(Out + 2, DLOAD_TYPE_CLM);
    BwlPutLe32(Out + 4, n);
    BwlPutLe32(Out + 8, 0);
    BwlMemCpy(Out + DLOAD_HDR_LEN, Blob + Off, n);
    *Consumed = n;
    return (int)(DLOAD_HDR_LEN + n);
}
