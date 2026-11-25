/*
 * aic_wlan.c - the runtime firmware (USB PID 8D81): initialisation, scan,
 * join, keys and the indications the firmware sends on its own.
 *
 * The initialisation is the order of rwnx_cfg80211_init() and rwnx_open()
 * for an AIC8800D81 in AICSemi's Linux driver (aic8800_fdrv):
 *   MM_SET_STACK_START, MM_GET_FW_VERSION, MM_SET_RF_CALIB,
 *   MM_GET_MAC_ADDR, MM_RESET, MM_VERSION, ME_CONFIG, ME_CHAN_CONFIG,
 *   then MM_START and MM_ADD_IF (station).
 * Left out for now: the TX power tables from aic_userconfig_8800d80.txt
 * (the firmware keeps its built-in ones), power save (off), HE/VHT and
 * 40/80 MHz (HT20 only for the first board tests).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic.h"
#include "aic_proto.h"

static const u8 k_chan24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const u8 k_chan5[] = {
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
    132, 136, 140, 144, 149, 153, 157, 161, 165,
};

static u16 chan_freq(u8 Band, u8 Ch)
{
    return Band == 0 ? (u16)(2407 + 5 * Ch) : (u16)(5000 + 5 * Ch);
}

static void
put_chan(u8 *P, u16 Freq, u8 Band, s8 Power)
{
    aic_put16(P, Freq);
    P[2] = Band;
    P[3] = 0;               /* flags: the vendor build sets none */
    P[4] = (u8)Power;
    P[5] = 0;
}

/* Fill an array of mac_chan_def; returns the count. */
static u32
put_channels(u8 *P, int Band, u32 Max)
{
    const u8 *list = Band == 0 ? k_chan24 : k_chan5;
    u32 n = Band == 0 ? sizeof(k_chan24) : sizeof(k_chan5), i;

    if (n > Max) {
        n = Max;
    }
    for (i = 0; i < n; i++) {
        put_chan(P + i * AIC_CHAN_DEF_LEN, chan_freq((u8)Band, list[i]), (u8)Band, Band == 0 ? 20 : 23);
    }
    return n;
}

/* ------------------------------------------------------------- init */

static int
step(aic_dev *Dev, int Rc, const char *What)
{
    if (Rc != AIC_OK && Dev->LastError[0] == 0) {
        aic_set_error(Dev, What, (u32)-Rc);
    }
    return Rc;
}

