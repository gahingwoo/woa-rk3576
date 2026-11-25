/*
 * fakechip.c - the simulated AIC8800D80 for tools/aictest; see fakechip.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakechip.h"
#include "../../drivers/net/aic8800usb/aic_proto.h"

#define FAIL(...) do { if (!F->Error[0]) snprintf(F->Error, sizeof(F->Error), __VA_ARGS__); } while (0)

static const u8 k_lan_mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };   /* the router behind the AP */

void
fake_init(fakechip *F, aic_dev *Dev)
{
    memset(F, 0, sizeof(*F));
    F->Dev = Dev;
    F->Ram = calloc(1, FAKE_RAM_SIZE);
    F->ChipWord = (AIC_CHIP_REV_U02 << 16) | (1u << 25);   /* U02, not an MCU part */
    F->SubWord = 0x20;
    F->Joined = -1;
    F->Aggregate = 1;
}

void
fake_free(fakechip *F)
{
    free(F->Ram);
    F->Ram = NULL;
}

/* --------------------------------------------------------------- memory */

static int
in_ram(u32 Addr, u32 Len)
{
    return Addr >= FAKE_RAM_BASE && Addr + Len <= FAKE_RAM_BASE + FAKE_RAM_SIZE && Addr + Len >= Addr;
}

u32
fake_ram_word(fakechip *F, u32 Addr)
{
    return in_ram(Addr, 4) ? aic_get32(F->Ram + (Addr - FAKE_RAM_BASE)) : 0;
}

u32
fake_reg(fakechip *F, u32 Addr)
{
    u32 i;

    if (in_ram(Addr, 4)) {
        return fake_ram_word(F, Addr);
    }
    for (i = 0; i < F->RegCount; i++) {
        if (F->RegAddr[i] == Addr) {
            return F->RegVal[i];
        }
    }
    return 0;
}

static void
write_word(fakechip *F, u32 Addr, u32 Val)
{
    u32 i;

    if (in_ram(Addr, 4)) {
        aic_put32(F->Ram + (Addr - FAKE_RAM_BASE), Val);
        return;
    }
    for (i = 0; i < F->RegCount; i++) {
        if (F->RegAddr[i] == Addr) {
            F->RegVal[i] = Val;
            return;
        }
    }
    if (F->RegCount < 4096) {
        F->RegAddr[F->RegCount] = Addr;
        F->RegVal[F->RegCount++] = Val;
    }
}

static u32
read_word(fakechip *F, u32 Addr)
{
    if (Addr == AIC_CHIP_ID_ADDR) {
        return F->ChipWord;
    }
    if (Addr == AIC_CHIP_SUBID_ADDR) {
        return F->SubWord;
    }
    return fake_reg(F, Addr);
}

/* ------------------------------------------------------------ IN queue */

static void
enqueue(fakechip *F, const u8 *Pkt, u32 Len)
{
    u32 padded = (Len + 3) & ~3u;

    if (F->Aggregate && F->InHead != F->InTail) {
        u32 last = (F->InHead + 63) % 64;

        if (F->InLen[last] + padded <= 2048) {
            memcpy(F->InQ[last] + F->InLen[last], Pkt, Len);
            memset(F->InQ[last] + F->InLen[last] + Len, 0, padded - Len);
            F->InLen[last] += padded;
            return;
        }
    }
    if ((F->InHead + 1) % 64 == F->InTail) {
        FAIL("IN queue overflow");
        return;
    }
    memcpy(F->InQ[F->InHead], Pkt, Len);
    memset(F->InQ[F->InHead] + Len, 0, padded - Len);
    F->InLen[F->InHead] = padded;
    F->InHead = (F->InHead + 1) % 64;
}

static void
send_msg(fakechip *F, u16 Id, const void *Param, u32 Len)
{
    u8 p[2048];
    u32 plen = AIC_E2A_HDR + Len;

    memset(p, 0, sizeof(p));
    aic_put16(p, plen);
    p[2] = AIC_USB_TYPE_CMD_RSP;
    aic_put16(p + 4, Id);
    aic_put16(p + 6, AIC_DRV_TASK_ID);
    aic_put16(p + 8, (u16)(Id >> 10));
    aic_put16(p + 10, (u16)Len);
    aic_put32(p + 12, 0x0BADCAFE);
    if (Len) {
        memcpy(p + 4 + AIC_E2A_HDR, Param, Len);
    }
    enqueue(F, p, 4 + plen);
}

/* A console print packet, to check the core skips it. */
static void
send_print(fakechip *F, const char *S)
{
    u8 p[128];
    u32 n = (u32)strlen(S);

    memset(p, 0, sizeof(p));
    aic_put16(p, n);
    p[2] = AIC_USB_TYPE_PRINT;
    memcpy(p + 4, S, n);
    enqueue(F, p, 4 + n);
}

