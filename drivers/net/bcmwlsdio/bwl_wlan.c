/*
 * bwl_wlan.c - the station on top of the control channel: firmware set-up
 * after the start, escan and its results, joining an open or a WPA2-PSK
 * network, keys, leaving, link rate and RSSI, and the firmware events that
 * drive all of it.
 *
 * WPA2-PSK runs in one of two ways, chosen per join:
 *   - host supplicant (Windows' own, through WiFiCx): sup_wpa 0, the RSN
 *     element set with "wpaie", EAPOL frames pass as ordinary data frames
 *     and the host hands the keys down (BwlSetKey, "wsec_key"); this is
 *     what Linux brcmfmac does under wpa_supplicant without offload;
 *   - firmware supplicant: sup_wpa 1 and the passphrase with
 *     C_SET_WSEC_PMK, the firmware runs the 4-way and group handshakes and
 *     reports E_PSK_SUP; brcmfmac's "FWSUP" path.
 *
 * The sequences follow OpenBSD bwfm (bwfm_init, bwfm_scan, bwfm_connect,
 * bwfm_set_key_cb, bwfm_rx_event) and Linux brcmfmac (cfg80211.c:
 * brcmf_run_escan, brcmf_cfg80211_connect, brcmf_is_linkup and
 * brcmf_is_linkdown, brcmf_get_assoc_ies, brcmf_cfg80211_add_key).
 *
 * Event handlers only record: a firmware command from inside the receive
 * path would reenter it. What has to follow an event runs in
 * BwlWlanService, which BwlService calls after each poll.
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

#define WLAN_TIMEOUT_MS 2000

/* bwl_wlan.JoinFlags: the join's progress between events */
#define JOIN_OK     0x01u   /* E_SET_SSID (or E_LINK up) succeeded */
#define JOIN_KEYED  0x02u   /* E_PSK_SUP completed (firmware supplicant) */

static int
SetInt(bwl_dev *Dev, u32 Cmd, u32 Val)
{
    u8 b[4];
    int fws = 0, r;

    BwlPutLe32(b, Val);
    r = BwlDcmd(Dev, Cmd, 1, b, 4, NULL, 0, NULL, &fws, WLAN_TIMEOUT_MS);
    if (r == BWL_E_FIRMWARE) {
        Dev->Wlan.LastFwStatus = (u32)fws;
    }
    return r;
}

static int
SetVarInt(bwl_dev *Dev, const char *Name, u32 Val)
{
    u8 b[4];
    int fws = 0, r;

    BwlPutLe32(b, Val);
    r = BwlSetVar(Dev, Name, b, 4, &fws);
    if (r == BWL_E_FIRMWARE) {
        Dev->Wlan.LastFwStatus = (u32)fws;
    }
    return r;
}

static int
SetVarData(bwl_dev *Dev, const char *Name, const u8 *Val, u32 Len)
{
    int fws = 0, r;

    r = BwlSetVar(Dev, Name, Val, Len, &fws);
    if (r == BWL_E_FIRMWARE) {
        Dev->Wlan.LastFwStatus = (u32)fws;
    }
    return r;
}

/* Is Word a space-separated token of S (Len bytes, NUL or not)? */
static int
HasToken(const char *S, u32 Len, const char *Word)
{
    u32 wl = BwlStrLen(Word), i = 0;

    while (i < Len && S[i]) {
        u32 j = i;

        while (j < Len && S[j] && S[j] != ' ' && S[j] != '\n') {
            j++;
        }
        if (j - i == wl && BwlMemCmp(S + i, Word, wl) == 0) {
            return 1;
        }
        i = j + 1;
    }
    return 0;
}

static void
SetBit(u8 *Mask, unsigned Event)
{
    if (Event / 8 < EVENT_MASK_LEN) {
        Mask[Event / 8] = (u8)(Mask[Event / 8] | (1u << (Event % 8)));
    }
}

