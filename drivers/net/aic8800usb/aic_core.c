/*
 * aic_core.c - message framing, the command/confirm exchange, and the
 * receive dispatcher for the AIC8800 USB core.
 *
 * Framing follows aicwf_set_cmd_tx() and aicwf_process_rxframes() /
 * aicwf_process_msg_rxframes() of AICSemi's Linux driver; see aic_proto.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic.h"
#include "aic_proto.h"

/* ----------------------------------------------------------- helpers */

void
aic_memcpy(void *Dst, const void *Src, u32 Len)
{
    u8 *d = (u8 *)Dst;
    const u8 *s = (const u8 *)Src;

    while (Len--) {
        *d++ = *s++;
    }
}

void
aic_memset(void *Dst, int Val, u32 Len)
{
    u8 *d = (u8 *)Dst;

    while (Len--) {
        *d++ = (u8)Val;
    }
}

int
aic_memcmp(const void *A, const void *B, u32 Len)
{
    const u8 *a = (const u8 *)A, *b = (const u8 *)B;

    for (; Len; Len--, a++, b++) {
        if (*a != *b) {
            return *a < *b ? -1 : 1;
        }
    }
    return 0;
}

static char *
put_str(char *P, char *End, const char *S)
{
    while (*S && P < End) {
        *P++ = *S++;
    }
    return P;
}

static char *
put_hex(char *P, char *End, u32 V)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    P = put_str(P, End, "0x");
    for (i = 28; i >= 0; i -= 4) {
        if (P < End) {
            *P++ = hex[(V >> i) & 0xF];
        }
    }
    return P;
}

/* Fmt with up to two "%x" (printed as 0x%08x). */
static void
format2(char *Out, u32 Max, const char *Fmt, u32 A, u32 B)
{
    char *p = Out, *end = Out + Max - 1;
    int n = 0;

    while (*Fmt && p < end) {
        if (Fmt[0] == '%' && Fmt[1] == 'x') {
            p = put_hex(p, end, n++ == 0 ? A : B);
            Fmt += 2;
        } else {
            *p++ = *Fmt++;
        }
    }
    *p = 0;
}

void
aic_log(aic_dev *Dev, const char *Fmt, u32 A, u32 B)
{
    char line[128];

    if (Dev->Ops->log == NULL) {
        return;
    }
    format2(line, sizeof(line), Fmt, A, B);
    Dev->Ops->log(Dev->Ctx, line);
}

void
aic_set_error(aic_dev *Dev, const char *Msg, u32 Val)
{
    format2(Dev->LastError, sizeof(Dev->LastError), Msg, Val, 0);
    if (Dev->Ops->log) {
        Dev->Ops->log(Dev->Ctx, Dev->LastError);
    }
}

void
aic_init(aic_dev *Dev)
{
    Dev->CfmWaitId = AIC_NO_CFM;
    Dev->CfmDone = 0;
    Dev->VifIdx = 0xFF;
    Dev->ApIdx = AIC_INVALID_STA;
    Dev->Link = AIC_LINK_DOWN;
    Dev->BssCount = 0;
    Dev->EapolHead = Dev->EapolTail = 0;
    Dev->LastError[0] = 0;
}

/* ------------------------------------------------------- commands */

int
aic_cmd(aic_dev *Dev, u16 Id, u16 DestTask, const void *Param, u16 ParamLen,
        u16 CfmId, void *Cfm, u32 CfmLen)
{
    u8 *b = Dev->TxBuf;
    u32 len = AIC_A2E_HDR + ParamLen;         /* LMAC header + parameters */
    u32 total = AIC_USB_HDR + 4 + len;
    u32 waited = 0, timeout = Dev->CfmTimeoutMs ? Dev->CfmTimeoutMs : 2000;
    int rc;

    if (total > Dev->TxBufSize || total > AIC_CMD_BUF_MAX) {
        return AIC_E_SPACE;
    }
    aic_memset(b, 0, AIC_USB_HDR + 4 + AIC_A2E_HDR);
    aic_put16(b, (len + 4) & 0x0FFF);
    b[2] = AIC_USB_TYPE_CMD_RSP;
    b[3] = 0;
    /* b[4..7]: the dummy word */
    aic_put16(b + 8, Id);
    aic_put16(b + 10, DestTask);
    aic_put16(b + 12, AIC_DRV_TASK_ID);
    aic_put16(b + 14, ParamLen);
    if (ParamLen) {
        aic_memcpy(b + 16, Param, ParamLen);
    }

    Dev->CfmBuf = (u8 *)Cfm;
    Dev->CfmLen = Cfm ? CfmLen : 0;
    Dev->CfmDone = 0;
    Dev->CfmWaitId = CfmId;

    rc = Dev->Ops->send(Dev->Ctx, 1, b, total);
    Dev->Stats.TxMsg++;
    if (rc != AIC_OK) {
        Dev->CfmWaitId = AIC_NO_CFM;
        aic_set_error(Dev, "send of message %x failed", Id);
        return AIC_E_IO;
    }
    if (CfmId == AIC_NO_CFM) {
        return AIC_OK;
    }
    while (!Dev->CfmDone && waited < timeout && !Dev->Abort) {
        Dev->Ops->wait(Dev->Ctx, 20);
        waited += 20;
    }
    Dev->CfmWaitId = AIC_NO_CFM;
    if (!Dev->CfmDone) {
        aic_set_error(Dev, "no confirm for message %x", Id);
        return AIC_E_TIMEOUT;
    }
    return AIC_OK;
}

