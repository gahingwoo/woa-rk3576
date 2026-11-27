/*
 * bwl_proto.c - the wire formats between host and firmware: SDPCM frames
 * on F2, BCDC firmware commands inside control frames, the BCDC header on
 * data and event frames, and Broadcom firmware events.
 *
 * Layouts as in OpenBSD bwfm (if_bwfm_sdio.h: bwfm_sdio_hwhdr/swhdr;
 * bwfmreg.h: bwfm_proto_bcdc_dcmd, bwfm_proto_bcdc_hdr, bwfm_event) and
 * Linux brcmfmac (sdio.c, bcdc.c, fweh.h). Frame building follows
 * bwfm_sdio_tx_ctrlframe, the checks follow bwfm_sdio_rx_frames.
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
BwlSdpcmBuild(u8 *Buf, u32 Cap, u8 Channel, u8 Seq, const u8 *Payload, u32 Len, u16 F2BlockSize)
{
    u32 len = BWL_SDPCM_HDR + Len, padded;

    if (len > 0xFFFFu) {
        return BWL_E_SPACE;
    }
    /* bwfm: a frame longer than a block is padded to whole blocks, else to 4. */
    if (F2BlockSize && len > F2BlockSize && (len % F2BlockSize) != 0) {
        padded = (len + F2BlockSize - 1) / F2BlockSize * F2BlockSize;
    } else {
        padded = (len + 3) & ~3u;
    }
    if (padded > Cap) {
        return BWL_E_SPACE;
    }
    /* hardware header: length and its complement */
    BwlPutLe16(Buf + 0, (u16)len);
    BwlPutLe16(Buf + 2, (u16)~len);
    /* software header */
    Buf[4] = Seq;
    Buf[5] = (u8)(Channel & SDPCM_CH_MASK);
    Buf[6] = 0;                     /* next length */
    Buf[7] = (u8)BWL_SDPCM_HDR;     /* data offset */
    Buf[8] = 0;                     /* flow control */
    Buf[9] = 0;                     /* max sequence */
    Buf[10] = 0;
    Buf[11] = 0;
    if (Len && Payload != Buf + BWL_SDPCM_HDR) {
        BwlMemCpy(Buf + BWL_SDPCM_HDR, Payload, Len);
    }
    BwlMemSet(Buf + len, 0, padded - len);
    return (int)padded;
}

int
BwlSdpcmParseHeader(const u8 *Buf, u32 Len, bwl_frame *F)
{
    u16 frmlen, cksum;

    BwlMemSet(F, 0, (u32)sizeof(*F));
    if (Len < BWL_SDPCM_HDR) {
        return BWL_E_FORMAT;
    }
    frmlen = BwlGetLe16(Buf);
    cksum = BwlGetLe16(Buf + 2);
    F->FrameLen = frmlen;
    if (frmlen == 0 && cksum == 0) {
        return BWL_E_STATE;         /* nothing pending */
    }
    if ((u16)(frmlen ^ cksum) != 0xFFFFu) {
        return BWL_E_FORMAT;
    }
    if (frmlen < BWL_SDPCM_HDR) {
        return BWL_E_FORMAT;
    }
    F->Seq = Buf[4];
    F->Channel = (u8)(Buf[5] & SDPCM_CH_MASK);
    F->NextLen = Buf[6];
    F->DataOff = Buf[7];
    F->FlowCtl = Buf[8];
    F->MaxSeq = Buf[9];
    if (F->DataOff < BWL_SDPCM_HDR || F->DataOff > frmlen) {
        return BWL_E_FORMAT;
    }
    return BWL_OK;
}

int
BwlSdpcmParse(const u8 *Buf, u32 Len, bwl_frame *F)
{
    int r = BwlSdpcmParseHeader(Buf, Len, F);

    if (r) {
        return r;
    }
    if (F->FrameLen > Len) {
        return BWL_E_FORMAT;
    }
    F->Payload = Buf + F->DataOff;
    F->PayloadLen = (u32)F->FrameLen - F->DataOff;
    return BWL_OK;
}

int
BwlTxCredit(u8 Seq, u8 MaxSeq)
{
    u8 d = (u8)(MaxSeq - Seq);

    return d != 0 && (d & 0x80) == 0;
}