int
BwlWlanInit(bwl_dev *Dev, const char *Country)
{
    static const unsigned events[] = {
        E_SET_SSID, E_AUTH, E_DEAUTH, E_DEAUTH_IND, E_ASSOC, E_REASSOC, E_DISASSOC, E_DISASSOC_IND,
        E_LINK, E_ROAM, E_PSK_SUP, E_IF, E_ESCAN_RESULT,
    };
    bwl_wlan *w = &Dev->Wlan;
    u8 mask[EVENT_MASK_LEN], b[16];
    char cap[256];
    u32 v, n, i;
    int fws = 0, r;

    w->Up = 0;
    w->Link = BWL_LINK_DOWN;
    w->Events = 0;
    if (!Dev->FwRunning) {
        return BWL_E_STATE;
    }

    Dev->Step = "wlan: version";
    r = BwlGetInt(Dev, C_GET_VERSION, &v, &fws);
    if (r) {
        return r;
    }
    w->IoType = v;

    Dev->Step = "wlan: address";
    r = BwlGetVar(Dev, "cur_etheraddr", w->Mac, 6, NULL, &fws);
    if (r) {
        return r;
    }

    /* the firmware's feature list; "idsup" is its own supplicant */
    BwlMemSet(cap, 0, (u32)sizeof(cap));
    if (BwlGetVar(Dev, "cap", (u8 *)cap, (u32)sizeof(cap) - 1, NULL, &fws) == 0) {
        w->FwSupCapable = (u8)HasToken(cap, (u32)sizeof(cap) - 1, "idsup");
    }

    Dev->Step = "wlan: event mask";
    BwlMemSet(mask, 0, EVENT_MASK_LEN);
    (void)BwlGetVar(Dev, "event_msgs", mask, EVENT_MASK_LEN, NULL, &fws);
    for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        SetBit(mask, events[i]);
    }
    r = SetVarData(Dev, "event_msgs", mask, EVENT_MASK_LEN);
    if (r) {
        return r;
    }

    /* Not fatal when a firmware does not know them: radio and power save
     * always on (the host polls, and Windows is told there is no power
     * save), no firmware roaming (Windows picks the network), no RX
     * superframes (the receive path does not take them apart). */
    (void)SetVarInt(Dev, "mpc", 0);
    (void)SetInt(Dev, C_SET_PM, 0);
    (void)SetVarInt(Dev, "roam_off", 1);
    (void)SetVarInt(Dev, "bus:rxglom", 0);

    Dev->Step = "wlan: infrastructure";
    r = SetInt(Dev, C_SET_INFRA, 1);
    if (r == 0) {
        r = SetInt(Dev, C_SET_AUTH, 0);
    }
    if (r) {
        return r;
    }

    if (Country != NULL && Country[0] && Country[1]) {
        /* struct brcmf_fil_country_le: abbreviation, revision -1, code */
        u8 c[12];

        Dev->Step = "wlan: country";
        BwlMemSet(c, 0, (u32)sizeof(c));
        c[0] = c[8] = (u8)Country[0];
        c[1] = c[9] = (u8)Country[1];
        BwlPutLe32(c + 4, 0xFFFFFFFFu);
        (void)SetVarData(Dev, "country", c, (u32)sizeof(c));
    }

    Dev->Step = "wlan: up";
    r = SetInt(Dev, C_UP, 1);
    if (r) {
        return r;
    }

    /* bands: count, then WLC_BAND_5G (1) and WLC_BAND_2G (2) */
    BwlMemSet(b, 0, (u32)sizeof(b));
    if (BwlDcmd(Dev, C_GET_BANDLIST, 0, b, (u32)sizeof(b), b, (u32)sizeof(b), NULL, &fws, WLAN_TIMEOUT_MS) == 0) {
        n = BwlGetLe32(b);
        for (i = 0; i < n && i < 3; i++) {
            if (BwlGetLe32(b + 4 + 4 * i) == 1) {
                w->Has5g = 1;
            }
        }
    }
    w->Up = 1;
    Dev->Step = "wlan: up";
    return BWL_OK;
}

