/*
 * bwl_data.c - pure functions for the WLAN layer: chanspecs, escan's
 * wl_bss_info records, frame bodies and their elements, our RSN element,
 * native 802.11 data frames to and from Ethernet, and the frame FIFO.
 *
 * Layouts as in OpenBSD bwfm (bwfmreg.h: bwfm_bss_info) and Linux
 * brcmfmac (fwil_types.h: brcmf_bss_info_le; brcmu_d11.c: the chanspec
 * encodings). The 802.11 conversions are the ones aic8800usb's aic_data.c
 * has, rewritten for this core's types.
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

/* ------------------------------------------------------------ chanspecs */

u16
BwlChanspec(u32 IoType, unsigned Channel, unsigned Band)
{
    u16 ch = (u16)(Channel & CS_CH_MASK);

    if (IoType == IOTYPE_D11N) {
        return (u16)(ch | CS_N_SB_NONE | CS_N_BW_20 | (Band ? CS_N_BND_5G : CS_N_BND_2G));
    }
    return (u16)(ch | CS_AC_BW_20 | (Band ? CS_AC_BND_5G : 0));
}

void
BwlChanspecDecode(u32 IoType, u16 Cs, u8 *Channel, u8 *Band)
{
    int ch = Cs & CS_CH_MASK;

    if (IoType == IOTYPE_D11N) {
        *Band = (u8)((Cs & CS_N_BND_MASK) == CS_N_BND_5G);
        if ((Cs & CS_N_BW_MASK) == CS_N_BW_40) {
            ch += (Cs & CS_N_SB_U) ? 2 : -2;
        }
    } else {
        unsigned sb = (Cs & CS_AC_SB_MASK) >> CS_AC_SB_SHIFT;

        *Band = (u8)((Cs & CS_AC_BND_MASK) == CS_AC_BND_5G);
        switch (Cs & CS_AC_BW_MASK) {
        case CS_AC_BW_40:
            ch += (sb & 1) ? 2 : -2;
            break;
        case CS_AC_BW_80:
            ch += -6 + 4 * (int)(sb & 3);
            break;
        default:
            break;
        }
    }
    *Channel = (u8)(ch > 0 && ch < 256 ? ch : 0);
}

/* ------------------------------------------------------------ elements */

const u8 *
BwlFindIe(const u8 *Ies, u32 Len, u8 Id)
{
    u32 i = 0;

    while (i + 2 <= Len) {
        u32 l = Ies[i + 1];

        if (i + 2 + l > Len) {
            return NULL;
        }
        if (Ies[i] == Id) {
            return Ies + i;
        }
        i += 2 + l;
    }
    return NULL;
}

static int
SuiteIs(const u8 *P, u8 Type)
{
    return P[0] == 0x00 && P[1] == 0x0F && P[2] == 0xAC && P[3] == Type;
}

/*
 * RSN: version, group suite, pairwise count and suites, AKM count and
 * suites. Joinable here: CCMP group and a CCMP pairwise suite with PSK
 * (00-0F-AC:2) among the AKMs. A privacy bit without RSN is WEP or WPA1.
 */
int
BwlRsnSecurity(const u8 *Rsn, u16 Capability)
{
    u32 len, off, n, i;
    int ccmp = 0, psk = 0;

    if (Rsn == NULL) {
        return (Capability & 0x0010) ? BWL_SEC_OTHER : BWL_SEC_OPEN;
    }
    len = Rsn[1];
    Rsn += 2;
    if (len < 2 + 4 + 2 || BwlGetLe16(Rsn) != 1 || !SuiteIs(Rsn + 2, 4)) {
        return BWL_SEC_OTHER;
    }
    off = 6;
    n = BwlGetLe16(Rsn + off);
    off += 2;
    for (i = 0; i < n && off + 4 <= len; i++, off += 4) {
        ccmp |= SuiteIs(Rsn + off, 4);
    }
    if (i < n || off + 2 > len) {
        return BWL_SEC_OTHER;
    }
    n = BwlGetLe16(Rsn + off);
    off += 2;
    for (i = 0; i < n && off + 4 <= len; i++, off += 4) {
        psk |= SuiteIs(Rsn + off, 2);
    }
    if (i < n) {
        return BWL_SEC_OTHER;
    }
    /* RSN capabilities: "MFP required" (bit 6) needs management frame
     * protection, which this driver does not offer */
    if (off + 2 <= len && (BwlGetLe16(Rsn + off) & 0x0040)) {
        return BWL_SEC_OTHER;
    }
    return (ccmp && psk) ? BWL_SEC_WPA2_PSK : BWL_SEC_OTHER;
}