int
aic_wlan_init(aic_dev *Dev)
{
    u8 p[AIC_ME_CHAN_CONFIG_REQ_LEN], cfm[AIC_MM_GET_FW_VERSION_CFM_LEN];
    u32 n;
    int rc;

    Dev->LastError[0] = 0;

    /* system_config_8800d80(): the chip ID read; nothing is written. */
    if ((rc = step(Dev, aic_identify(Dev), "chip ID read failed (%x)")) != AIC_OK) {
        return rc;
    }

    /* MM_SET_STACK_START: on, efuse_valid 0, set_vendor_info BIT(5), no
     * firmware trace redirection (the D81 branch, USE_5G build). */
    p[0] = 1; p[1] = 0; p[2] = 0x20; p[3] = 0;
    aic_memset(cfm, 0, sizeof(cfm));
    rc = aic_cmd(Dev, AIC_MM_SET_STACK_START_REQ, AIC_TASK_MM, p, AIC_MM_SET_STACK_START_REQ_LEN,
                 AIC_MM_SET_STACK_START_CFM, cfm, 2);
    if ((rc = step(Dev, rc, "stack start failed (%x)")) != AIC_OK) {
        return rc;
    }
    Dev->Is5gSupported = cfm[0];
    Dev->VendorInfo = cfm[1];

    p[0] = 0;
    aic_memset(cfm, 0, sizeof(cfm));
    rc = aic_cmd(Dev, AIC_MM_GET_FW_VERSION_REQ, AIC_TASK_MM, p, 1,
                 AIC_MM_GET_FW_VERSION_CFM, cfm, sizeof(cfm));
    if ((rc = step(Dev, rc, "firmware version request failed (%x)")) != AIC_OK) {
        return rc;
    }
    n = cfm[0] < sizeof(Dev->FwVersion) - 1 ? cfm[0] : sizeof(Dev->FwVersion) - 1;
    if (n > 63) {
        n = 63;
    }
    aic_memcpy(Dev->FwVersion, cfm + 1, n);
    Dev->FwVersion[n] = 0;

    /* aicwf_set_rf_config_8800d80(): the calibration request. */
    aic_memset(p, 0, AIC_MM_SET_RF_CALIB_REQ_LEN);
    aic_put32(p + 0, 0x0F8F);           /* cal_cfg_24g */
    aic_put32(p + 4, 0x0F0F);           /* cal_cfg_5g */
    aic_put32(p + 8, 0x0C34C008);       /* param_alpha */
    aic_put32(p + 12, 0);               /* bt_calib_en */
    aic_put32(p + 16, 0x264203);        /* bt_calib_param */
    /* xtal_cap and xtal_cap_fine 0: not from aic_userconfig */
    rc = aic_cmd(Dev, AIC_MM_SET_RF_CALIB_REQ, AIC_TASK_MM, p, AIC_MM_SET_RF_CALIB_REQ_LEN,
                 AIC_MM_SET_RF_CALIB_CFM, cfm, AIC_MM_SET_RF_CALIB_CFM_LEN);
    if ((rc = step(Dev, rc, "RF calibration failed (%x)")) != AIC_OK) {
        return rc;
    }

    aic_put32(p, 1);
    aic_memset(cfm, 0, sizeof(cfm));
    rc = aic_cmd(Dev, AIC_MM_GET_MAC_ADDR_REQ, AIC_TASK_MM, p, 4, AIC_MM_GET_MAC_ADDR_CFM, cfm, 6);
    if ((rc = step(Dev, rc, "MAC address request failed (%x)")) != AIC_OK) {
        return rc;
    }
    aic_memcpy(Dev->Mac, cfm, 6);
    if ((Dev->Mac[0] | Dev->Mac[1] | Dev->Mac[2] | Dev->Mac[3]) == 0) {
        /* No efuse address. The Linux driver then uses a fixed one; use a
         * locally administered address instead. */
        Dev->Mac[0] = 0x02; Dev->Mac[1] = 0xA1; Dev->Mac[2] = 0xC8;
    }

    rc = aic_cmd(Dev, AIC_MM_RESET_REQ, AIC_TASK_MM, NULL, 0, AIC_MM_RESET_CFM, NULL, 0);
    if ((rc = step(Dev, rc, "reset failed (%x)")) != AIC_OK) {
        return rc;
    }

    aic_memset(cfm, 0, sizeof(cfm));
    rc = aic_cmd(Dev, AIC_MM_VERSION_REQ, AIC_TASK_MM, NULL, 0, AIC_MM_VERSION_CFM, cfm,
                 AIC_MM_VERSION_CFM_LEN);
    if ((rc = step(Dev, rc, "version request failed (%x)")) != AIC_OK) {
        return rc;
    }
    Dev->LmacVersion = aic_get32(cfm);
    Dev->Features = aic_get32(cfm + 20);
    Dev->MaxSta = aic_get16(cfm + 24);
    Dev->MaxVif = cfm[26];

    /* ME_CONFIG: HT only, 20 MHz, LDPC, 1 stream (rwnx_send_me_config_req
     * with RWNX_HT_CAPABILITIES: cap 0 | LDPC, 64K A-MPDU, 16 us density,
     * rx mask 0xff, rx highest 65, TX MCS set defined). */
    aic_memset(p, 0, AIC_ME_CONFIG_REQ_LEN);
    aic_put16(p + AIC_ME_CONFIG_HT_CAPA_INFO, 0x0001);
    p[AIC_ME_CONFIG_HT_AMPDU_PARAM] = 0x03 | (0x07 << 2);
    p[AIC_ME_CONFIG_HT_MCS + 0] = 0xFF;
    aic_put16(p + AIC_ME_CONFIG_HT_MCS + 10, 65);
    p[AIC_ME_CONFIG_HT_MCS + 12] = 0x01;
    aic_put16(p + AIC_ME_CONFIG_TX_LFT, 1000);
    p[AIC_ME_CONFIG_PHY_BW_MAX] = 0;    /* PHY_CHNL_BW_20 */
    p[AIC_ME_CONFIG_HT_SUPP] = 1;
    p[AIC_ME_CONFIG_PS_ON] = 0;
    rc = aic_cmd(Dev, AIC_ME_CONFIG_REQ, AIC_TASK_ME, p, AIC_ME_CONFIG_REQ_LEN,
                 AIC_ME_CONFIG_CFM, NULL, 0);
    if ((rc = step(Dev, rc, "ME config failed (%x)")) != AIC_OK) {
        return rc;
    }

    aic_memset(p, 0, AIC_ME_CHAN_CONFIG_REQ_LEN);
    p[AIC_ME_CHAN_CONFIG_CNT24] = (u8)put_channels(p, 0, AIC_MAX_CHAN_24G);
    if (Dev->Is5gSupported) {
        p[AIC_ME_CHAN_CONFIG_CNT5] = (u8)put_channels(p + AIC_ME_CHAN_CONFIG_5G, 1, AIC_MAX_CHAN_5G);
    }
    rc = aic_cmd(Dev, AIC_ME_CHAN_CONFIG_REQ, AIC_TASK_ME, p, AIC_ME_CHAN_CONFIG_REQ_LEN,
                 AIC_ME_CHAN_CONFIG_CFM, NULL, 0);
    if ((rc = step(Dev, rc, "channel config failed (%x)")) != AIC_OK) {
        return rc;
    }

    /* rwnx_open(): MM_START with an empty PHY config, then the VIF. */
    aic_memset(p, 0, AIC_MM_START_REQ_LEN);
    aic_put32(p + AIC_MM_START_UAPSD_TIMEOUT, 300);
    aic_put16(p + AIC_MM_START_LP_CLK, 20);
    rc = aic_cmd(Dev, AIC_MM_START_REQ, AIC_TASK_MM, p, AIC_MM_START_REQ_LEN,
                 AIC_MM_START_CFM, NULL, 0);
    if ((rc = step(Dev, rc, "start failed (%x)")) != AIC_OK) {
        return rc;
    }

    aic_memset(p, 0, AIC_MM_ADD_IF_REQ_LEN);
    p[AIC_MM_ADD_IF_TYPE] = AIC_MM_STA;
    aic_memcpy(p + AIC_MM_ADD_IF_ADDR, Dev->Mac, 6);
    aic_memset(cfm, 0xFF, 2);
    rc = aic_cmd(Dev, AIC_MM_ADD_IF_REQ, AIC_TASK_MM, p, AIC_MM_ADD_IF_REQ_LEN,
                 AIC_MM_ADD_IF_CFM, cfm, 2);
    if ((rc = step(Dev, rc, "add interface failed (%x)")) != AIC_OK) {
        return rc;
    }
    if (cfm[0] != 0) {
        aic_set_error(Dev, "add interface: status %x", cfm[0]);
        return AIC_E_FIRMWARE;
    }
    Dev->VifIdx = cfm[1];
    Dev->VifUp = 1;
    return AIC_OK;
}