/* ------------------------------------------------------------ scan */

int
BwlScanStart(bwl_dev *Dev)
{
    bwl_wlan *w = &Dev->Wlan;
    u8 p[ESCAN_PARAMS_FIXED];
    int r;

    if (!w->Up) {
        return BWL_E_STATE;
    }
    BwlMemSet(p, 0, (u32)sizeof(p));
    w->ScanSyncId++;
    BwlPutLe32(p + 0, ESCAN_REQ_VERSION);
    BwlPutLe16(p + 4, ESCAN_ACTION_START);
    BwlPutLe16(p + 6, w->ScanSyncId);
    /* scan params at 8: SSID length 0 and SSID (the wildcard), BSSID broadcast */
    BwlMemSet(p + 8 + 36, 0xFF, 6);
    p[8 + 42] = BSSTYPE_ANY;
    p[8 + 43] = 0;                          /* active */
    BwlPutLe32(p + 8 + 44, 0xFFFFFFFFu);    /* probes, active, passive, home time: the firmware's */
    BwlPutLe32(p + 8 + 48, 0xFFFFFFFFu);
    BwlPutLe32(p + 8 + 52, 0xFFFFFFFFu);
    BwlPutLe32(p + 8 + 56, 0xFFFFFFFFu);
    BwlPutLe32(p + 8 + 60, 0);              /* no channel list: all of them */
    w->Scanning = 1;
    w->ScanResults = 0;
    w->ScanStatus = 0;
    w->Events &= ~BWL_EV_SCAN_DONE;
    r = SetVarData(Dev, "escan", p, (u32)sizeof(p));
    if (r) {
        w->Scanning = 0;
    }
    return r;
}

/* brcmf_abort_scanning: C_SCAN with the channel list { -1 }. */
static int
ScanAbort(bwl_dev *Dev)
{
    u8 p[SCAN_PARAMS_FIXED + 4];

    BwlMemSet(p, 0, (u32)sizeof(p));
    BwlMemSet(p + 36, 0xFF, 6);
    p[42] = BSSTYPE_ANY;
    BwlPutLe32(p + 44, 0xFFFFFFFFu);
    BwlPutLe32(p + 48, 0xFFFFFFFFu);
    BwlPutLe32(p + 52, 0xFFFFFFFFu);
    BwlPutLe32(p + 56, 0xFFFFFFFFu);
    BwlPutLe32(p + 60, 1);
    BwlPutLe16(p + 64, 0xFFFF);
    return BwlDcmd(Dev, C_SCAN, 1, p, (u32)sizeof(p), NULL, 0, NULL, NULL, WLAN_TIMEOUT_MS);
}

static void
EscanEvent(bwl_dev *Dev, const bwl_event *E)
{
    bwl_wlan *w = &Dev->Wlan;
    const u8 *d = E->Data;
    u32 len = E->DataLen, off, n, i;

    if (E->Status != E_STATUS_PARTIAL) {
        if (w->Scanning) {
            w->Scanning = 0;
            w->ScanStatus = E->Status;
            w->Events |= BWL_EV_SCAN_DONE;
        }
        return;
    }
    if (!w->Scanning || len < ESCAN_RESULT_FIXED) {
        return;
    }
    n = BwlGetLe16(d + 10);
    off = ESCAN_RESULT_FIXED;
    for (i = 0; i < n && off + BI_FIXED <= len; i++) {
        u32 rlen = BwlGetLe32(d + off + BI_LENGTH), blen = 0;
        bwl_bss b;

        if (rlen < BI_FIXED || rlen > len - off) {
            break;
        }
        if (BwlBssInfoParse(d + off, rlen, w->IoType, &b, w->ScanBody, (u32)sizeof(w->ScanBody), &blen) == 0) {
            w->ScanResults++;
            if (w->Net && w->Net->scan_bss) {
                w->Net->scan_bss(Dev->Ctx, &b, w->ScanBody, blen);
            }
        }
        off += (rlen + 3) & ~3u;
    }
}