u32
BwlBuildRsnIe(u8 *Out, u32 Max)
{
    static const u8 rsn[] = {
        48, 20,
        0x01, 0x00,                     /* version 1 */
        0x00, 0x0F, 0xAC, 0x04,         /* group: CCMP */
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,   /* pairwise: CCMP */
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,   /* AKM: PSK */
        0x00, 0x00,                     /* capabilities */
    };

    if (Max < sizeof(rsn)) {
        return 0;
    }
    BwlMemCpy(Out, rsn, (u32)sizeof(rsn));
    return (u32)sizeof(rsn);
}

/* ------------------------------------------------------------ BSS records */

static void
FillFromIes(bwl_bss *B, const u8 *Ies, u32 Len)
{
    const u8 *ssid = BwlFindIe(Ies, Len, 0);

    if (ssid != NULL && ssid[1] <= 32) {
        B->SsidLen = ssid[1];
        BwlMemCpy(B->Ssid, ssid + 2, ssid[1]);
    }
    B->Security = (u8)BwlRsnSecurity(BwlFindIe(Ies, Len, 48), B->Capability);
}

int
BwlBssInfoParse(const u8 *Bi, u32 Len, u32 IoType, bwl_bss *Out, u8 *Body, u32 BodyMax, u32 *BodyLen)
{
    u32 rlen, ieoff, ielen, n, nrates;
    u16 cs;
    u8 ch, band;

    BwlMemSet(Out, 0, (u32)sizeof(*Out));
    *BodyLen = 0;
    if (Len < BI_FIXED) {
        return BWL_E_FORMAT;
    }
    rlen = BwlGetLe32(Bi + BI_LENGTH);
    if (rlen < BI_FIXED || rlen > Len) {
        return BWL_E_FORMAT;
    }
    BwlMemCpy(Out->Bssid, Bi + BI_BSSID, 6);
    Out->BeaconPeriod = BwlGetLe16(Bi + BI_BEACON_PERIOD);
    Out->Capability = BwlGetLe16(Bi + BI_CAPABILITY);
    Out->SsidLen = Bi[BI_SSID_LEN] <= 32 ? Bi[BI_SSID_LEN] : 32;
    BwlMemCpy(Out->Ssid, Bi + BI_SSID, Out->SsidLen);
    cs = BwlGetLe16(Bi + BI_CHANSPEC);
    BwlChanspecDecode(IoType, cs, &ch, &band);
    /* brcmfmac: the record's control channel when it has one */
    Out->Channel = Bi[BI_CTL_CH] != 0 ? Bi[BI_CTL_CH] : ch;
    Out->Band = band;
    Out->Rssi = (int)(short)BwlGetLe16(Bi + BI_RSSI);
    Out->Beacon = (u8)((Bi[BI_FLAGS] & BI_FLAGS_FROM_BEACON) != 0);
    ieoff = BwlGetLe16(Bi + BI_IE_OFFSET);
    ielen = BwlGetLe32(Bi + BI_IE_LENGTH);
    if (ielen != 0 && (ieoff < BI_FIXED || ieoff > rlen || ielen > rlen - ieoff)) {
        return BWL_E_FORMAT;
    }
    if (BodyMax < 12) {
        return BWL_E_SPACE;
    }
    BwlMemSet(Body, 0, 8);                          /* Timestamp: not reported */
    BwlPutLe16(Body + 8, Out->BeaconPeriod);
    BwlPutLe16(Body + 10, Out->Capability);
    n = 12;
    if (ielen != 0) {
        if (n + ielen > BodyMax) {
            return BWL_E_SPACE;
        }
        BwlMemCpy(Body + n, Bi + ieoff, ielen);
        n += ielen;
        FillFromIes(Out, Bi + ieoff, ielen);
        /* the record's SSID wins when the element is empty (a hidden network's beacon) */
        if (Out->SsidLen == 0 && Bi[BI_SSID_LEN] != 0 && Bi[BI_SSID_LEN] <= 32) {
            Out->SsidLen = Bi[BI_SSID_LEN];
            BwlMemCpy(Out->Ssid, Bi + BI_SSID, Out->SsidLen);
        }
    } else {
        /* a cached record without elements: SSID and rates from the record */
        nrates = BwlGetLe32(Bi + BI_NRATES);
        if (nrates > 16) {
            nrates = 16;
        }
        if (n + 2 + Out->SsidLen + 2 + nrates > BodyMax) {
            return BWL_E_SPACE;
        }
        Body[n++] = 0;
        Body[n++] = Out->SsidLen;
        BwlMemCpy(Body + n, Out->Ssid, Out->SsidLen);
        n += Out->SsidLen;
        if (nrates != 0) {
            Body[n++] = 1;
            Body[n++] = (u8)(nrates > 8 ? 8 : nrates);
            BwlMemCpy(Body + n, Bi + BI_RATES, nrates > 8 ? 8 : nrates);
            n += nrates > 8 ? 8 : nrates;
        }
        Out->Security = (u8)BwlRsnSecurity(NULL, Out->Capability);
    }
    *BodyLen = n;
    return BWL_OK;
}

