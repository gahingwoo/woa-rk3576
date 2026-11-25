/*
 * fakechip.h - a simulated AIC8800D80 on USB for tools/aictest: the ROM
 * loader (PID 8D80) with its debug-task memory interface, then the runtime
 * firmware (PID 8D81) with a scan list and one WPA2-PSK access point that
 * runs the authenticator side of the 4-way handshake.
 *
 * It checks what the core sends as strictly as it can (frame headers,
 * message lengths, the download order, the descriptor fields), and counts
 * everything for the test to assert on.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "../../drivers/net/aic8800usb/aic.h"

#define FAKE_RAM_BASE   0x00100000u
#define FAKE_RAM_SIZE   0x00180000u     /* 0x100000 .. 0x280000 */

typedef struct fake_ap {
    const char *Ssid;
    u8  Bssid[6];
    u16 Freq;
    s8  Rssi;
    int Wpa2;                   /* 1: RSN PSK CCMP/CCMP */
    const char *Passphrase;
} fake_ap;

typedef struct fakechip {
    aic_dev *Dev;               /* to hand IN transfers to */
    int Runtime;                /* 0 loader, 1 after START_APP */
    int Reenumerated;
    u32 ChipWord, SubWord;
    u8 *Ram;                    /* FAKE_RAM_SIZE bytes */
    /* sparse word memory outside RAM (registers, patch area) */
    u32 RegAddr[4096];
    u32 RegVal[4096];
    u32 RegCount;

    /* IN queue: packets waiting to be delivered, several per transfer */
    u8  InQ[64][2048];
    u32 InLen[64];
    u32 InHead, InTail;
    int Aggregate;              /* pack several packets into one transfer */

    /* loader checks */
    u32 BlockWrites, MemWrites, MemReads;
    u32 PatchMagicOk;
    u32 BtModeOk;
    u32 StartAppAddr, StartAppType;
    int DropStartAppCfm;

    /* runtime */
    u32 Msgs[0x2000];           /* count per message id seen */
    u8  StaMac[6];
    u8  VifIdx;
    int Started, VifAdded, ResetSeen, MeConfigSeen, ChanConfigSeen, CalibSeen;
    fake_ap Aps[4];
    u32 ApCount;
    int Joined;                 /* index of the AP, -1 none */
    u8  ApIdx;
    u8  Pmk[32], Anonce[32], Snonce[32], Ptk[48], Gtk[16];
    int HsState;                /* 0 idle, 1 sent msg1, 3 sent msg3, 4 done */
    int Msg2Ok, Msg4Ok, Msg2MicBad;
    int PairwiseKeyOk, GroupKeyOk, PortOpen;
    u32 KeyAdds;
    u32 TxData, TxDataBad, TxEapol;
    u8  LastTxEth[2048];
    u32 LastTxEthLen;
    int ZeroPacketSeen;
    char Error[200];
} fakechip;

void fake_init(fakechip *F, aic_dev *Dev);
void fake_free(fakechip *F);
/* bus ops */
int  fake_send(void *Ctx, int Msg, const u8 *Buf, u32 Len);
void fake_wait(void *Ctx, u32 Ms);
void fake_delay(void *Ctx, u32 Ms);
/* inject a data frame from the joined AP (802.11 body after the header) */
void fake_ap_send_data(fakechip *F, const u8 *Da, u16 Ethertype, const u8 *Payload, u32 Len,
                       int Qos, int Ccmp);
void fake_ap_send_amsdu(fakechip *F, const u8 *Da);
void fake_ap_deauth(fakechip *F, u16 Reason);
/* RAM image helpers */
u32  fake_ram_word(fakechip *F, u32 Addr);
u32  fake_reg(fakechip *F, u32 Addr);