/* ------------------------------------------------------------- scan */

/* Classify the RSN element (after its 2-byte header). */
static void
parse_rsn(aic_bss *B, const u8 *Ie, u32 Len)
{
    static const u8 oui[3] = { 0x00, 0x0F, 0xAC };
    u32 off = 2, n, i;
    int ccmp = 0, psk = 0;

    B->Security = AIC_SEC_RSN_OTHER;
    if (Len < 2 + 4 || aic_get16(Ie) != 1) {
        return;
    }
    if (aic_memcmp(Ie + off, oui, 3) == 0) {
        B->GroupCipher = Ie[off + 3];
    }
    off += 4;
    if (off + 2 > Len) {
        return;
    }
    n = aic_get16(Ie + off);
    off += 2;
    for (i = 0; i < n && off + 4 <= Len; i++, off += 4) {
        if (aic_memcmp(Ie + off, oui, 3) == 0 && Ie[off + 3] == 4) {
            ccmp = 1;
        }
    }
    if (off + 2 > Len) {
        return;
    }
    n = aic_get16(Ie + off);
    off += 2;
    for (i = 0; i < n && off + 4 <= Len; i++, off += 4) {
        if (aic_memcmp(Ie + off, oui, 3) == 0 && Ie[off + 3] == 2) {
            psk = 1;
        }
    }
    if (ccmp && psk && (B->GroupCipher == 4 || B->GroupCipher == 2)) {
        B->Security = AIC_SEC_WPA2_PSK;
    }
}