/* ------------------------------------------------------------ join */

int
BwlConnect(bwl_dev *Dev, const bwl_bss *Bss, const char *Passphrase)
{
    bwl_wlan *w = &Dev->Wlan;
    int psk = Bss->Security == BWL_SEC_WPA2_PSK;
    u8 j[JOIN_PARAMS_LEN];
    int r;

    if (!w->Up) {
        return BWL_E_STATE;
    }
    if (Bss->Security != BWL_SEC_OPEN && !psk) {
        return BWL_E_FORMAT;
    }
    if (Bss->SsidLen == 0 || Bss->SsidLen > 32 || Bss->Channel == 0) {
        return BWL_E_FORMAT;
    }
    if (w->Scanning) {
        (void)ScanAbort(Dev);
        w->Scanning = 0;
        w->ScanStatus = 4;      /* aborted */
        w->Events |= BWL_EV_SCAN_DONE;
    }
    w->FwSup = (u8)(psk && Passphrase != NULL);
    w->Target = *Bss;
    w->AssocReqIeLen = w->AssocRspIeLen = 0;
    w->AssocPending = 0;
    w->ConnStatus = 0;
    w->ConnEvent = 0;
    w->AssocAid = 0;
    w->AssocCapability = 0;
    w->AssocListen = 0;
    w->Events &= ~(BWL_EV_ASSOC | BWL_EV_KEYED | BWL_EV_DISCONNECT);
    w->JoinFlags = 0;

    Dev->Step = "join: security";
    r = SetVarInt(Dev, "wpa_auth", psk ? WPA2_AUTH_PSK : WPA_AUTH_DISABLED);
    if (r == 0) {
        r = SetVarInt(Dev, "wsec", psk ? WSEC_AES : WSEC_NONE);
    }
    if (r) {
        return r;
    }
    r = SetVarInt(Dev, "sup_wpa", w->FwSup ? 1 : 0);
    if (r && w->FwSup) {
        return r;               /* no firmware supplicant in this build */
    }
    w->RsnIeLen = psk ? BwlBuildRsnIe(w->RsnIe, (u32)sizeof(w->RsnIe)) : 0;
    (void)SetVarData(Dev, "wpaie", w->RsnIe, w->RsnIeLen);
    if (w->FwSup) {
        u8 pmk[WSEC_PMK_LEN];
        u32 pl = BwlStrLen(Passphrase);

        if (pl < 8 || pl > 64) {
            return BWL_E_FORMAT;
        }
        Dev->Step = "join: passphrase";
        BwlMemSet(pmk, 0, (u32)sizeof(pmk));
        BwlPutLe16(pmk + 0, (u16)pl);
        BwlPutLe16(pmk + 2, WSEC_PASSPHRASE);
        BwlMemCpy(pmk + 4, Passphrase, pl);
        r = BwlDcmd(Dev, C_SET_WSEC_PMK, 1, pmk, (u32)sizeof(pmk), NULL, 0, NULL, NULL, WLAN_TIMEOUT_MS);
        BwlMemSet(pmk, 0, (u32)sizeof(pmk));
        if (r) {
            return r;
        }
    }
    r = SetInt(Dev, C_SET_AUTH, 0);
    if (r) {
        return r;
    }

    /* struct brcmf_join_params: SSID, then BSSID, one chanspec */
    Dev->Step = "join: set ssid";
    BwlMemSet(j, 0, (u32)sizeof(j));
    BwlPutLe32(j, Bss->SsidLen);
    BwlMemCpy(j + 4, Bss->Ssid, Bss->SsidLen);
    BwlMemCpy(j + 36, Bss->Bssid, 6);
    BwlPutLe32(j + 44, 1);
    BwlPutLe16(j + 48, BwlChanspec(w->IoType, Bss->Channel, Bss->Band));
    w->Link = BWL_LINK_CONNECTING;
    BwlMemCpy(w->Bssid, Bss->Bssid, 6);
    r = BwlDcmd(Dev, C_SET_SSID, 1, j, (u32)sizeof(j), NULL, 0, NULL, NULL, WLAN_TIMEOUT_MS);
    if (r) {
        w->Link = BWL_LINK_DOWN;
    }
    return r;
}

