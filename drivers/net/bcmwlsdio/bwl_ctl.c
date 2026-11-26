/*
 * bwl_ctl.c - the control channel, polled: send a BCDC firmware command
 * in an SDPCM control frame, read frames until its reply arrives, and
 * the iovar helpers built on it (get/set by name, clmload).
 *
 * Polled: no card interrupt is used. The SDIO core's intstatus is read
 * over the backplane, every millisecond inside a command and every few
 * milliseconds from the Wi-Fi worker (BwlService). Events go to
 * bwl_wlan.c, data frames to the front end (Wlan.Net); BwlTxEth sends
 * data frames inside the firmware's credit window.
 *
 * The receive loop is OpenBSD bwfm's bwfm_sdio_rx_frames and
 * bwfm_sdio_task; the abort after a bad frame is Linux brcmfmac's
 * brcmf_sdio_rxfail.
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

/* An event is at least the Ethernet header, bcmeth_hdr and the message. */
#define EV_MIN (14u + 10u + 48u)

typedef struct want {
    int  Active;
    u16  ReqId;
    int  Got;
    int  Error;
    int  Status;
    u8  *Out;
    u32  OutCap;
    u32  OutLen;
} want;

/* brcmf_sdio_rxfail: abort F2, terminate the read frame, wait for the count to drain. */
static void
RxAbort(bwl_dev *Dev)
{
    u8 hi = 0, lo = 0;
    int i;

    Dev->Stats.RxAborts++;
    (void)BwlWrite8(Dev, CCCR_ABORT, 2);
    (void)BwlWrite8(Dev, F1_FRAMECTRL, FRAMECTRL_RF_TERM);
    for (i = 0; i < 10; i++) {
        if (BwlRead8(Dev, F1_RFRAMEBCHI, &hi) || BwlRead8(Dev, F1_RFRAMEBCLO, &lo)) {
            break;
        }
        if (hi == 0 && lo == 0) {
            break;
        }
        Dev->Ops->delay_us(Dev->Ctx, 10);
    }
}

static void
Dispatch(bwl_dev *Dev, const bwl_frame *F, want *W)
{
    bwl_dcmd_reply rep;
    bwl_event ev;
    const u8 *p;
    u32 plen;
    int r;

    switch (F->Channel) {
    case SDPCM_CH_CONTROL:
        Dev->Stats.CtlRx++;
        if (BwlBcdcParseDcmd(F->Payload, F->PayloadLen, &rep)) {
            Dev->Stats.RxErrors++;
            return;
        }
        if (W == NULL || !W->Active || rep.ReqId != W->ReqId) {
            return;         /* a late reply to a command that timed out */
        }
        W->Got = 1;
        W->Error = rep.Error;
        W->Status = rep.Status;
        W->OutLen = rep.DataLen;
        if (W->Out) {
            BwlMemCpy(W->Out, rep.Data, rep.DataLen < W->OutCap ? rep.DataLen : W->OutCap);
        }
        return;

    case SDPCM_CH_EVENT:
    case SDPCM_CH_DATA:
        if (BwlBcdcData(F->Payload, F->PayloadLen, &p, &plen, NULL)) {
            Dev->Stats.RxErrors++;
            return;
        }
        r = BwlEventParse(p, plen, &ev);
        if (r == 0) {
            Dev->Stats.EventsRx++;
            Dev->Stats.LastEvent = ev.Type;
            Dev->Stats.LastEventStatus = ev.Status;
            BwlWlanEvent(Dev, &ev);
        } else if (r == BWL_E_STATE || plen < EV_MIN) {
            /* an ordinary data frame (also a short one the event parser refused) */
            Dev->Stats.DataRx++;
            if (plen >= 14) {
                Dev->Wlan.RxData++;
                if (BwlGetBe16(p + 12) == ETHERTYPE_EAPOL) {
                    Dev->Wlan.RxEapol++;
                }
                if (Dev->Wlan.Net && Dev->Wlan.Net->rx_eth) {
                    Dev->Wlan.Net->rx_eth(Dev->Ctx, p, plen);
                }
            }
        } else {
            Dev->Stats.RxErrors++;  /* a Broadcom frame that is not a well-formed event */
        }
        return;

    default:
        Dev->Stats.GlomRx++;
        return;
    }
}

