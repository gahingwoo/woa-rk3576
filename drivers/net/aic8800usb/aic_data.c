/*
 * aic_data.c - data frames: Ethernet to the firmware's USB data frame, and
 * the firmware's received 802.11 frames back to Ethernet.
 *
 * TX follows aicwf_usb_bus_txdata() and rwnx_start_xmit(): a 4-byte USB
 * header (length, type 0x01), the 28-byte host descriptor (payload length,
 * destination, source, ethertype, queue, TID, VIF, station), then the
 * Ethernet payload; the firmware builds the 802.11 header. RX follows
 * rwnx_rxdataind_aicwf(): the firmware hands up the 802.11 frame, already
 * decrypted, after a 60-byte header; the host strips the 802.11 header,
 * the security header and the LLC/SNAP header.
 *
 * Not done yet: RX reordering of A-MPDU sequences (flags_need_reord; TCP
 * copes with the occasional out-of-order frame), defragmentation, TX flow
 * control from the firmware's credit indications.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic.h"
#include "aic_proto.h"

int
aic_build_tx(const aic_dev *Dev, const u8 *Eth, u32 EthLen, u8 *Out, u32 OutMax)
{
    u32 payload, total;

    if (EthLen < 14) {
        return AIC_E_FORMAT;
    }
    payload = EthLen - 14;
    total = AIC_USB_HDR + AIC_HOSTDESC_LEN + payload;
    total = (total + 3) & ~3u;
    if (total + 1 > OutMax || total > 0x0FFF) {
        return AIC_E_SPACE;
    }
    aic_memset(Out, 0, AIC_USB_HDR + AIC_HOSTDESC_LEN);
    {
        u8 *hd = Out + AIC_USB_HDR;

        aic_put16(hd + AIC_HD_PACKET_LEN, payload);
        aic_put32(hd + AIC_HD_STATUS_ADDR, 0);
        aic_memcpy(hd + AIC_HD_DEST, Eth, 6);
        aic_memcpy(hd + AIC_HD_SRC, Eth + 6, 6);
        hd[AIC_HD_ETHERTYPE] = Eth[12];         /* kept in network order */
        hd[AIC_HD_ETHERTYPE + 1] = Eth[13];
        hd[AIC_HD_AC] = AIC_HWQ_BE;
        hd[AIC_HD_TID] = Dev->ApQos ? 0 : AIC_TID_NONE;
        hd[AIC_HD_VIF] = Dev->VifIdx;
        hd[AIC_HD_STA] = Dev->ApIdx;
        aic_put16(hd + AIC_HD_FLAGS, 0);
    }
    aic_memcpy(Out + AIC_USB_HDR + AIC_HOSTDESC_LEN, Eth + 14, payload);
    aic_memset(Out + AIC_USB_HDR + AIC_HOSTDESC_LEN + payload, 0,
               total - (AIC_USB_HDR + AIC_HOSTDESC_LEN + payload));
    /* The vendor driver does not use URB_ZERO_PACKET for data: a transfer
     * that fills whole 512-byte packets gets one more byte instead. */
    if ((total % 512) == 0) {
        Out[total] = 0;
        aic_put16(Out, total);
        Out[2] = AIC_USB_TX_TYPE_DATA;
        Out[3] = 0;
        return (int)(total + 1);
    }
    aic_put16(Out, total);
    Out[2] = AIC_USB_TX_TYPE_DATA;
    Out[3] = 0;
    return (int)total;
}

int
aic_tx_eth(aic_dev *Dev, const u8 *Eth, u32 EthLen)
{
    int n = aic_build_tx(Dev, Eth, EthLen, Dev->TxBuf, Dev->TxBufSize);

    if (n < 0) {
        return n;
    }
    Dev->Stats.TxData++;
    return Dev->Ops->send(Dev->Ctx, 0, Dev->TxBuf, (u32)n) == AIC_OK ? AIC_OK : AIC_E_IO;
}

/* Where the 802.11 header ends and the MSDU (LLC/SNAP) starts; 0 if the
 * frame is not one to hand up. Da and Sa point into the frame. */
static u32
parse_80211(const u8 *Pkt, const u8 *F, u32 FLen, const u8 **Da, const u8 **Sa, int *Amsdu)
{
    u32 flags = aic_get32(Pkt + AIC_RX_FLAGS);
    u32 decr = (aic_get32(Pkt + AIC_RX_STATUS) >> 2) & 7;
    u32 hdr = 24;
    u8 ds;

    *Amsdu = 0;
    if (FLen < 24 || (F[0] & 0x0C) != 0x08 || (flags & AIC_RX_FLAG_4ADDR)) {
        return 0;               /* not data, or 4-address (WDS) */
    }
    if (F[0] & 0x40) {
        return 0;               /* null-data subtypes carry nothing */
    }
    if (F[0] & 0x80) {          /* QoS data */
        if (F[24] & 0x80) {
            *Amsdu = 1;
        }
        hdr += 2;
    }
    if (F[1] & 0x80) {          /* +HTC */
        hdr += 4;
    }
    ds = F[1] & 3;
    if (ds == 2) {              /* from the AP */
        *Da = F + 4;
        *Sa = F + 16;
    } else if (ds == 1) {
        *Da = F + 16;
        *Sa = F + 10;
    } else if (ds == 0) {
        *Da = F + 4;
        *Sa = F + 10;
    } else {
        return 0;
    }
    switch (decr) {
    case AIC_DECR_TKIP:
    case AIC_DECR_CCMP128:
    case AIC_DECR_CCMP256:
    case AIC_DECR_GCMP128:
    case AIC_DECR_GCMP256:
        hdr += 8;
        break;
    case AIC_DECR_WEP:
        hdr += 4;
        break;
    case AIC_DECR_WAPI:
        hdr += 18;
        break;
    default:
        break;
    }
    return hdr <= FLen ? hdr : 0;
}