int
BwlSetKey(bwl_dev *Dev, int Pairwise, unsigned KeyIdx, const u8 *Key, u32 Len)
{
    bwl_wlan *w = &Dev->Wlan;
    u8 k[WSEC_KEY_LEN];
    int r;

    if (w->Link < BWL_LINK_ASSOCIATED || w->FwSup) {
        return BWL_E_STATE;
    }
    if (Len != 16 || KeyIdx > 3) {
        return BWL_E_FORMAT;
    }
    /* bwfm_set_key_cb / brcmf_cfg80211_add_key: the pairwise key carries
     * the peer's address, the group key the "primary" flag */
    BwlMemSet(k, 0, (u32)sizeof(k));
    BwlPutLe32(k + 0, Pairwise ? 0 : KeyIdx);
    BwlPutLe32(k + 4, Len);
    BwlMemCpy(k + WSEC_KEY_DATA, Key, Len);
    BwlPutLe32(k + WSEC_KEY_ALGO, CRYPTO_ALGO_AES_CCM);
    if (Pairwise) {
        BwlMemCpy(k + WSEC_KEY_EA, w->Bssid, 6);
    } else {
        BwlPutLe32(k + WSEC_KEY_FLAGS, WSEC_PRIMARY_KEY);
    }
    r = SetVarData(Dev, "wsec_key", k, (u32)sizeof(k));
    BwlMemSet(k, 0, (u32)sizeof(k));
    if (r == 0) {
        r = SetVarInt(Dev, "wsec", WSEC_AES);
    }
    if (r == 0) {
        w->Keys++;
        if (Pairwise) {
            w->Link = BWL_LINK_UP;
        }
    }
    return r;
}

int
BwlDisconnect(bwl_dev *Dev, u16 Reason)
{
    bwl_wlan *w = &Dev->Wlan;
    u8 s[12];
    int r = BWL_OK;

    if (w->Link != BWL_LINK_DOWN) {
        /* down first: the firmware's own E_LINK and E_DISASSOC for this
         * are not a drop to report */
        w->Link = BWL_LINK_DOWN;
        BwlMemSet(s, 0, (u32)sizeof(s));
        BwlPutLe32(s, Reason);
        BwlMemCpy(s + 4, w->Bssid, 6);
        r = BwlDcmd(Dev, C_DISASSOC, 1, s, (u32)sizeof(s), NULL, 0, NULL, NULL, WLAN_TIMEOUT_MS);
    }
    w->Link = BWL_LINK_DOWN;
    w->AssocPending = 0;
    w->JoinFlags = 0;
    w->Events &= ~(BWL_EV_ASSOC | BWL_EV_KEYED);
    return r;
}

int
BwlLinkInfo(bwl_dev *Dev)
{
    bwl_wlan *w = &Dev->Wlan;
    u8 s[12];
    u32 v;
    int r;

    if (w->Link < BWL_LINK_ASSOCIATED) {
        return BWL_E_STATE;
    }
    r = BwlGetInt(Dev, C_GET_RATE, &v, NULL);
    if (r == 0) {
        w->RateKbps = v * 500u;
    }
    BwlMemSet(s, 0, (u32)sizeof(s));
    r = BwlDcmd(Dev, C_GET_RSSI, 0, s, (u32)sizeof(s), s, (u32)sizeof(s), NULL, NULL, WLAN_TIMEOUT_MS);
    if (r == 0) {
        w->Rssi = (int)BwlGetLe32(s);
    }
    return r;
}

/* ------------------------------------------------------------ events */