int
aic_mem_read(aic_dev *Dev, u32 Addr, u32 *Val)
{
    u8 req[AIC_DBG_MEM_READ_REQ_LEN], cfm[AIC_DBG_MEM_READ_CFM_LEN];
    int rc;

    aic_put32(req, Addr);
    aic_memset(cfm, 0, sizeof(cfm));
    rc = aic_cmd(Dev, AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG, req, sizeof(req),
                 AIC_DBG_MEM_READ_CFM, cfm, sizeof(cfm));
    if (rc == AIC_OK) {
        if (aic_get32(cfm) != Addr) {
            aic_set_error(Dev, "memory read answered for another address %x", aic_get32(cfm));
            return AIC_E_FIRMWARE;
        }
        *Val = aic_get32(cfm + 4);
    }
    return rc;
}

int
aic_mem_write(aic_dev *Dev, u32 Addr, u32 Val)
{
    u8 req[AIC_DBG_MEM_WRITE_REQ_LEN];

    aic_put32(req, Addr);
    aic_put32(req + 4, Val);
    return aic_cmd(Dev, AIC_DBG_MEM_WRITE_REQ, AIC_TASK_DBG, req, sizeof(req),
                   AIC_DBG_MEM_WRITE_CFM, NULL, 0);
}

int
aic_mem_block_write(aic_dev *Dev, u32 Addr, const u8 *Data, u32 Len)
{
    /* The vendor driver always sends the full 1032-byte parameter block,
     * with memsize saying how much of it counts. */
    u8 *req = Dev->Scratch;

    if (Len == 0 || Len > AIC_DBG_BLOCK_MAX) {
        return AIC_E_SPACE;
    }
    aic_memset(req, 0, AIC_DBG_MEM_BLOCK_WRITE_REQ_LEN);
    aic_put32(req, Addr);
    aic_put32(req + 4, Len);
    aic_memcpy(req + 8, Data, Len);
    return aic_cmd(Dev, AIC_DBG_MEM_BLOCK_WRITE_REQ, AIC_TASK_DBG, req, AIC_DBG_MEM_BLOCK_WRITE_REQ_LEN,
                   AIC_DBG_MEM_BLOCK_WRITE_CFM, NULL, 0);
}

/* -------------------------------------------------------- receive */

void aic_wlan_ind(aic_dev *Dev, u16 Id, const u8 *Param, u32 Len);   /* aic_wlan.c */
void aic_data_rx(aic_dev *Dev, const u8 *Pkt, u32 Len);              /* aic_data.c */

static void
rx_msg(aic_dev *Dev, const u8 *Msg, u32 Avail)
{
    u16 id, plen;

    if (Avail < AIC_E2A_HDR) {
        Dev->Stats.RxErrors++;
        return;
    }
    id = aic_get16(Msg);
    plen = aic_get16(Msg + 6);
    if (plen > Avail - AIC_E2A_HDR) {
        plen = (u16)(Avail - AIC_E2A_HDR);
    }
    Dev->Stats.RxMsg++;
    if (id == Dev->CfmWaitId && !Dev->CfmDone) {
        u32 n = plen < Dev->CfmLen ? plen : Dev->CfmLen;

        if (n) {
            aic_memcpy(Dev->CfmBuf, Msg + AIC_E2A_HDR, n);
        }
        Dev->CfmLen = n;
        Dev->CfmDone = 1;
        /* SCANU_START_CFM_ADDTIONAL ends the request, but the scan's own
         * SCANU_START_CFM still has to reach aic_wlan_ind. */
        return;
    }
    Dev->Stats.RxInd++;
    aic_wlan_ind(Dev, id, Msg + AIC_E2A_HDR, plen);
}

void
aic_rx(aic_dev *Dev, const u8 *Buf, u32 Len)
{
    u32 off = 0;

    while (off + AIC_USB_HDR <= Len) {
        const u8 *p = Buf + off;
        u32 plen = aic_get16(p);
        u8 type = p[2];
        u32 step;

        if (plen == 0) {
            break;
        }
        if ((type & AIC_USB_TYPE_CFG) != AIC_USB_TYPE_CFG) {
            /* data: plen bytes of 802.11 frame after the 60-byte header */
            u32 total = plen + AIC_RX_HDR_LEN;

            if (off + total > Len) {
                Dev->Stats.RxErrors++;
                break;
            }
            Dev->Stats.RxData++;
            aic_data_rx(Dev, p, total);
            step = (total + 3) & ~3u;
        } else {
            u32 body = (plen + 3) & ~3u;

            if (off + AIC_USB_HDR + plen > Len) {
                Dev->Stats.RxErrors++;
                break;
            }
            switch (type & 0x7F) {
            case AIC_USB_TYPE_CMD_RSP:
                rx_msg(Dev, p + AIC_USB_HDR, plen);
                break;
            case AIC_USB_TYPE_DATA_CFM:
                Dev->Stats.RxDataCfm++;
                break;
            case AIC_USB_TYPE_PRINT:
                Dev->Stats.RxPrint++;
                break;
            default:
                Dev->Stats.RxUnknown++;
                break;
            }
            step = AIC_USB_HDR + body;
        }
        off += step;
    }
}