int
aic_rx_to_eth(const u8 *Pkt, u32 PktLen, u8 *Out, u32 OutMax)
{
    const u8 *f, *da = NULL, *sa = NULL;
    u32 flen, hdr, body;
    int amsdu;

    if (PktLen < AIC_RX_HDR_LEN) {
        return AIC_E_FORMAT;
    }
    flen = aic_get16(Pkt);
    if (flen > PktLen - AIC_RX_HDR_LEN) {
        return AIC_E_FORMAT;
    }
    if (!(aic_get32(Pkt + AIC_RX_FLAGS) & AIC_RX_FLAG_UPLOAD) ||
        (aic_get32(Pkt + AIC_RX_FLAGS) & AIC_RX_FLAG_MONITOR)) {
        return 0;
    }
    f = Pkt + AIC_RX_HDR_LEN;
    hdr = parse_80211(Pkt, f, flen, &da, &sa, &amsdu);
    if (hdr == 0 || amsdu || flen < hdr + 8) {
        return 0;
    }
    body = flen - hdr - 8;      /* after LLC/SNAP (6) and the ethertype (2) */
    if (14 + body > OutMax) {
        return AIC_E_SPACE;
    }
    aic_memcpy(Out, da, 6);
    aic_memcpy(Out + 6, sa, 6);
    Out[12] = f[hdr + 6];
    Out[13] = f[hdr + 7];
    aic_memcpy(Out + 14, f + hdr + 8, body);
    return (int)(14 + body);
}

static void
deliver(aic_dev *Dev, const u8 *Eth, u32 Len)
{
    if (Len >= 14 && Eth[12] == 0x88 && Eth[13] == 0x8E) {
        u8 next = (u8)((Dev->EapolHead + 1) % AIC_EAPOL_QUEUE);

        Dev->Stats.RxEapol++;
        if (next == Dev->EapolTail || Len > AIC_EAPOL_MAX) {
            Dev->Stats.RxDataDropped++;
            return;
        }
        aic_memcpy(Dev->EapolBuf[Dev->EapolHead], Eth, Len);
        Dev->EapolLen[Dev->EapolHead] = (u16)Len;
        Dev->EapolHead = next;
        return;
    }
    if (Dev->Ops->rx_eth && Dev->Link == AIC_LINK_UP) {
        Dev->Ops->rx_eth(Dev->Ctx, Eth, Len);
    } else {
        Dev->Stats.RxDataDropped++;
    }
}

/* A-MSDU: subframes of DA, SA, length (big endian), MSDU (LLC/SNAP,
 * ethertype, data), each padded to 4 bytes except the last. */
static void
deliver_amsdu(aic_dev *Dev, const u8 *P, u32 Len, u8 *Tmp, u32 TmpMax)
{
    u32 off = 0;

    while (off + 14 <= Len) {
        u32 l = aic_be16(P + off + 12);

        if (off + 14 + l > Len || l < 8 || 14 + l - 8 > TmpMax) {
            Dev->Stats.RxErrors++;
            return;
        }
        aic_memcpy(Tmp, P + off, 12);
        aic_memcpy(Tmp + 12, P + off + 14 + 6, l - 6);
        deliver(Dev, Tmp, 12 + (l - 6));
        off += (14 + l + 3) & ~3u;
    }
}

void
aic_data_rx(aic_dev *Dev, const u8 *Pkt, u32 Len)
{
    u8 *eth = Dev->RxEth;
    int n = aic_rx_to_eth(Pkt, Len, eth, sizeof(Dev->RxEth));

    if (n > 0) {
        deliver(Dev, eth, (u32)n);
        return;
    }
    if (n == 0 && Len >= AIC_RX_HDR_LEN) {
        const u8 *f = Pkt + AIC_RX_HDR_LEN, *da, *sa;
        u32 flen = aic_get16(Pkt);
        int amsdu = 0;
        u32 hdr;

        if ((aic_get32(Pkt + AIC_RX_FLAGS) & AIC_RX_FLAG_UPLOAD) && flen <= Len - AIC_RX_HDR_LEN) {
            hdr = parse_80211(Pkt, f, flen, &da, &sa, &amsdu);
            if (hdr && amsdu) {
                deliver_amsdu(Dev, f + hdr, flen - hdr, eth, sizeof(Dev->RxEth));
                return;
            }
        }
    }
    Dev->Stats.RxDataDropped++;
}

/* ------------------------------------------------------- frame FIFO */

int
aic_frameq_push(aic_frameq *Q, const u8 *Frame, u32 Len)
{
    u32 next = (Q->Head + 1) % AIC_FRAMEQ_SLOTS;

    if (next == Q->Tail || Len == 0 || Len > AIC_FRAMEQ_MAX) {
        Q->Drops++;
        return 0;
    }
    aic_memcpy(Q->Buf[Q->Head], Frame, Len);
    Q->Len[Q->Head] = (u16)Len;
    Q->Head = next;
    return 1;
}

const u8 *
aic_frameq_peek(const aic_frameq *Q, u32 *Len)
{
    if (Q->Head == Q->Tail) {
        return NULL;
    }
    *Len = Q->Len[Q->Tail];
    return Q->Buf[Q->Tail];
}

void
aic_frameq_pop(aic_frameq *Q)
{
    if (Q->Head != Q->Tail) {
        Q->Tail = (Q->Tail + 1) % AIC_FRAMEQ_SLOTS;
    }
}