static void
JoinFailed(bwl_wlan *W, const bwl_event *E, u32 Status)
{
    W->Link = BWL_LINK_DOWN;
    W->AssocPending = 0;
    W->ConnStatus = Status ? Status : E_STATUS_FAIL;
    W->ConnEvent = E->Type;
    W->Events |= BWL_EV_ASSOC;
    W->JoinFlags = 0;
}

static void
JoinStep(bwl_dev *Dev, const bwl_event *E, u32 Bit)
{
    bwl_wlan *w = &Dev->Wlan;
    static const u8 zero[6] = { 0, 0, 0, 0, 0, 0 };

    if (Bit == JOIN_OK && BwlMemCmp(E->Addr, zero, 6) != 0) {
        BwlMemCpy(w->Bssid, E->Addr, 6);
    }
    w->JoinFlags |= Bit;
    /* brcmf_is_linkup: the join alone, or with the firmware supplicant
     * the join and the completed handshake */
    if ((w->JoinFlags & JOIN_OK) && (!w->FwSup || (w->JoinFlags & JOIN_KEYED))) {
        w->AssocPending = 1;
        w->ConnEvent = E->Type;
    }
}

static int
LinkDownEvent(const bwl_event *E)
{
    return E->Type == E_DEAUTH || E->Type == E_DEAUTH_IND || E->Type == E_DISASSOC_IND ||
           (E->Type == E_LINK && !(E->Flags & EVENT_FLAG_LINK));
}

void
BwlWlanEvent(bwl_dev *Dev, const bwl_event *E)
{
    bwl_wlan *w = &Dev->Wlan;

    switch (E->Type) {
    case E_ESCAN_RESULT:
        EscanEvent(Dev, E);
        return;
    case E_SET_SSID:
        if (w->Link != BWL_LINK_CONNECTING) {
            return;
        }
        if (E->Status == E_STATUS_SUCCESS) {
            JoinStep(Dev, E, JOIN_OK);
        } else {
            JoinFailed(w, E, E->Status);
        }
        return;
    case E_PSK_SUP:
        if (!w->FwSup || w->Link != BWL_LINK_CONNECTING) {
            return;
        }
        if (E->Status == E_STATUS_FWSUP_DONE) {
            JoinStep(Dev, E, JOIN_KEYED);
        } else {
            JoinFailed(w, E, E->Status | 0x100u);    /* told apart from the join's statuses */
        }
        return;
    default:
        break;
    }
    if (E->Type == E_LINK && (E->Flags & EVENT_FLAG_LINK)) {
        if (w->Link == BWL_LINK_CONNECTING) {
            JoinStep(Dev, E, JOIN_OK);
        }
        return;
    }
    if (LinkDownEvent(E)) {
        if (w->Link == BWL_LINK_CONNECTING) {
            /* a link-down left from an earlier connection says nothing
             * about this join; a deauthentication does */
            if (E->Type != E_LINK) {
                JoinFailed(w, E, E->Reason ? E->Reason : E_STATUS_FAIL);
            }
            return;
        }
        if (w->Link != BWL_LINK_DOWN && !w->AssocPending) {
            w->Link = BWL_LINK_DOWN;
            w->DisconnectReason = E->Reason;
            w->DisconnectEvent = E->Type;
            w->DisconnectByPeer = (u8)(E->Type != E_LINK);
            w->Events |= BWL_EV_DISCONNECT;
        }
    }
}

/*
 * The join succeeded: read what went over the air (brcmf_get_assoc_ies).
 * "assoc_info" is wl_assoc_info_t: request and response lengths, flags,
 * the request's capability and listen interval, the reassociation BSSID,
 * the response's capability, status and AID.
 */