int
BwlBssFromBody(const u8 *Bssid, unsigned Channel, unsigned Band, int Rssi, const u8 *Body, u32 Len, bwl_bss *Out)
{
    BwlMemSet(Out, 0, (u32)sizeof(*Out));
    if (Len < 12 || Channel == 0 || Channel > 196) {
        return BWL_E_FORMAT;
    }
    BwlMemCpy(Out->Bssid, Bssid, 6);
    Out->Channel = (u8)Channel;
    Out->Band = (u8)(Band != 0);
    Out->Rssi = Rssi;
    Out->BeaconPeriod = BwlGetLe16(Body + 8);
    Out->Capability = BwlGetLe16(Body + 10);
    FillFromIes(Out, Body + 12, Len - 12);
    return Out->SsidLen != 0 ? BWL_OK : BWL_E_FORMAT;
}

/* ------------------------------------------------------------ 802.11 data frames */

/* RFC 1042 SNAP, and the 802.1H bridge tunnel for the two ethertypes that
 * IEEE 802.1H keeps out of RFC 1042 (AppleTalk ARP, IPX). */
static const u8 k_rfc1042[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
static const u8 k_bridge_tunnel[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8 };

int
BwlEthTo80211(const u8 *Bssid, const u8 *Eth, u32 EthLen, u8 *Out, u32 OutMax)
{
    u32 type, body, n;

    if (EthLen < 14) {
        return BWL_E_FORMAT;
    }
    type = BwlGetBe16(Eth + 12);
    body = EthLen - 14;
    if (type < 0x0600 && type < body) {
        body = type;                /* an 802.3 length: the LLC frame, minus padding */
    }
    n = BWL_80211_HDR + (type >= 0x0600 ? 8u : 0u) + body;
    if (n > OutMax) {
        return BWL_E_SPACE;
    }
    BwlMemSet(Out, 0, BWL_80211_HDR);
    Out[0] = 0x08;                  /* data */
    Out[1] = 0x02;                  /* From DS */
    BwlMemCpy(Out + 4, Eth, 6);     /* A1: the destination (us, or a group) */
    BwlMemCpy(Out + 10, Bssid, 6);  /* A2: the AP */
    BwlMemCpy(Out + 16, Eth + 6, 6);    /* A3: the source */
    n = BWL_80211_HDR;
    if (type >= 0x0600) {
        BwlMemCpy(Out + n, (type == 0x80F3 || type == 0x8137) ? k_bridge_tunnel : k_rfc1042, 6);
        Out[n + 6] = Eth[12];
        Out[n + 7] = Eth[13];
        n += 8;
    }
    BwlMemCpy(Out + n, Eth + 14, body);
    return (int)(n + body);
}

int
Bwl80211ToEth(const u8 *F, u32 Len, u8 *Out, u32 OutMax)
{
    const u8 *da, *sa, *b;
    u32 hdr = BWL_80211_HDR, blen;

    if (Len < BWL_80211_HDR || (F[0] & 0x0C) != 0x08 || (F[0] & 0x40) || (F[1] & 0x40)) {
        return 0;                   /* not data, null data, or protected */
    }
    switch (F[1] & 3) {
    case 1:                         /* To DS: A1 BSSID, A2 SA, A3 DA */
        da = F + 16;
        sa = F + 10;
        break;
    case 0:                         /* no DS bits: A1 DA, A2 SA */
        da = F + 4;
        sa = F + 10;
        break;
    default:
        return 0;
    }
    if (F[0] & 0x80) {              /* QoS data */
        hdr += 2;
        if (F[1] & 0x80) {          /* +HTC */
            hdr += 4;
        }
    }
    if (Len < hdr) {
        return 0;
    }
    b = F + hdr;
    blen = Len - hdr;
    if (blen >= 8 && (BwlMemCmp(b, k_rfc1042, 6) == 0 || BwlMemCmp(b, k_bridge_tunnel, 6) == 0)) {
        if (14 + blen - 8 > OutMax) {
            return BWL_E_SPACE;
        }
        BwlMemCpy(Out, da, 6);
        BwlMemCpy(Out + 6, sa, 6);
        Out[12] = b[6];
        Out[13] = b[7];
        BwlMemCpy(Out + 14, b + 8, blen - 8);
        return (int)(14 + blen - 8);
    }
    /* any other LLC frame goes as 802.3 with a length */
    if (blen == 0) {
        return 0;
    }
    if (blen >= 0x0600 || 14 + blen > OutMax) {
        return BWL_E_SPACE;
    }
    BwlMemCpy(Out, da, 6);
    BwlMemCpy(Out + 6, sa, 6);
    Out[12] = (u8)(blen >> 8);
    Out[13] = (u8)blen;
    BwlMemCpy(Out + 14, b, blen);
    return (int)(14 + blen);
}

/* ------------------------------------------------------------ frame FIFO */

int
BwlFrameqPush(bwl_frameq *Q, const u8 *Frame, u32 Len, unsigned Prio)
{
    u32 next = (Q->Head + 1) % BWL_FRAMEQ_SLOTS;

    if (next == Q->Tail || Len == 0 || Len > BWL_FRAMEQ_MAX) {
        Q->Drops++;
        return 0;
    }
    BwlMemCpy(Q->Buf[Q->Head], Frame, Len);
    Q->Len[Q->Head] = (u16)Len;
    Q->Prio[Q->Head] = (u8)(Prio & 7);
    Q->Head = next;
    return 1;
}

const u8 *
BwlFrameqPeek(const bwl_frameq *Q, u32 *Len, unsigned *Prio)
{
    if (Q->Head == Q->Tail) {
        return NULL;
    }
    *Len = Q->Len[Q->Tail];
    if (Prio) {
        *Prio = Q->Prio[Q->Tail];
    }
    return Q->Buf[Q->Tail];
}

void
BwlFrameqPop(bwl_frameq *Q)
{
    if (Q->Head != Q->Tail) {
        Q->Tail = (Q->Tail + 1) % BWL_FRAMEQ_SLOTS;
    }
}

u32
BwlFrameqCount(const bwl_frameq *Q)
{
    return (Q->Head + BWL_FRAMEQ_SLOTS - Q->Tail) % BWL_FRAMEQ_SLOTS;
}