int
BwlBcdcBuildDcmd(u8 *Buf, u32 Cap, u32 Cmd, int Set, unsigned IfIdx, u16 ReqId, const u8 *Data, u32 Len)
{
    u32 flags = (Set ? BCDC_DCMD_SET : 0) | BCDC_DCMD_IF(IfIdx) | BCDC_DCMD_ID(ReqId);

    if (Len > BCDC_DCMD_MAXLEN || Cap < BWL_BCDC_DCMD_HDR + Len) {
        return BWL_E_SPACE;
    }
    BwlPutLe32(Buf + 0, Cmd);
    BwlPutLe32(Buf + 4, Len);
    BwlPutLe32(Buf + 8, flags);
    BwlPutLe32(Buf + 12, 0);
    if (Len && Data != Buf + BWL_BCDC_DCMD_HDR) {
        BwlMemCpy(Buf + BWL_BCDC_DCMD_HDR, Data, Len);
    }
    return (int)(BWL_BCDC_DCMD_HDR + Len);
}

int
BwlBcdcParseDcmd(const u8 *Buf, u32 Len, bwl_dcmd_reply *R)
{
    BwlMemSet(R, 0, (u32)sizeof(*R));
    if (Len < BWL_BCDC_DCMD_HDR) {
        return BWL_E_FORMAT;
    }
    R->Cmd = BwlGetLe32(Buf + 0);
    R->Len = BwlGetLe32(Buf + 4);
    R->Flags = BwlGetLe32(Buf + 8);
    R->Status = (int)BwlGetLe32(Buf + 12);
    R->ReqId = (u16)BCDC_DCMD_ID_GET(R->Flags);
    R->Error = (R->Flags & BCDC_DCMD_ERROR) != 0;
    R->Data = Buf + BWL_BCDC_DCMD_HDR;
    R->DataLen = Len - BWL_BCDC_DCMD_HDR;
    /* The reply carries the request's buffer length; never read past the frame. */
    if (R->Len < R->DataLen) {
        R->DataLen = R->Len;
    }
    return BWL_OK;
}

int
BwlBcdcData(const u8 *Buf, u32 Len, const u8 **Payload, u32 *PayloadLen, unsigned *IfIdx)
{
    u32 skip;

    if (Len < BCDC_HDR_LEN) {
        return BWL_E_FORMAT;
    }
    skip = BCDC_HDR_LEN + ((u32)Buf[3] << 2);
    if (skip > Len) {
        return BWL_E_FORMAT;
    }
    if (IfIdx) {
        *IfIdx = Buf[2] & BCDC_FLAG2_IF_MASK;
    }
    *Payload = Buf + skip;
    *PayloadLen = Len - skip;
    return BWL_OK;
}

/*
 * struct bwfm_event: 14-byte Ethernet header, then bcmeth_hdr
 * (subtype 2, length 2, version 1, OUI 3, usr_subtype 2) = 10 bytes, then
 * the event message, big endian: version 2, flags 2, type 4, status 4,
 * reason 4, auth_type 4, datalen 4, addr 6, ifname 16, ifidx 1,
 * bsscfgidx 1 = 48 bytes; then datalen bytes of event data.
 */
#define EV_ETH      14u
#define EV_BCMETH   10u
#define EV_MSG      48u

int
BwlEventParse(const u8 *Eth, u32 Len, bwl_event *E)
{
    const u8 *h, *m;

    BwlMemSet(E, 0, (u32)sizeof(*E));
    if (Len < EV_ETH + EV_BCMETH + EV_MSG) {
        return BWL_E_FORMAT;
    }
    if (BwlGetBe16(Eth + 12) != ETHERTYPE_BRCM) {
        return BWL_E_STATE;         /* not an event: an ordinary data frame */
    }
    h = Eth + EV_ETH;
    if (h[5] != 0x00 || h[6] != 0x10 || h[7] != 0x18 || BwlGetBe16(h + 8) != BRCM_SUBTYPE_EVENT) {
        return BWL_E_FORMAT;
    }
    m = h + EV_BCMETH;
    E->Type = BwlGetBe32(m + 4);
    E->Status = BwlGetBe32(m + 8);
    E->Reason = BwlGetBe32(m + 12);
    E->AuthType = BwlGetBe32(m + 16);
    E->DataLen = BwlGetBe32(m + 20);
    BwlMemCpy(E->Addr, m + 24, 6);
    E->IfIdx = m[46];
    E->BssCfgIdx = m[47];
    if (E->DataLen > Len - (EV_ETH + EV_BCMETH + EV_MSG)) {
        return BWL_E_FORMAT;
    }
    E->Data = m + EV_MSG;
    return BWL_OK;
}