void
fake_wait(void *Ctx, u32 Ms)
{
    fakechip *F = (fakechip *)Ctx;

    (void)Ms;
    while (F->InTail != F->InHead) {
        u32 t = F->InTail;

        F->InTail = (F->InTail + 1) % 64;
        aic_rx(F->Dev, F->InQ[t], F->InLen[t]);
    }
}

void
fake_delay(void *Ctx, u32 Ms)
{
    (void)Ctx;
    (void)Ms;
}

/* ------------------------------------------------------------ beacons */

static u32
build_beacon(const fake_ap *A, u8 *B)
{
    u32 o = 0, l = (u32)strlen(A->Ssid);
    static const u8 rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    static const u8 rsn[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
                              1, 0, 0, 0x0f, 0xac, 2, 0x0c, 0 };

    memset(B, 0, 36);
    B[0] = 0x80;
    memset(B + 4, 0xFF, 6);
    memcpy(B + 10, A->Bssid, 6);
    memcpy(B + 16, A->Bssid, 6);
    aic_put16(B + 32, 100);
    aic_put16(B + 34, A->Wpa2 ? 0x0411 : 0x0401);
    o = 36;
    B[o++] = 0;
    B[o++] = (u8)l;
    memcpy(B + o, A->Ssid, l);
    o += l;
    memcpy(B + o, rates, sizeof(rates));
    o += sizeof(rates);
    B[o++] = 3;
    B[o++] = 1;
    B[o++] = (u8)(A->Freq < 3000 ? (A->Freq - 2407) / 5 : (A->Freq - 5000) / 5);
    if (A->Wpa2) {
        memcpy(B + o, rsn, sizeof(rsn));
        o += sizeof(rsn);
    }
    return o;
}

/* --------------------------------------------------------- data frames */

/* An RX data packet: the 60-byte header, then the 802.11 frame. */
static void
rx_frame(fakechip *F, const u8 *Frame, u32 Len, int Ccmp)
{
    u8 p[2048];

    if (Len + AIC_RX_HDR_LEN > sizeof(p)) {
        FAIL("rx frame too long");
        return;
    }
    memset(p, 0, AIC_RX_HDR_LEN);
    aic_put16(p, Len);
    p[2] = AIC_USB_TYPE_DATA;
    aic_put32(p + AIC_RX_STATUS, (Ccmp ? AIC_DECR_CCMP128 : AIC_DECR_NONE) << 2);
    aic_put32(p + AIC_RX_FLAGS, AIC_RX_FLAG_UPLOAD | AIC_RX_FLAG_80211 | ((u32)F->VifIdx << 8) |
                                ((u32)F->ApIdx << 16) | (0xFFu << 24));
    memcpy(p + AIC_RX_HDR_LEN, Frame, Len);
    enqueue(F, p, AIC_RX_HDR_LEN + Len);
}

static u32
data_header(fakechip *F, u8 *B, const u8 *Da, int Qos, int Ccmp, int Amsdu, const u8 *Sa)
{
    const fake_ap *a = &F->Aps[F->Joined];
    u32 o;

    memset(B, 0, 34);
    B[0] = Qos ? 0x88 : 0x08;
    B[1] = 0x02 | (Ccmp ? 0x40 : 0);       /* FromDS */
    memcpy(B + 4, Da, 6);
    memcpy(B + 10, a->Bssid, 6);
    memcpy(B + 16, Sa ? Sa : k_lan_mac, 6);
    o = 24;
    if (Qos) {
        B[o++] = Amsdu ? 0x80 : 0;
        B[o++] = 0;
    }
    if (Ccmp) {
        memset(B + o, 0x5A, 8);             /* the CCMP header the firmware leaves in */
        o += 8;
    }
    return o;
}

void
fake_ap_send_data(fakechip *F, const u8 *Da, u16 Ethertype, const u8 *Payload, u32 Len,
                  int Qos, int Ccmp)
{
    u8 b[2048];
    /* EAPOL comes from the AP itself, everything else from the LAN */
    u32 o = data_header(F, b, Da, Qos, Ccmp, 0, Ethertype == 0x888E ? F->Aps[F->Joined].Bssid : NULL);
    static const u8 snap[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };

    memcpy(b + o, snap, 6);
    o += 6;
    aic_put_be16(b + o, Ethertype);
    o += 2;
    memcpy(b + o, Payload, Len);
    o += Len;
    rx_frame(F, b, o, Ccmp);
}