static void
ReadAssocIes(bwl_dev *Dev)
{
    bwl_wlan *w = &Dev->Wlan;
    u8 info[32];
    u32 reql, rspl, got = 0;

    BwlMemSet(info, 0, (u32)sizeof(info));
    if (BwlGetVar(Dev, "assoc_info", info, (u32)sizeof(info), &got, NULL) != 0) {
        return;
    }
    reql = BwlGetLe32(info + 0);
    rspl = BwlGetLe32(info + 4);
    if (got >= 28) {
        w->AssocCapability = BwlGetLe16(info + 12);
        w->AssocListen = BwlGetLe16(info + 14);
        w->AssocAid = (u16)(BwlGetLe16(info + 26) & 0x3FFF);
    }
    if (reql != 0 && reql <= BWL_ASSOC_IE_MAX &&
        BwlGetVar(Dev, "assoc_req_ies", w->AssocReqIe, reql, NULL, NULL) == 0) {
        w->AssocReqIeLen = reql;
    }
    if (rspl != 0 && rspl <= BWL_ASSOC_IE_MAX &&
        BwlGetVar(Dev, "assoc_resp_ies", w->AssocRspIe, rspl, NULL, NULL) == 0) {
        w->AssocRspIeLen = rspl;
    }
}

int
BwlWlanService(bwl_dev *Dev)
{
    bwl_wlan *w = &Dev->Wlan;

    if (w->AssocPending && w->Link == BWL_LINK_CONNECTING) {
        w->AssocPending = 0;
        ReadAssocIes(Dev);
        if (w->Link != BWL_LINK_CONNECTING) {
            return BWL_OK;      /* it went down while the elements were read */
        }
        /* an open network, or keys from the firmware supplicant: the port is open */
        w->Link = (w->Target.Security == BWL_SEC_OPEN || w->FwSup) ? BWL_LINK_UP : BWL_LINK_ASSOCIATED;
        w->ConnStatus = 0;
        w->Events |= BWL_EV_ASSOC | (w->FwSup ? BWL_EV_KEYED : 0u);
        w->JoinFlags = 0;
    }
    return BWL_OK;
}

/* ------------------------------------------------------------ association bodies */

u32
BwlAssocReqBody(const bwl_dev *Dev, u8 *Out, u32 Max)
{
    const bwl_wlan *w = &Dev->Wlan;
    u32 n = 4;
    u16 cap = w->AssocCapability ? w->AssocCapability : (u16)((w->Target.Capability & 0x0431u) | 0x0001u);

    if (Max < 4) {
        return 0;
    }
    BwlPutLe16(Out, cap);
    BwlPutLe16(Out + 2, w->AssocListen ? w->AssocListen : 10);
    if (w->AssocReqIeLen != 0) {
        if (n + w->AssocReqIeLen > Max) {
            return 0;
        }
        BwlMemCpy(Out + n, w->AssocReqIe, w->AssocReqIeLen);
        return n + w->AssocReqIeLen;
    }
    /* the firmware gave no elements: what it was told to send */
    if (n + 2 + w->Target.SsidLen + w->RsnIeLen > Max) {
        return 0;
    }
    Out[n++] = 0;
    Out[n++] = w->Target.SsidLen;
    BwlMemCpy(Out + n, w->Target.Ssid, w->Target.SsidLen);
    n += w->Target.SsidLen;
    BwlMemCpy(Out + n, w->RsnIe, w->RsnIeLen);
    return n + w->RsnIeLen;
}

u32
BwlAssocRspBody(const bwl_dev *Dev, u8 *Out, u32 Max)
{
    const bwl_wlan *w = &Dev->Wlan;

    if (Max < 6u + w->AssocRspIeLen) {
        return 0;
    }
    BwlPutLe16(Out, w->AssocCapability ? w->AssocCapability : w->Target.Capability);
    BwlPutLe16(Out + 2, 0);
    BwlPutLe16(Out + 4, (u16)((w->AssocAid ? w->AssocAid : 1u) | 0xC000u));
    BwlMemCpy(Out + 6, w->AssocRspIe, w->AssocRspIeLen);
    return 6u + w->AssocRspIeLen;
}
