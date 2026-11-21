/*
 * hcd.h - Broadcom patchram (.hcd) file walker
 *
 * A .hcd file is a plain sequence of HCI commands: 2 bytes opcode (little
 * endian), 1 byte parameter length, then the parameters. For the BCM4345C5
 * every record but the last is 0xFC4C (Write RAM) and the last is 0xFC4E
 * (Launch RAM), after which the controller restarts from the new image.
 *
 * Plain C with no Windows or libc dependency, so that the same file builds
 * into the driver and into tools/hcdcheck, which CI runs against the real
 * firmware file on Linux.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define HCD_OP_WRITE_RAM    0xFC4C
#define HCD_OP_LAUNCH_RAM   0xFC4E

typedef struct _HCD_RECORD {
    unsigned short       Opcode;
    unsigned char        Length;
    const unsigned char *Params;
} HCD_RECORD;

typedef struct _HCD_SUMMARY {
    unsigned long Records;
    unsigned long WriteRam;
    unsigned long Other;        /* neither Write RAM nor Launch RAM */
    unsigned long Bytes;        /* parameter bytes in Write RAM records */
    int           EndsWithLaunch;
} HCD_SUMMARY;

/*
 * Returns 1 and fills Rec with the record at *Offset, advancing *Offset;
 * 0 at the clean end of the buffer; -1 if the buffer ends inside a record.
 */
int HcdNext(const unsigned char *Buf, unsigned long Len, unsigned long *Offset, HCD_RECORD *Rec);

/*
 * Walks the whole file. Returns 0 if it is well formed (at least one
 * record, no truncated record, Launch RAM last and nowhere else), else a
 * negative number: -1 truncated, -2 empty, -3 no Launch RAM at the end,
 * -4 Launch RAM before the end.
 */
int HcdValidate(const unsigned char *Buf, unsigned long Len, HCD_SUMMARY *Summary);