/* Read frames until the firmware has none left (header 0/0), at most 64. */
static int
RxFrames(bwl_dev *Dev, want *W)
{
    u8 *b = Dev->Bounce;
    u32 nextlen = 0, k;
    bwl_frame f;
    int r;

    for (k = 0; k < 64; k++) {
        u32 have, flen;

        have = nextlen ? nextlen : BWL_SDPCM_HDR;
        if (have > Dev->BounceSize) {
            RxAbort(Dev);
            return BWL_E_SPACE;
        }
        r = BwlFrameRw(Dev, b, have, 0);
        if (r) {
            RxAbort(Dev);
            return r;
        }
        r = BwlSdpcmParseHeader(b, have, &f);
        if (r == BWL_E_STATE) {
            return BWL_OK;
        }
        if (r || (nextlen && f.FrameLen > nextlen)) {
            Dev->Stats.RxErrors++;
            RxAbort(Dev);
            return BWL_E_FORMAT;
        }
        Dev->TxMaxSeq = f.MaxSeq;
        Dev->Wlan.FlowCtl = f.FlowCtl;
        flen = (u32)f.FrameLen - BWL_SDPCM_HDR;
        if (flen == 0) {
            nextlen = (u32)f.NextLen << 4;
            continue;
        }
        if (!nextlen) {
            u32 rest = (flen + 3) & ~3u;

            if (BWL_SDPCM_HDR + rest > Dev->BounceSize) {
                RxAbort(Dev);
                return BWL_E_SPACE;
            }
            r = BwlFrameRw(Dev, b + BWL_SDPCM_HDR, rest, 0);
            if (r) {
                RxAbort(Dev);
                return r;
            }
        }
        r = BwlSdpcmParse(b, BWL_SDPCM_HDR + flen, &f);
        if (r) {
            Dev->Stats.RxErrors++;
            RxAbort(Dev);
            return r;
        }
        Dev->Stats.FramesRx++;
        if (f.Channel == SDPCM_CH_GLOM) {
            /* Superframes are not handled before W4: drop the rest. */
            Dev->Stats.GlomRx++;
            RxAbort(Dev);
            return BWL_OK;
        }
        Dispatch(Dev, &f, W);
        nextlen = (u32)f.NextLen << 4;
    }
    return BWL_OK;
}

/* One look at the SDIO core: acknowledge, read the mailbox, read frames. */
static int
Poll(bwl_dev *Dev, want *W)
{
    u32 st, mb;
    int r;

    Dev->Stats.Polls++;
    r = BwlSdioCoreRead(Dev, SDCORE_INTSTATUS, &st);
    if (r) {
        return r;
    }
    st &= INTSTATUS_HMB_SW_MASK | INTSTATUS_CHIPACTIVE;
    if (st) {
        Dev->Stats.IntStatus = st;
        r = BwlSdioCoreWrite(Dev, SDCORE_INTSTATUS, st);
        if (r) {
            return r;
        }
    }
    if (st & INTSTATUS_HMB_HOST_INT) {
        r = BwlSdioCoreRead(Dev, SDCORE_TOHOSTMAILBOXDATA, &mb);
        if (r == 0) {
            r = BwlSdioCoreWrite(Dev, SDCORE_TOSBMAILBOX, TOSBMAILBOX_INT_ACK);
        }
        if (r) {
            return r;
        }
        Dev->Stats.MailboxData = mb;
        if (mb & TOHOST_NAKHANDLED) {
            st |= INTSTATUS_HMB_FRAME_IND;
        }
        if ((mb & (TOHOST_DEVREADY | TOHOST_FWREADY)) && Dev->SharedAddr == 0) {
            (void)BwlReadShared(Dev);
        }
    }
    if (st & INTSTATUS_HMB_FRAME_IND) {
        return RxFrames(Dev, W);
    }
    return BWL_OK;
}

int
BwlPollOnce(bwl_dev *Dev)
{
    return Poll(Dev, NULL);
}

int
BwlService(bwl_dev *Dev)
{
    u32 before = Dev->Stats.FramesRx;
    int r;

    if (!Dev->FwRunning) {
        return BWL_E_STATE;
    }
    r = Poll(Dev, NULL);
    if (r) {
        return r;
    }
    r = BwlWlanService(Dev);
    if (r) {
        return r;
    }
    return (int)(Dev->Stats.FramesRx - before);
}

