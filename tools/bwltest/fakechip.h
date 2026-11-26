/*
 * fakechip.h - a simulated BCM43456 or BCM43752 behind SDIO, enough for bcmwlsdio's
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

/* BCM43456: what bwfm and brcmfmac say about the 4345 family. */
#define FAKE_RAMBASE    0x198000u
#define FAKE_RAMSIZE    0xC8000u
#define FAKE_CHIPID     0x15294345u     /* 0x4345 rev 9, AI interconnect */

/*
 * BCM43752: RAM base and chip ID as brcmfmac has them (0x170000; 43752 =
 * 0xaae8, rev 2 as in Armbian's "4362a2" firmware). The RAM size is the
 * model's own (1 MB): brcmfmac reads it from the CR4's bank registers and
 * no figure for the real chip was at hand. Chipcommon rev 35 or later with
 * a separate PMU core, as on brcmfmac's newer chips, so that the driver's
 * PMU-core path runs.
 */
#define FAKE43752_RAMBASE  0x170000u
#define FAKE43752_RAMSIZE  0x100000u
#define FAKE43752_CHIPID   0x1502AAE8u

enum fake_model {
    FAKE_4345 = 0,
    FAKE_43752,
};

typedef struct fake_chip {
    int Model;              /* enum fake_model */
    unsigned ChipIdReg, RamBase, RamSize;

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
    unsigned char DeviceCtl;
    unsigned char MesBusyCtrl;
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
    unsigned char AdvMaxSeq;    /* the window the last queued frame announced */
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

    /* the firmware's station (W4): settings the host made */
    int Up;
    int EventMaskSet;
    unsigned char EventMask[18];
    unsigned Mpc, Pm, Infra, Auth, Wsec, WpaAuth, SupWpa, RoamOff, Country;
    unsigned char WpaIe[64];
    unsigned WpaIeLen;
    char Pmk[65];
    unsigned PmkLen;
    /* the air: FAKE_NAPS access points, one of which may be joined */
    int Assoc;              /* index of the AP joined, -1 if none */
    int Authorized;         /* pairwise key in, or the firmware's handshake done */
    int Eapol3Sent;
    unsigned char PairKey[16], GroupKey[16];
    unsigned GroupIdx, KeysSet, KeyErrors;
    /* counters */
    int HoldEscan;          /* test: keep escan results back until C_SCAN aborts it */
    int EscanPending;
    unsigned EscanSync;
    unsigned Escans, Joins, ScanAborts, DataFromHost, EapolFromHost, DataDropped, ArpReplies;
    unsigned char LastEapol[128];
    unsigned LastEapolLen;
} fake_chip;

/* The simulated air. */
#define FAKE_NAPS       3
#define FAKE_AP_OPEN    0       /* "bwltest-open", channel 6, open */
#define FAKE_AP_PSK     1       /* "bwltest-psk", channel 36 (80 MHz), WPA2-PSK CCMP */
#define FAKE_AP_WEP     2       /* "bwltest-wep", channel 11, privacy without RSN */
#define FAKE_PSK_PASS   "correct horse battery"

typedef struct fake_ap {
    const char *Ssid;
    unsigned char Bssid[6];
    unsigned Channel;       /* control channel */
    unsigned Band;          /* 0 2.4 GHz, 1 5 GHz */
    unsigned Chanspec;      /* as the firmware reports it (d11ac encoding) */
    int Sec;                /* 0 open, 1 WPA2-PSK, 2 WEP */
    int Rssi;
} fake_ap;

extern const fake_ap FakeAps[FAKE_NAPS];
extern const unsigned char FakeGatewayMac[6];
extern const unsigned char FakeGatewayIp[4];
extern const unsigned char FakeStaIp[4];

/* The AP sends a deauthentication. */
void FakeApDeauth(fake_chip *F, unsigned Reason);

extern const bwl_bus_ops FakeOps;

void FakeInit(fake_chip *F);                    /* a BCM43456 */
void FakeInitModel(fake_chip *F, int Model);
void FakeFree(fake_chip *F);