void
fake_ap_send_amsdu(fakechip *F, const u8 *Da)
{
    u8 b[2048];
    u32 o = data_header(F, b, Da, 1, 1, 1, NULL), i;
    static const u8 snap[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };

    for (i = 0; i < 2; i++) {
        u32 start = o, l = 6 + 2 + 20 + i;

        memcpy(b + o, Da, 6);
        memcpy(b + o + 6, k_lan_mac, 6);
        aic_put_be16(b + o + 12, l);
        memcpy(b + o + 14, snap, 6);
        aic_put_be16(b + o + 20, 0x0800);
        memset(b + o + 22, 0x40 + (int)i, 20 + i);
        o += 14 + l;
        if (i == 0) {
            while ((o - start) & 3) {
                b[o++] = 0;
            }
        }
    }
    rx_frame(F, b, o, 1);
}

void
fake_ap_deauth(fakechip *F, u16 Reason)
{
    u8 p[6] = { 0 };

    aic_put16(p, Reason);
    p[2] = F->VifIdx;
    send_msg(F, AIC_SM_DISCONNECT_IND, p, sizeof(p));
    F->Joined = -1;
}

/* ------------------------------------------------------ authenticator */

static void
ap_eapol(fakechip *F, u16 Info, u32 Replay, const u8 *Nonce, const u8 *Data, u32 DataLen, int Mic)
{
    u8 e[512];
    u32 len = 99 + DataLen;

    memset(e, 0, len);
    e[0] = 2;
    e[1] = 3;
    aic_put_be16(e + 2, len - 4);
    e[4] = 2;
    aic_put_be16(e + 5, Info);
    aic_put_be16(e + 7, 16);
    e[16] = (u8)Replay;
    if (Nonce) {
        memcpy(e + 17, Nonce, 32);
    }
    aic_put_be16(e + 97, DataLen);
    if (DataLen) {
        memcpy(e + 99, Data, DataLen);
    }
    if (Mic) {
        u8 h[20];

        aic_hmac_sha1(F->Ptk, 16, e, len, h);
        memcpy(e + 81, h, 16);
    }
    fake_ap_send_data(F, F->StaMac, 0x888E, e, len, 0, 0);
}

static void
ap_derive(fakechip *F)
{
    const u8 *aa = F->Aps[F->Joined].Bssid, *spa = F->StaMac;
    u8 d[76];
    int a1 = memcmp(aa, spa, 6) < 0, n1 = memcmp(F->Anonce, F->Snonce, 32) < 0;

    memcpy(d, a1 ? aa : spa, 6);
    memcpy(d + 6, a1 ? spa : aa, 6);
    memcpy(d + 12, n1 ? F->Anonce : F->Snonce, 32);
    memcpy(d + 44, n1 ? F->Snonce : F->Anonce, 32);
    aic_prf_sha1(F->Pmk, 32, "Pairwise key expansion", d, sizeof(d), F->Ptk, 48);
}

static int
mic_ok(fakechip *F, u8 *E, u32 Len)
{
    u8 got[16], h[20];

    memcpy(got, E + 81, 16);
    memset(E + 81, 0, 16);
    aic_hmac_sha1(F->Ptk, 16, E, Len, h);
    memcpy(E + 81, got, 16);
    return memcmp(got, h, 16) == 0;
}

static void
ap_rx_eapol(fakechip *F, u8 *E, u32 Len)
{
    u16 info;
    u32 body;

    F->TxEapol++;
    if (Len < 99 || E[1] != 3 || E[4] != 2) {
        FAIL("STA sent a malformed EAPOL-Key");
        return;
    }
    body = aic_be16(E + 2);
    if (body + 4 != Len) {
        FAIL("EAPOL body length %u, frame %u", body, Len);
        return;
    }
    info = aic_be16(E + 5);
    if (F->HsState == 1) {
        static const u8 want_ie[22] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
                                        1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
        u8 plain[64], wrapped[72];
        u32 n = 0;

        if (info != 0x010A || E[16] != 1) {
            FAIL("message 2: key info %04x replay %u", info, E[16]);
            return;
        }
        memcpy(F->Snonce, E + 17, 32);
        ap_derive(F);
        if (!mic_ok(F, E, Len)) {
            F->Msg2MicBad = 1;          /* a real AP drops it silently */
            return;
        }
        if (aic_be16(E + 97) != 22 || memcmp(E + 99, want_ie, 22) != 0) {
            FAIL("message 2 does not carry the association RSN IE");
            return;
        }
        F->Msg2Ok = 1;
        /* message 3: our RSN IE, the GTK KDE, padding */
        memcpy(plain, want_ie, 22);
        plain[20] = 0x0c;                /* the AP's own capabilities */
        n = 22;
        plain[n++] = 0xDD; plain[n++] = 22;
        plain[n++] = 0x00; plain[n++] = 0x0F; plain[n++] = 0xAC; plain[n++] = 0x01;
        plain[n++] = 0x01; plain[n++] = 0x00;
        memcpy(plain + n, F->Gtk, 16);
        n += 16;
        plain[n++] = 0xDD;              /* 46 -> 48: KDE padding */
        plain[n++] = 0x00;
        aic_aes_wrap(F->Ptk + 16, plain, n, wrapped);
        F->HsState = 3;
        ap_eapol(F, 0x13CA, 2, F->Anonce, wrapped, n + 8, 1);
        return;
    }
    if (F->HsState == 3) {
        if (info != 0x030A || E[16] != 2 || !mic_ok(F, E, Len) || aic_be16(E + 97) != 0) {
            FAIL("message 4: key info %04x replay %u", info, E[16]);
            return;
        }
        F->Msg4Ok = 1;
        F->HsState = 4;
        return;
    }
    FAIL("EAPOL in handshake state %d", F->HsState);
}