static void
scan_result(aic_dev *Dev, const u8 *P, u32 Len)
{
    const u8 *f = P + AIC_SCANU_RESULT_PAYLOAD;
    u32 flen, off, i;
    aic_bss *b = NULL;
    u8 ssid_len = 0;
    const u8 *ssid = NULL, *rsn = NULL;
    u32 rsn_len = 0;
    int wpa = 0;

    if (Len < AIC_SCANU_RESULT_PAYLOAD) {
        return;
    }
    flen = aic_get16(P);
    if (flen > Len - AIC_SCANU_RESULT_PAYLOAD) {
        flen = Len - AIC_SCANU_RESULT_PAYLOAD;
    }
    if (flen < 36) {
        return;
    }
    Dev->Stats.ScanResults++;
    for (i = 0; i < Dev->BssCount; i++) {
        if (aic_memcmp(Dev->Bss[i].Bssid, f + 16, 6) == 0) {
            b = &Dev->Bss[i];
            break;
        }
    }
    if (b == NULL) {
        if (Dev->BssCount >= AIC_MAX_BSS) {
            return;
        }
        b = &Dev->Bss[Dev->BssCount++];
        aic_memset(b, 0, sizeof(*b));
        aic_memcpy(b->Bssid, f + 16, 6);
    }
    b->Seen++;
    b->Freq = aic_get16(P + 4);
    b->Band = P[6];
    b->Rssi = (s8)P[9];
    b->Capability = aic_get16(f + 34);
    for (off = 36; off + 2 <= flen; off += 2 + f[off + 1]) {
        u8 id = f[off], l = f[off + 1];

        if (off + 2 + l > flen) {
            break;
        }
        if (id == 0 && l <= 32) {
            ssid = f + off + 2;
            ssid_len = l;
        } else if (id == 48) {
            rsn = f + off + 2;
            rsn_len = l;
        } else if (id == 221 && l >= 4 && f[off + 2] == 0x00 && f[off + 3] == 0x50 &&
                   f[off + 4] == 0xF2 && f[off + 5] == 0x01) {
            wpa = 1;
        }
    }
    if (ssid && (ssid_len || b->SsidLen == 0)) {
        b->SsidLen = ssid_len;
        aic_memcpy(b->Ssid, ssid, ssid_len);
    }
    if (rsn) {
        parse_rsn(b, rsn, rsn_len);
        if (rsn_len + 2 <= AIC_MAX_RSN_IE) {
            b->RsnIe[0] = 48;
            b->RsnIe[1] = (u8)rsn_len;
            aic_memcpy(b->RsnIe + 2, rsn, rsn_len);
            b->RsnIeLen = (u8)(rsn_len + 2);
        }
    } else if (wpa) {
        b->Security = AIC_SEC_WPA;
    } else if (b->Capability & 0x0010) {
        b->Security = AIC_SEC_WEP;
    } else {
        b->Security = AIC_SEC_OPEN;
    }
}

int
aic_scan(aic_dev *Dev, u32 TimeoutMs)
{
    u8 *p = Dev->Scratch;
    u32 n24, n5 = 0, waited = 0;
    int rc;

    if (!Dev->VifUp) {
        return AIC_E_STATE;
    }
    aic_memset(p, 0, AIC_SCANU_START_REQ_LEN);
    n24 = put_channels(p, 0, AIC_SCAN_CHANNEL_MAX);
    if (Dev->Is5gSupported) {
        n5 = put_channels(p + n24 * AIC_CHAN_DEF_LEN, 1, AIC_SCAN_CHANNEL_MAX - n24);
    }
    /* one wildcard SSID: an active scan */
    p[AIC_SCANU_SSID] = 0;
    aic_memset(p + AIC_SCANU_BSSID, 0xFF, 6);
    p[AIC_SCANU_VIF_IDX] = Dev->VifIdx;
    p[AIC_SCANU_CHAN_CNT] = (u8)(n24 + n5);
    p[AIC_SCANU_SSID_CNT] = 1;
    p[AIC_SCANU_NO_CCK] = 0;
    Dev->BssCount = 0;
    Dev->ScanDone = 0;
    rc = aic_cmd(Dev, AIC_SCANU_START_REQ, AIC_TASK_SCANU, p, AIC_SCANU_START_REQ_LEN,
                 AIC_SCANU_START_CFM_ADDTIONAL, NULL, 0);
    if (rc != AIC_OK) {
        return rc;
    }
    while (!Dev->ScanDone && waited < TimeoutMs && !Dev->Abort) {
        Dev->Ops->wait(Dev->Ctx, 50);
        waited += 50;
    }
    if (!Dev->ScanDone) {
        aic_set_error(Dev, "scan did not finish in %x ms", TimeoutMs);
        return AIC_E_TIMEOUT;
    }
    return Dev->ScanStatus == 0 ? AIC_OK : AIC_E_FIRMWARE;
}

