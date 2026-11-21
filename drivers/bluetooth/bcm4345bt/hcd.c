/*
 * hcd.c - Broadcom patchram (.hcd) file walker; see hcd.h
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "hcd.h"

int
HcdNext(const unsigned char *Buf, unsigned long Len, unsigned long *Offset, HCD_RECORD *Rec)
{
    unsigned long off = *Offset;

    if (off == Len) {
        return 0;
    }
    if (off > Len || Len - off < 3) {
        return -1;
    }
    Rec->Opcode = (unsigned short)(Buf[off] | (Buf[off + 1] << 8));
    Rec->Length = Buf[off + 2];
    if (Len - off - 3 < Rec->Length) {
        return -1;
    }
    Rec->Params = Buf + off + 3;
    *Offset = off + 3 + Rec->Length;
    return 1;
}

int
HcdValidate(const unsigned char *Buf, unsigned long Len, HCD_SUMMARY *Summary)
{
    unsigned long off = 0;
    HCD_RECORD rec;
    int r;

    Summary->Records = 0;
    Summary->WriteRam = 0;
    Summary->Other = 0;
    Summary->Bytes = 0;
    Summary->EndsWithLaunch = 0;

    while ((r = HcdNext(Buf, Len, &off, &rec)) == 1) {
        if (Summary->EndsWithLaunch) {
            return -4;      /* something after Launch RAM */
        }
        Summary->Records++;
        if (rec.Opcode == HCD_OP_WRITE_RAM) {
            Summary->WriteRam++;
            Summary->Bytes += rec.Length;
        } else if (rec.Opcode == HCD_OP_LAUNCH_RAM) {
            Summary->EndsWithLaunch = 1;
        } else {
            Summary->Other++;
        }
    }
    if (r < 0) {
        return -1;
    }
    if (Summary->Records == 0) {
        return -2;
    }
    if (!Summary->EndsWithLaunch) {
        return -3;
    }
    return 0;
}