/* -------------------------------------------------------------- LAN */

u16
fake_ip_checksum(const u8 *P, u32 Len)
{
    u32 sum = 0, i;

    for (i = 0; i + 1 < Len; i += 2) {
        sum += (u32)(P[i] << 8 | P[i + 1]);
    }
    if (Len & 1) {
        sum += (u32)(P[Len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (u16)~sum;
}

static void
put_be32(u8 *P, u32 V)
{
    P[0] = (u8)(V >> 24); P[1] = (u8)(V >> 16); P[2] = (u8)(V >> 8); P[3] = (u8)V;
}

static u32
get_be32(const u8 *P)
{
    return (u32)P[0] << 24 | (u32)P[1] << 16 | (u32)P[2] << 8 | P[3];
}

/* An IPv4 packet from the gateway: header + Payload (UDP or ICMP body
 * already complete), to Dst (an Ethernet address). */
static void
lan_send_ip(fakechip *F, const u8 *DstMac, u32 DstIp, u8 Proto, const u8 *Body, u32 Len)
{
    u8 ip[1500];

    memset(ip, 0, 20);
    ip[0] = 0x45;
    aic_put_be16(ip + 2, 20 + Len);
    ip[8] = 64;
    ip[9] = Proto;
    put_be32(ip + 12, FAKE_GW_IP);
    put_be32(ip + 16, DstIp);
    aic_put_be16(ip + 10, fake_ip_checksum(ip, 20));
    memcpy(ip + 20, Body, Len);
    fake_ap_send_data(F, DstMac, 0x0800, ip, 20 + Len, 1, 1);
}

static void
lan_dhcp(fakechip *F, const u8 *Udp, u32 Len)
{
    static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const u8 *b = Udp + 8, *o;
    u8 r[8 + 300], *rb = r + 8, type = 0;
    u32 blen = Len - 8, n;

    if (Len < 8 + 240 || b[0] != 1 || get_be32(b + 236) != 0x63825363) {
        FAIL("DHCP from the STA is malformed");
        return;
    }
    for (o = b + 240; o < b + blen && *o != 255; o += (*o == 0) ? 1 : 2 + o[1]) {
        if (*o == 53 && o[1] == 1) {
            type = o[2];
        }
    }
    if (type != 1 && type != 3) {
        FAIL("DHCP message type %u", type);
        return;
    }
    memset(r, 0, sizeof(r));
    rb[0] = 2;                          /* BOOTREPLY */
    rb[1] = 1;
    rb[2] = 6;
    memcpy(rb + 4, b + 4, 4);           /* xid */
    put_be32(rb + 16, FAKE_LEASE_IP);   /* yiaddr */
    put_be32(rb + 20, FAKE_GW_IP);      /* siaddr */
    memcpy(rb + 28, b + 28, 16);        /* chaddr */
    put_be32(rb + 236, 0x63825363);
    n = 240;
    rb[n++] = 53; rb[n++] = 1; rb[n++] = type == 1 ? 2 : 5;
    rb[n++] = 54; rb[n++] = 4; put_be32(rb + n, FAKE_GW_IP); n += 4;
    rb[n++] = 51; rb[n++] = 4; put_be32(rb + n, 86400); n += 4;
    rb[n++] = 1; rb[n++] = 4; put_be32(rb + n, 0xFFFFFF00); n += 4;
    rb[n++] = 3; rb[n++] = 4; put_be32(rb + n, FAKE_GW_IP); n += 4;
    rb[n++] = 6; rb[n++] = 4; put_be32(rb + n, FAKE_GW_IP); n += 4;
    rb[n++] = 255;
    aic_put_be16(r, 67);
    aic_put_be16(r + 2, 68);
    aic_put_be16(r + 4, 8 + n);
    aic_put_be16(r + 6, 0);             /* UDP checksum optional on IPv4 */
    if (type == 1) {
        F->DhcpOffers++;
    } else {
        F->DhcpAcks++;
    }
    lan_send_ip(F, bcast, 0xFFFFFFFF, 17, r, 8 + n);
}

/* A frame the STA sent to the LAN (keys are in place, port open). */
static void
lan_rx(fakechip *F, const u8 *Eth, u32 Len)
{
    u16 type = aic_be16(Eth + 12);
    const u8 *p = Eth + 14;
    u32 plen = Len - 14;

    if (type == 0x0806 && plen >= 28 && aic_be16(p + 6) == 1 && get_be32(p + 24) == FAKE_GW_IP) {
        u8 a[28];

        memcpy(a, p, 8);
        aic_put_be16(a + 6, 2);
        memcpy(a + 8, k_lan_mac, 6);
        put_be32(a + 14, FAKE_GW_IP);
        memcpy(a + 18, p + 8, 10);      /* the asker's MAC and IP */
        F->ArpReplies++;
        fake_ap_send_data(F, Eth + 6, 0x0806, a, 28, 1, 1);
        return;
    }
    if (type != 0x0800 || plen < 20 || (p[0] >> 4) != 4) {
        return;
    }
    if (fake_ip_checksum(p, (u32)(p[0] & 15) * 4) != 0) {
        F->IpChecksumBad++;
        return;
    }
    if (p[9] == 17 && plen >= 28 && aic_be16(p + 22) == 67) {
        lan_dhcp(F, p + 20, aic_be16(p + 2) - 20);
        return;
    }
    if (p[9] == 1 && get_be32(p + 16) == FAKE_GW_IP && p[20] == 8) {
        u8 icmp[1500];
        u32 ilen = aic_be16(p + 2) - 20;

        memcpy(icmp, p + 20, ilen);
        icmp[0] = 0;                    /* echo reply */
        aic_put_be16(icmp + 2, 0);
        aic_put_be16(icmp + 2, fake_ip_checksum(icmp, ilen));
        F->EchoReplies++;
        lan_send_ip(F, Eth + 6, get_be32(p + 12), 1, icmp, ilen);
    }
}

/* ------------------------------------------------------------ TX path */

static void
tx_data(fakechip *F, const u8 *Buf, u32 Len)
{
    u32 l = aic_get16(Buf) & 0x0FFF;
    const u8 *hd = Buf + AIC_USB_HDR;
    u32 plen;
    u8 eth[2048];

    F->TxData++;
    if (!(l == Len || (l + 1 == Len && (l % 512) == 0))) {
        F->TxDataBad++;
        FAIL("data frame: header length %u, transfer %u", l, Len);
        return;
    }
    if (l + 1 == Len) {
        F->ZeroPacketSeen = 1;
    }
    if ((l & 3) || l < AIC_USB_HDR + AIC_HOSTDESC_LEN) {
        F->TxDataBad++;
        FAIL("data frame length %u not padded", l);
        return;
    }
    plen = aic_get16(hd + AIC_HD_PACKET_LEN);
    if (AIC_USB_HDR + AIC_HOSTDESC_LEN + plen > l || hd[AIC_HD_STA] != F->ApIdx ||
        hd[AIC_HD_VIF] != F->VifIdx || hd[AIC_HD_AC] != AIC_HWQ_BE) {
        F->TxDataBad++;
        FAIL("host descriptor: len %u sta %u vif %u ac %u", plen, hd[AIC_HD_STA], hd[AIC_HD_VIF],
             hd[AIC_HD_AC]);
        return;
    }
    memcpy(eth, hd + AIC_HD_DEST, 6);
    memcpy(eth + 6, hd + AIC_HD_SRC, 6);
    eth[12] = hd[AIC_HD_ETHERTYPE];
    eth[13] = hd[AIC_HD_ETHERTYPE + 1];
    memcpy(eth + 14, hd + AIC_HOSTDESC_LEN, plen);
    memcpy(F->LastTxEth, eth, 14 + plen);
    F->LastTxEthLen = 14 + plen;
    if (eth[12] == 0x88 && eth[13] == 0x8E) {
        if (F->Joined < 0 || memcmp(eth, F->Aps[F->Joined].Bssid, 6) != 0 ||
            memcmp(eth + 6, F->StaMac, 6) != 0) {
            FAIL("EAPOL to the wrong address");
            return;
        }
        ap_rx_eapol(F, eth + 14, plen);
        return;
    }
    if (F->Joined >= 0 && F->PortOpen) {
        lan_rx(F, eth, 14 + plen);
    }
}

/* ------------------------------------------------------- message path */

static void
runtime_msg(fakechip *F, u16 Id, const u8 *P, u32 Len)
{
    u8 c[1024];

    memset(c, 0, sizeof(c));
    switch (Id) {
    case AIC_MM_SET_STACK_START_REQ:
        if (Len != 4 || P[0] != 1 || P[2] != 0x20) {
            FAIL("stack start parameters");
        }
        c[0] = 1;
        c[1] = 0x20;
        send_msg(F, AIC_MM_SET_STACK_START_CFM, c, 2);
        break;
    case AIC_MM_GET_FW_VERSION_REQ:
        c[0] = 12;
        memcpy(c + 1, "fake 6.4.3.0", 12);
        send_msg(F, AIC_MM_GET_FW_VERSION_CFM, c, 64);
        break;
    case AIC_MM_SET_RF_CALIB_REQ:
        if (Len != AIC_MM_SET_RF_CALIB_REQ_LEN || aic_get32(P) != 0x0F8F || aic_get32(P + 4) != 0x0F0F ||
            aic_get32(P + 8) != 0x0C34C008 || aic_get32(P + 16) != 0x264203) {
            FAIL("RF calibration parameters");
        }
        F->CalibSeen = 1;
        send_msg(F, AIC_MM_SET_RF_CALIB_CFM, c, 16);
        break;
    case AIC_MM_GET_MAC_ADDR_REQ:
        c[0] = 0x88; c[1] = 0x00; c[2] = 0x33; c[3] = 0x77; c[4] = 0x10; c[5] = 0x01;
        send_msg(F, AIC_MM_GET_MAC_ADDR_CFM, c, 6);
        break;
    case AIC_MM_RESET_REQ:
        if (Len != 0) {
            FAIL("reset has parameters");
        }
        F->ResetSeen = 1;
        send_print(F, "fw: reset\n");
        send_msg(F, AIC_MM_RESET_CFM, c, 0);
        break;
    case AIC_MM_VERSION_REQ:
        aic_put32(c, 0x06040300);
        aic_put32(c + 20, 0x12345678);
        aic_put16(c + 24, 10);
        c[26] = 4;
        send_msg(F, AIC_MM_VERSION_CFM, c, 28);
        break;
    case AIC_ME_CONFIG_REQ:
        if (Len != AIC_ME_CONFIG_REQ_LEN || P[AIC_ME_CONFIG_HT_SUPP] != 1) {
            FAIL("ME config: length %u", Len);
        }
        F->MeConfigSeen = 1;
        send_msg(F, AIC_ME_CONFIG_CFM, c, 0);
        break;
    case AIC_ME_CHAN_CONFIG_REQ:
        if (Len != AIC_ME_CHAN_CONFIG_REQ_LEN || P[AIC_ME_CHAN_CONFIG_CNT24] != 13 ||
            aic_get16(P) != 2412 || aic_get16(P + 12 * 6) != 2472 || P[AIC_ME_CHAN_CONFIG_CNT5] != 25 ||
            aic_get16(P + AIC_ME_CHAN_CONFIG_5G) != 5180 || P[AIC_ME_CHAN_CONFIG_5G + 2] != 1) {
            FAIL("channel config contents");
        }
        F->ChanConfigSeen = 1;
        send_msg(F, AIC_ME_CHAN_CONFIG_CFM, c, 0);
        break;
    case AIC_MM_START_REQ:
        if (Len != AIC_MM_START_REQ_LEN || aic_get32(P + 64) != 300 || aic_get16(P + 68) != 20) {
            FAIL("start parameters");
        }
        F->Started = 1;
        send_msg(F, AIC_MM_START_CFM, c, 1);
        break;
    case AIC_MM_ADD_IF_REQ:
        if (Len != AIC_MM_ADD_IF_REQ_LEN || P[0] != AIC_MM_STA || !F->Started) {
            FAIL("add interface before start or wrong type");
        }
        memcpy(F->StaMac, P + AIC_MM_ADD_IF_ADDR, 6);
        F->VifIdx = 0;
        F->VifAdded = 1;
        c[0] = 0;
        c[1] = F->VifIdx;
        send_msg(F, AIC_MM_ADD_IF_CFM, c, 2);
        break;
    case AIC_SCANU_START_REQ: {
        u32 i;

        if (Len != AIC_SCANU_START_REQ_LEN || P[AIC_SCANU_VIF_IDX] != F->VifIdx ||
            P[AIC_SCANU_CHAN_CNT] != 38 || P[AIC_SCANU_SSID_CNT] != 1 || P[AIC_SCANU_BSSID] != 0xFF) {
            FAIL("scan request: len %u chans %u", Len, P[AIC_SCANU_CHAN_CNT]);
        }
        send_msg(F, AIC_SCANU_START_CFM_ADDTIONAL, c, 3);
        for (i = 0; i < F->ApCount; i++) {
            u8 r[400];
            u32 n;
            int rep;

            for (rep = 0; rep < 2; rep++) {     /* each AP answers twice */
                memset(r, 0, 12);
                n = build_beacon(&F->Aps[i], r + 12);
                aic_put16(r, n);
                aic_put16(r + 2, 0x0080);
                aic_put16(r + 4, F->Aps[i].Freq);
                r[6] = F->Aps[i].Freq < 3000 ? 0 : 1;
                r[7] = 0xFF;
                r[8] = F->VifIdx;
                r[9] = (u8)F->Aps[i].Rssi;
                send_msg(F, AIC_SCANU_RESULT_IND, r, 12 + n);
            }
        }
        c[0] = F->VifIdx;
        c[1] = 0;
        c[2] = (u8)F->ApCount;
        send_msg(F, AIC_SCANU_START_CFM, c, 3);
        break;
    }
    case AIC_SM_CONNECT_REQ: {
        u32 i, flags = aic_get32(P + AIC_SM_CONNECT_FLAGS);
        fake_ap *a = NULL;

        for (i = 0; i < F->ApCount; i++) {
            if (memcmp(F->Aps[i].Bssid, P + AIC_SM_CONNECT_BSSID, 6) == 0) {
                a = &F->Aps[i];
                break;
            }
        }
        if (Len != AIC_SM_CONNECT_REQ_LEN || a == NULL || P[0] != strlen(a->Ssid) ||
            memcmp(P + 1, a->Ssid, P[0]) != 0 || aic_get16(P + AIC_SM_CONNECT_CHAN) != a->Freq ||
            P[AIC_SM_CONNECT_VIF_IDX] != F->VifIdx) {
            FAIL("connect request does not name a known network");
            c[0] = 1;
            send_msg(F, AIC_SM_CONNECT_CFM, c, 1);
            break;
        }
        if (a->Wpa2 && (flags != (AIC_CONTROL_PORT_HOST | AIC_WPA_WPA2_IN_USE) ||
                        aic_get16(P + AIC_SM_CONNECT_IE_LEN) != 22 || P[AIC_SM_CONNECT_IE_BUF] != 48 ||
                        P[AIC_SM_CONNECT_ETHERTYPE] != 0x88 || P[AIC_SM_CONNECT_ETHERTYPE + 1] != 0x8E)) {
            FAIL("WPA2 connect: flags %x ie_len %u", flags, aic_get16(P + AIC_SM_CONNECT_IE_LEN));
        }
        c[0] = 0;
        send_msg(F, AIC_SM_CONNECT_CFM, c, 1);
        F->Joined = (int)i;
        F->ApIdx = 3;
        memset(c, 0, AIC_SM_CONNECT_IND_LEN);
        memcpy(c + AIC_SM_CONNECT_IND_BSSID, a->Bssid, 6);
        c[AIC_SM_CONNECT_IND_AP_IDX] = F->ApIdx;
        c[AIC_SM_CONNECT_IND_QOS] = 1;
        aic_put16(c + AIC_SM_CONNECT_IND_AID, 1);
        aic_put16(c + AIC_SM_CONNECT_IND_FREQ, a->Freq);
        send_msg(F, AIC_SM_CONNECT_IND, c, AIC_SM_CONNECT_IND_LEN);
        if (a->Wpa2) {
            aic_wpa_pmk(a->Passphrase, (u32)strlen(a->Passphrase), (const u8 *)a->Ssid,
                        (u32)strlen(a->Ssid), F->Pmk);
            for (i = 0; i < 32; i++) {
                F->Anonce[i] = (u8)(0xA0 + i);
            }
            for (i = 0; i < 16; i++) {
                F->Gtk[i] = (u8)(0x30 + i);
            }
            F->HsState = 1;
            ap_eapol(F, 0x008A, 1, F->Anonce, NULL, 0, 0);
        }
        break;
    }
    case AIC_MM_KEY_ADD_REQ:
        F->KeyAdds++;
        if (Len != AIC_MM_KEY_ADD_REQ_LEN || P[AIC_KEY_ADD_INST] != F->VifIdx) {
            FAIL("key add: length %u", Len);
        }
        if (P[AIC_KEY_ADD_PAIRWISE]) {
            if (P[AIC_KEY_ADD_STA_IDX] != F->ApIdx || P[AIC_KEY_ADD_KEY_LEN] != 16 ||
                memcmp(P + AIC_KEY_ADD_KEY, F->Ptk + 32, 16) != 0 ||
                P[AIC_KEY_ADD_CIPHER] != AIC_MAC_CIPHER_CCMP || !F->Msg4Ok) {
                FAIL("pairwise key wrong or before message 4");
            } else {
                F->PairwiseKeyOk = 1;
            }
        } else {
            if (P[AIC_KEY_ADD_STA_IDX] != AIC_INVALID_STA || P[AIC_KEY_ADD_KEY_IDX] != 1 ||
                P[AIC_KEY_ADD_KEY_LEN] != 16 || memcmp(P + AIC_KEY_ADD_KEY, F->Gtk, 16) != 0) {
                FAIL("group key wrong");
            } else {
                F->GroupKeyOk = 1;
            }
        }
        c[0] = 0;
        c[1] = (u8)F->KeyAdds;
        send_msg(F, AIC_MM_KEY_ADD_CFM, c, 2);
        break;
    case AIC_ME_SET_CONTROL_PORT_REQ:
        if (Len != 2 || P[0] != F->ApIdx) {
            FAIL("control port for station %u", P[0]);
        }
        F->PortOpen = P[1];
        send_msg(F, AIC_ME_SET_CONTROL_PORT_CFM, c, 0);
        break;
    case AIC_SM_DISCONNECT_REQ:
        send_msg(F, AIC_SM_DISCONNECT_CFM, c, 0);
        fake_ap_deauth(F, aic_get16(P));
        break;
    default:
        FAIL("runtime: unexpected message %04x", Id);
        break;
    }
}

static void
debug_msg(fakechip *F, u16 Id, const u8 *P, u32 Len)
{
    u8 c[16];

    memset(c, 0, sizeof(c));
    switch (Id) {
    case AIC_DBG_MEM_READ_REQ:
        if (Len != 4) {
            FAIL("mem read length %u", Len);
        }
        F->MemReads++;
        aic_put32(c, aic_get32(P));
        aic_put32(c + 4, read_word(F, aic_get32(P)));
        send_msg(F, AIC_DBG_MEM_READ_CFM, c, 8);
        break;
    case AIC_DBG_MEM_WRITE_REQ:
        if (Len != 8) {
            FAIL("mem write length %u", Len);
        }
        F->MemWrites++;
        write_word(F, aic_get32(P), aic_get32(P + 4));
        memcpy(c, P, 8);
        send_msg(F, AIC_DBG_MEM_WRITE_CFM, c, 8);
        break;
    case AIC_DBG_MEM_BLOCK_WRITE_REQ: {
        u32 a = aic_get32(P), n = aic_get32(P + 4);

        F->BlockWrites++;
        if (Len != AIC_DBG_MEM_BLOCK_WRITE_REQ_LEN || n == 0 || n > 1024 || !in_ram(a, n) || F->Runtime) {
            FAIL("block write %08x+%u (param %u)", a, n, Len);
        } else {
            memcpy(F->Ram + (a - FAKE_RAM_BASE), P + 8, n);
        }
        send_msg(F, AIC_DBG_MEM_BLOCK_WRITE_CFM, c, 4);
        break;
    }
    case AIC_DBG_START_APP_REQ:
        F->StartAppAddr = aic_get32(P);
        F->StartAppType = aic_get32(P + 4);
        if (F->StartAppType == AIC_HOST_START_APP_AUTO) {
            u32 sb = fake_ram_word(F, AIC_D80_PATCH_STR_PTR);

            F->PatchMagicOk = fake_reg(F, sb) == AIC_PATCH_MAGIC && fake_reg(F, sb + 8) == AIC_PATCH_MAGIC_2 &&
                              fake_reg(F, sb + 12) == 3;
            if (!F->DropStartAppCfm) {
                send_msg(F, AIC_DBG_START_APP_CFM, c, 4);
            }
            F->Runtime = 1;
            F->Reenumerated = 1;
        } else {
            send_msg(F, AIC_DBG_START_APP_CFM, c, 4);
            F->Runtime = 0;
        }
        break;
    default:
        FAIL("debug: unexpected message %04x", Id);
        break;
    }
}

int
fake_send(void *Ctx, int Msg, const u8 *Buf, u32 Len)
{
    fakechip *F = (fakechip *)Ctx;
    u32 l, plen;
    u16 id;

    if (Len < 4) {
        FAIL("transfer of %u bytes", Len);
        return AIC_E_IO;
    }
    if (Buf[2] == AIC_USB_TX_TYPE_DATA) {
        if (Msg || !F->Runtime) {
            FAIL("data frame on the message path or before the firmware runs");
        }
        tx_data(F, Buf, Len);
        return AIC_OK;
    }
    l = aic_get16(Buf);
    if (Buf[2] != AIC_USB_TYPE_CMD_RSP || !Msg || l + 4 != Len || Len < 16 || aic_get32(Buf + 4) != 0) {
        FAIL("message frame: type %02x len field %u transfer %u", Buf[2], l, Len);
        return AIC_OK;
    }
    id = aic_get16(Buf + 8);
    plen = aic_get16(Buf + 14);
    if (aic_get16(Buf + 12) != AIC_DRV_TASK_ID || plen + 16 != Len || aic_get16(Buf + 10) != (id >> 10)) {
        FAIL("message %04x header: src %u dest %u plen %u", id, aic_get16(Buf + 12), aic_get16(Buf + 10), plen);
        return AIC_OK;
    }
    if (id < 0x2000) {
        F->Msgs[id]++;
    }
    if ((id >> 10) == AIC_TASK_DBG) {
        debug_msg(F, id, Buf + 16, plen);
    } else if (!F->Runtime) {
        FAIL("message %04x to the ROM loader", id);
    } else {
        runtime_msg(F, id, Buf + 16, plen);
    }
    return AIC_OK;
}
