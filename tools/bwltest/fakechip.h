/*
 * fakechip.h - a simulated BCM43456 behind SDIO, enough for bcmwlsdio's
 * portable core (drivers/net/bcmwlsdio/bwl*.c) to attach, download
 * firmware, start it and talk to it on the control channel.
 *
 * It is a model of the parts the driver touches, written from the same
 * register descriptions (bwl_regs.h), not of the chip's internals. What it
 * checks is the host side: the CMD52/CMD53 shapes the driver issues, the
 * order (no F2 before the firmware runs, no CPU release before the image
 * and NVRAM are in RAM), the frame and command encodings.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "../../drivers/net/bcmwlsdio/bwl.h"

#define FAKE_RAMBASE    0x198000u
#define FAKE_RAMSIZE    0xC8000u
#define FAKE_CHIPID     0x15294345u     /* 0x4345 rev 9, AI interconnect */

typedef struct fake_chip {
    /* What the firmware "is": checked against RAM when the CPU starts. */
    const unsigned char *ExpectFw;
    unsigned ExpectFwLen;
    const unsigned char *ExpectNvram;   /* converted form, token included */
    unsigned ExpectNvramLen;
    const unsigned char *ExpectClm;
    unsigned ExpectClmLen;

    /* function 0 and FBRs, function 1 registers */
    unsigned char Cccr[0x100];
    unsigned short BlkSize[3];
    unsigned char ClkCsr;
    unsigned char SleepCsr;
    unsigned char Watermark;
    unsigned char WakeupCtrl;
    unsigned char FrameCtrl;
    unsigned Window;

    /* backplane */
    unsigned Ioctl[8], ResetCtl[8];     /* per core wrapper */
    unsigned BankIdx;
    unsigned PmuControl, ChipCtlAddr;
    unsigned IntStatus, HostIntMask, ToSbMbData, ToHostMbData;
    unsigned ResetVector;
    unsigned char *Ram;

    /* firmware state */
    int Running;
    int BootError;          /* why the image was refused, 0 if it was not */
    unsigned char ExpectSeq;
    unsigned char *Tx;      /* F2 frame being written */
    unsigned TxLen;
    unsigned char *Rx;      /* F2 bytes waiting to be read */
    unsigned RxLen, RxPos;
    unsigned char *Clm;
    unsigned ClmLen;
    int ClmBad;

    /* counters and checks */
    unsigned Cmd52, Cmd53, Cmd53Block, Cmd53Byte, Cmd53F2Block;
    unsigned BytesF1, MaxXfer;
    unsigned Violations;    /* a CMD shape the driver must never issue */
    char LastViolation[160];
    unsigned FailCmd53At;   /* inject an I/O error at this CMD53 (1-based), 0 = never */
    unsigned FlipRamAt;     /* corrupt this RAM offset on write, ~0 = never */
    unsigned DcmdSeen;
    unsigned EventsQueued;
} fake_chip;

extern const bwl_bus_ops FakeOps;

void FakeInit(fake_chip *F);
void FakeFree(fake_chip *F);