int
aic_find_bss(const aic_dev *Dev, const u8 *Ssid, u32 SsidLen)
{
    int best = -1;
    u32 i;

    for (i = 0; i < Dev->BssCount; i++) {
        const aic_bss *b = &Dev->Bss[i];

        if (b->SsidLen == SsidLen && aic_memcmp(b->Ssid, Ssid, SsidLen) == 0 &&
            (best < 0 || b->Rssi > Dev->Bss[best].Rssi)) {
            best = (int)i;
        }
    }
    return best;
}

/* ------------------------------------------------------------- join */

static int
wait_for(aic_dev *Dev, u32 TimeoutMs, int (*Done)(aic_dev *))
{
    u32 waited = 0;

    while (!Done(Dev) && waited < TimeoutMs && !Dev->Abort) {
        if (!aic_poll(Dev)) {
            Dev->Ops->wait(Dev->Ctx, 20);
            waited += 20;
        }
    }
    return Done(Dev);
}

static int assoc_done(aic_dev *Dev) { return Dev->Link != AIC_LINK_ASSOCIATING; }
static int keys_done(aic_dev *Dev) { return Dev->Wpa.Done || Dev->Link == AIC_LINK_DOWN; }

int
aic_set_port(aic_dev *Dev, int Open)
{
    u8 p[AIC_ME_SET_CONTROL_PORT_REQ_LEN];

    p[0] = Dev->ApIdx;
    p[1] = (u8)(Open ? 1 : 0);
    return aic_cmd(Dev, AIC_ME_SET_CONTROL_PORT_REQ, AIC_TASK_ME, p, sizeof(p),
                   AIC_ME_SET_CONTROL_PORT_CFM, NULL, 0);
}

int
aic_key_add(aic_dev *Dev, int Pairwise, u8 KeyIdx, const u8 *Key, u32 KeyLen, u8 Cipher)
{
    u8 p[AIC_MM_KEY_ADD_REQ_LEN], cfm[2];
    int rc;

    if (KeyLen > 32) {
        return AIC_E_SPACE;
    }
    aic_memset(p, 0, sizeof(p));
    if (Pairwise) {
        p[AIC_KEY_ADD_STA_IDX] = Dev->ApIdx;
    } else {
        p[AIC_KEY_ADD_STA_IDX] = AIC_INVALID_STA;
        p[AIC_KEY_ADD_KEY_IDX] = KeyIdx;
    }
    p[AIC_KEY_ADD_KEY_LEN] = (u8)KeyLen;
    aic_memcpy(p + AIC_KEY_ADD_KEY, Key, KeyLen);
    p[AIC_KEY_ADD_CIPHER] = Cipher;
    p[AIC_KEY_ADD_INST] = Dev->VifIdx;
    p[AIC_KEY_ADD_PAIRWISE] = (u8)(Pairwise ? 1 : 0);
    cfm[0] = 0xFF;
    rc = aic_cmd(Dev, AIC_MM_KEY_ADD_REQ, AIC_TASK_MM, p, sizeof(p), AIC_MM_KEY_ADD_CFM, cfm, 2);
    if (rc == AIC_OK && cfm[0] != 0) {
        aic_set_error(Dev, "key add: status %x", cfm[0]);
        rc = AIC_E_FIRMWARE;
    }
    return rc;
}