/* brcmfmac prio2prec: best effort and "none" swap with background, so that
 * the flow-control bit for 802.1D priority 0 is bit 2. */
static unsigned
Prec(unsigned Prio)
{
    Prio &= 7;
    return (Prio == 0 || Prio == 3) ? (Prio ^ 2) : Prio;
}

int
BwlTxReady(const bwl_dev *Dev, unsigned Prio)
{
    return Dev->FwRunning && BwlTxCredit(Dev->TxSeq, Dev->TxMaxSeq) &&
           !(Dev->Wlan.FlowCtl & (1u << Prec(Prio)));
}

int
BwlTxEth(bwl_dev *Dev, const u8 *Eth, u32 Len, unsigned Prio)
{
    u8 *b = Dev->Bounce;
    int len, r;

    if (!Dev->FwRunning) {
        return BWL_E_STATE;
    }
    if (Len < 14 || BWL_SDPCM_HDR + BCDC_HDR_LEN + Len + 512 > Dev->BounceSize / 2) {
        Dev->Wlan.TxDataDrops++;
        return BWL_E_FORMAT;
    }
    if (!BwlTxReady(Dev, Prio)) {
        if (Dev->Wlan.FlowCtl & (1u << Prec(Prio))) {
            Dev->Wlan.FlowHold++;
        }
        return BWL_E_CREDIT;
    }
    /* BCDC data header: version, 802.1D priority, interface 0, no data offset */
    b[BWL_SDPCM_HDR + 0] = (u8)BCDC_FLAG_VER(BCDC_PROTO_VER);
    b[BWL_SDPCM_HDR + 1] = (u8)(Prio & 7);
    b[BWL_SDPCM_HDR + 2] = 0;
    b[BWL_SDPCM_HDR + 3] = 0;
    BwlMemCpy(b + BWL_SDPCM_HDR + BCDC_HDR_LEN, Eth, Len);
    len = BwlSdpcmBuild(b, Dev->BounceSize / 2, SDPCM_CH_DATA, Dev->TxSeq, b + BWL_SDPCM_HDR,
                        BCDC_HDR_LEN + Len, Dev->F2BlockSize);
    if (len < 0) {
        Dev->Wlan.TxDataDrops++;
        return len;
    }
    r = BwlFrameRw(Dev, b, (u32)len, 1);
    if (r) {
        Dev->Wlan.TxDataDrops++;
        return r;
    }
    Dev->TxSeq++;
    Dev->Stats.FramesTx++;
    Dev->Wlan.TxData++;
    if (BwlGetBe16(Eth + 12) == ETHERTYPE_EAPOL) {
        Dev->Wlan.TxEapol++;
    }
    return BWL_OK;
}

int
BwlDcmd(bwl_dev *Dev, u32 Cmd, int Set, const u8 *In, u32 InLen, u8 *Out, u32 OutCap,
        u32 *OutLen, int *FwStatus, u32 TimeoutMs)
{
    u8 *b = Dev->Bounce;
    want w;
    int n, len, r;
    u32 t;

    if (OutLen) {
        *OutLen = 0;
    }
    if (FwStatus) {
        *FwStatus = 0;
    }
    if (!Dev->FwRunning) {
        return BWL_E_STATE;
    }
    if (BWL_SDPCM_HDR + BWL_BCDC_DCMD_HDR + InLen + 512 > Dev->BounceSize / 2) {
        return BWL_E_SPACE;
    }

    /* Wait for credit: the firmware's window is in every frame it sends. */
    for (t = 0; !BwlTxCredit(Dev->TxSeq, Dev->TxMaxSeq); t++) {
        if (t >= TimeoutMs) {
            Dev->Detail = ((u32)Dev->TxSeq << 8) | Dev->TxMaxSeq;
            return BWL_E_CREDIT;
        }
        r = Poll(Dev, NULL);
        if (r) {
            return r;
        }
        Dev->Ops->delay_us(Dev->Ctx, 1000);
    }

    BwlMemSet(&w, 0, (u32)sizeof(w));
    w.Active = 1;
    w.ReqId = Dev->ReqId++;
    w.Out = Out;
    w.OutCap = OutCap;

    n = BwlBcdcBuildDcmd(b + BWL_SDPCM_HDR, Dev->BounceSize / 2 - BWL_SDPCM_HDR, Cmd, Set, 0,
                         w.ReqId, In, InLen);
    if (n < 0) {
        return n;
    }
    len = BwlSdpcmBuild(b, Dev->BounceSize / 2, SDPCM_CH_CONTROL, Dev->TxSeq, b + BWL_SDPCM_HDR,
                        (u32)n, Dev->F2BlockSize);
    if (len < 0) {
        return len;
    }
    r = BwlFrameRw(Dev, b, (u32)len, 1);
    if (r) {
        return r;
    }
    Dev->TxSeq++;
    Dev->Stats.FramesTx++;

    for (t = 0; t <= TimeoutMs; t++) {
        r = Poll(Dev, &w);
        if (r) {
            return r;
        }
        if (w.Got) {
            if (OutLen) {
                *OutLen = w.OutLen;
            }
            if (w.Error) {
                if (FwStatus) {
                    *FwStatus = w.Status;
                }
                return BWL_E_FIRMWARE;
            }
            return BWL_OK;
        }
        Dev->Ops->delay_us(Dev->Ctx, 1000);
    }
    Dev->Detail = w.ReqId;
    return BWL_E_TIMEOUT;
}