int
aic_connect(aic_dev *Dev, int Index, u32 TimeoutMs)
{
    const aic_bss *b;
    u8 *p = Dev->Scratch, cfm[1];
    u32 flags = 0, ie_len = 0;
    int rc;

    if (Index < 0 || (u32)Index >= Dev->BssCount || !Dev->VifUp) {
        return AIC_E_STATE;
    }
    b = &Dev->Bss[Index];
    if (b->Security != AIC_SEC_OPEN && b->Security != AIC_SEC_WPA2_PSK) {
        aic_set_error(Dev, "network security %x is not supported", b->Security);
        return AIC_E_AUTH;
    }
    if (b->Security == AIC_SEC_WPA2_PSK && !Dev->Wpa.HavePmk) {
        aic_set_error(Dev, "no passphrase for a WPA2 network", 0);
        return AIC_E_STATE;
    }
    aic_memset(p, 0, AIC_SM_CONNECT_REQ_LEN);
    p[0] = b->SsidLen;
    aic_memcpy(p + 1, b->Ssid, b->SsidLen);
    aic_memcpy(p + AIC_SM_CONNECT_BSSID, b->Bssid, 6);
    put_chan(p + AIC_SM_CONNECT_CHAN, b->Freq, b->Band, 0);
    p[AIC_SM_CONNECT_CHAN + 4] = 0;
    if (b->Security == AIC_SEC_WPA2_PSK) {
        ie_len = aic_wpa_build_rsn_ie(b, p + AIC_SM_CONNECT_IE_BUF, AIC_SM_CONNECT_IE_MAX);
        aic_memcpy(Dev->AssocRsnIe, p + AIC_SM_CONNECT_IE_BUF, ie_len);
        Dev->AssocRsnIeLen = (u8)ie_len;
        flags = AIC_CONTROL_PORT_HOST | AIC_WPA_WPA2_IN_USE;
        /* ETH_P_PAE in network byte order, as cfg80211 passes it */
        p[AIC_SM_CONNECT_ETHERTYPE] = 0x88;
        p[AIC_SM_CONNECT_ETHERTYPE + 1] = 0x8E;
    }
    aic_put32(p + AIC_SM_CONNECT_FLAGS, flags);
    aic_put16(p + AIC_SM_CONNECT_IE_LEN, ie_len);
    aic_put16(p + AIC_SM_CONNECT_LISTEN, 0);
    p[AIC_SM_CONNECT_DONT_WAIT_BCMC] = 0;  /* listen_bcmc is true */
    p[AIC_SM_CONNECT_AUTH_TYPE] = AIC_AUTH_OPEN;
    p[AIC_SM_CONNECT_UAPSD] = 0x01;         /* IEEE80211_WMM_IE_STA_QOSINFO_AC_VO */
    p[AIC_SM_CONNECT_VIF_IDX] = Dev->VifIdx;

    aic_memset(&Dev->Wpa.Anonce, 0, sizeof(Dev->Wpa.Anonce));
    Dev->Wpa.HavePtk = 0;
    Dev->Wpa.Done = 0;
    Dev->Wpa.ReplayValid = 0;
    Dev->Wpa.Msg = 0;
    Dev->CurBss = (u8)Index;
    Dev->Link = AIC_LINK_ASSOCIATING;
    Dev->ConnectStatus = 0xFFFF;
    cfm[0] = 0xFF;
    rc = aic_cmd(Dev, AIC_SM_CONNECT_REQ, AIC_TASK_SM, p, AIC_SM_CONNECT_REQ_LEN,
                 AIC_SM_CONNECT_CFM, cfm, 1);
    if (rc != AIC_OK) {
        Dev->Link = AIC_LINK_DOWN;
        return rc;
    }
    if (cfm[0] != 0) {
        Dev->Link = AIC_LINK_DOWN;
        aic_set_error(Dev, "connect request refused, status %x", cfm[0]);
        return AIC_E_FIRMWARE;
    }
    if (!wait_for(Dev, TimeoutMs, assoc_done) || Dev->Link != AIC_LINK_ASSOCIATED) {
        if (Dev->Link == AIC_LINK_ASSOCIATING) {
            Dev->Link = AIC_LINK_DOWN;
            aic_set_error(Dev, "no association in %x ms", TimeoutMs);
            return AIC_E_TIMEOUT;
        }
        aic_set_error(Dev, "association failed, 802.11 status %x", Dev->ConnectStatus);
        return AIC_E_AUTH;
    }
    if (b->Security == AIC_SEC_OPEN) {
        Dev->Link = AIC_LINK_UP;
        return AIC_OK;
    }
    if (!wait_for(Dev, TimeoutMs, keys_done) || !Dev->Wpa.Done) {
        if (Dev->LastError[0] == 0) {
            aic_set_error(Dev, "4-way handshake did not complete (last message %x)", Dev->Wpa.Msg);
        }
        (void)aic_disconnect(Dev);
        return Dev->Stats.MicFailures ? AIC_E_MIC : AIC_E_AUTH;
    }
    rc = aic_set_port(Dev, 1);
    if (rc != AIC_OK) {
        return rc;
    }
    Dev->Link = AIC_LINK_UP;
    return AIC_OK;
}

int
aic_disconnect(aic_dev *Dev)
{
    u8 p[AIC_SM_DISCONNECT_REQ_LEN];
    int rc;

    if (Dev->Link == AIC_LINK_DOWN) {
        return AIC_OK;
    }
    aic_put16(p, 3);    /* WLAN_REASON_DEAUTH_LEAVING */
    p[2] = Dev->VifIdx;
    p[3] = 0;
    rc = aic_cmd(Dev, AIC_SM_DISCONNECT_REQ, AIC_TASK_SM, p, sizeof(p), AIC_SM_DISCONNECT_CFM, NULL, 0);
    Dev->Link = AIC_LINK_DOWN;
    return rc;
}

/* ------------------------------------------------------ indications */

void
aic_wlan_ind(aic_dev *Dev, u16 Id, const u8 *P, u32 Len)
{
    switch (Id) {
    case AIC_SCANU_RESULT_IND:
        scan_result(Dev, P, Len);
        break;
    case AIC_SCANU_START_CFM:
        Dev->ScanStatus = Len >= 2 ? P[1] : 0xFF;
        Dev->ScanResultCnt = Len >= 3 ? P[2] : 0;
        Dev->ScanDone = 1;
        break;
    case AIC_SM_CONNECT_IND:
        if (Len < AIC_SM_CONNECT_IND_FREQ + 2) {
            Dev->Stats.RxErrors++;
            break;
        }
        Dev->ConnectStatus = aic_get16(P);
        if (Dev->ConnectStatus == 0) {
            aic_memcpy(Dev->Bssid, P + AIC_SM_CONNECT_IND_BSSID, 6);
            Dev->ApIdx = P[AIC_SM_CONNECT_IND_AP_IDX];
            Dev->ApQos = P[AIC_SM_CONNECT_IND_QOS];
            Dev->Aid = aic_get16(P + AIC_SM_CONNECT_IND_AID);
            Dev->LinkFreq = aic_get16(P + AIC_SM_CONNECT_IND_FREQ);
            Dev->Link = AIC_LINK_ASSOCIATED;
        } else {
            Dev->Link = AIC_LINK_DOWN;
        }
        break;
    case AIC_SM_DISCONNECT_IND:
        Dev->DisconnectReason = Len >= 2 ? aic_get16(P) : 0;
        Dev->Link = AIC_LINK_DOWN;
        Dev->Wpa.Done = 0;
        break;
    case AIC_MM_FW_PANIC_IND:
    case AIC_MM_FW_ASSERT_IND:
        Dev->Stats.FwPanic++;
        aic_set_error(Dev, "firmware panic/assert indication %x", Id);
        break;
    case AIC_ME_TX_CREDITS_UPDATE_IND:
    case AIC_MM_PS_CHANGE_IND:
    case AIC_MM_TRAFFIC_REQ_IND:
    case AIC_MM_CHANNEL_SWITCH_IND:
    case AIC_MM_CHANNEL_PRE_SWITCH_IND:
    case AIC_MM_P2P_VIF_PS_CHANGE_IND:
        break;
    default:
        Dev->Stats.RxUnknown++;
        Dev->Stats.LastUnknownId = Id;
        break;
    }
}

/* ------------------------------------------------------------- poll */

int
aic_poll(aic_dev *Dev)
{
    int did = 0;

    while (Dev->EapolTail != Dev->EapolHead) {
        u8 slot = Dev->EapolTail;

        (void)aic_wpa_rx(Dev, Dev->EapolBuf[slot], Dev->EapolLen[slot]);
        Dev->EapolTail = (u8)((slot + 1) % AIC_EAPOL_QUEUE);
        did = 1;
    }
    return did;
}