#define CTL_TIMEOUT_MS 2000

int
BwlGetVar(bwl_dev *Dev, const char *Name, u8 *Out, u32 Cap, u32 *OutLen, int *FwStatus)
{
    u8 *s = BwlScratch(Dev);
    u32 nl = BwlStrLen(Name) + 1;
    u32 len = Cap > nl ? Cap : nl;

    if (len > BCDC_DCMD_MAXLEN) {
        return BWL_E_SPACE;
    }
    BwlMemSet(s, 0, len);
    BwlMemCpy(s, Name, nl);
    return BwlDcmd(Dev, C_GET_VAR, 0, s, len, Out, Cap, OutLen, FwStatus, CTL_TIMEOUT_MS);
}

int
BwlSetVar(bwl_dev *Dev, const char *Name, const u8 *Val, u32 Len, int *FwStatus)
{
    u8 *s = BwlScratch(Dev);
    u32 nl = BwlStrLen(Name) + 1;

    if (nl + Len > BCDC_DCMD_MAXLEN) {
        return BWL_E_SPACE;
    }
    BwlMemCpy(s, Name, nl);
    if (Len && Val != s + nl) {
        BwlMemCpy(s + nl, Val, Len);
    }
    return BwlDcmd(Dev, C_SET_VAR, 1, s, nl + Len, NULL, 0, NULL, FwStatus, CTL_TIMEOUT_MS);
}

int
BwlGetInt(bwl_dev *Dev, u32 Cmd, u32 *Val, int *FwStatus)
{
    u8 in[4] = { 0, 0, 0, 0 }, out[4] = { 0, 0, 0, 0 };
    int r;

    r = BwlDcmd(Dev, Cmd, 0, in, 4, out, 4, NULL, FwStatus, CTL_TIMEOUT_MS);
    *Val = BwlGetLe32(out);
    return r;
}

int
BwlClmLoad(bwl_dev *Dev, const u8 *Blob, u32 Len, u32 *Chunks, u32 *ClmStatus, int *FwStatus)
{
    static const char name[] = "clmload";
    u8 *s = BwlScratch(Dev);
    u8 st[4] = { 0, 0, 0, 0 };
    u32 off = 0, used;
    int n, r;

    *Chunks = 0;
    *ClmStatus = 0xFFFFFFFFu;
    r = BwlClmCheck(Blob, Len);
    if (r) {
        return r;
    }
    while (off < Len) {
        /* Chunk straight behind the name in scratch, then send in place. */
        n = BwlClmChunk(Blob, Len, off, s + sizeof(name), Dev->BounceSize / 2 - (u32)sizeof(name), &used);
        if (n < 0) {
            return n;
        }
        r = BwlSetVar(Dev, name, s + sizeof(name), (u32)n, FwStatus);
        if (r) {
            Dev->Detail = off;
            return r;
        }
        off += used;
        (*Chunks)++;
    }
    r = BwlGetVar(Dev, "clmload_status", st, 4, NULL, FwStatus);
    if (r == 0) {
        *ClmStatus = BwlGetLe32(st);
        if (*ClmStatus != 0) {
            return BWL_E_FIRMWARE;
        }
    }
    return r;
}
