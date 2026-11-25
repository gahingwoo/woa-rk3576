/*
 * aic_wpa.c - a WPA2-PSK supplicant for the station: the 4-way handshake
 * and the group key handshake of IEEE 802.11-2016 12.7.6 and 12.7.7, with
 * HMAC-SHA1 MICs and AES key wrap (key descriptor version 2), CCMP
 * pairwise, CCMP or TKIP group.
 *
 * On Linux this is wpa_supplicant's job: AICSemi's firmware associates,
 * then hands the EAPOL frames to the host and takes the keys back with
 * MM_KEY_ADD and the port state with ME_SET_CONTROL_PORT. Written from the
 * standard for this driver.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic.h"
#include "aic_proto.h"

/* EAPOL-Key frame, from the EAPOL header on:
 *   0 version, 1 type (3 = Key), 2 body length (BE)
 *   4 descriptor type (2 = RSN), 5 key information (BE), 7 key length (BE),
 *   9 replay counter (8), 17 nonce (32), 49 key IV (16), 65 RSC (8),
 *   73 reserved (8), 81 MIC (16), 97 key data length (BE), 99 key data */
#define EK_INFO     5
#define EK_KEYLEN   7
#define EK_REPLAY   9
#define EK_NONCE    17
#define EK_RSC      65
#define EK_MIC      81
#define EK_DATALEN  97
#define EK_DATA     99

#define KI_VER_MASK     0x0007
#define KI_PAIRWISE     0x0008
#define KI_INSTALL      0x0040
#define KI_ACK          0x0080
#define KI_MIC          0x0100
#define KI_SECURE       0x0200
#define KI_ERROR        0x0400
#define KI_ENCDATA      0x1000

void
aic_wpa_pmk(const char *Passphrase, u32 PassLen, const u8 *Ssid, u32 SsidLen, u8 Pmk[32])
{
    aic_pbkdf2_sha1((const u8 *)Passphrase, PassLen, Ssid, SsidLen, 4096, Pmk, 32);
}

u32
aic_wpa_build_rsn_ie(const aic_bss *Bss, u8 *Out, u32 OutMax)
{
    static const u8 tmpl[22] = {
        48, 20, 1, 0,
        0x00, 0x0F, 0xAC, 4,        /* group: set below from the AP */
        1, 0, 0x00, 0x0F, 0xAC, 4,  /* pairwise: CCMP */
        1, 0, 0x00, 0x0F, 0xAC, 2,  /* AKM: PSK */
        0, 0,                       /* RSN capabilities */
    };

    if (OutMax < sizeof(tmpl)) {
        return 0;
    }
    aic_memcpy(Out, tmpl, sizeof(tmpl));
    Out[7] = Bss->GroupCipher ? Bss->GroupCipher : 4;
    return sizeof(tmpl);
}

static void
make_nonce(aic_dev *Dev, u8 Out[32])
{
    /* SNonce = PRF-256(random?, ...). There is no kernel RNG in the core;
     * the caller seeds Wpa.Snonce with random bytes (the driver uses the
     * performance counter and the system time); mix in a counter so two
     * handshakes never reuse one. */
    u8 seed[32 + 4 + 6];

    aic_memcpy(seed, Dev->Wpa.Snonce, 32);
    aic_put32(seed + 32, ++Dev->Wpa.NonceCounter);
    aic_memcpy(seed + 36, Dev->Mac, 6);
    aic_prf_sha1(seed, sizeof(seed), "Init Counter", Dev->Mac, 6, Out, 32);
}

static void
derive_ptk(aic_dev *Dev, const u8 *Aa)
{
    u8 data[6 + 6 + 32 + 32];
    const u8 *spa = Dev->Mac;
    int a_first = aic_memcmp(Aa, spa, 6) < 0;
    int an_first = aic_memcmp(Dev->Wpa.Anonce, Dev->Wpa.Snonce, 32) < 0;

    aic_memcpy(data, a_first ? Aa : spa, 6);
    aic_memcpy(data + 6, a_first ? spa : Aa, 6);
    aic_memcpy(data + 12, an_first ? Dev->Wpa.Anonce : Dev->Wpa.Snonce, 32);
    aic_memcpy(data + 44, an_first ? Dev->Wpa.Snonce : Dev->Wpa.Anonce, 32);
    aic_prf_sha1(Dev->Wpa.Pmk, 32, "Pairwise key expansion", data, sizeof(data), Dev->Wpa.Ptk, 48);
    Dev->Wpa.HavePtk = 1;
}

static void
calc_mic(const u8 Kck[16], const u8 *Eapol, u32 Len, u8 Mic[16])
{
    u8 h[20];

    aic_hmac_sha1(Kck, 16, Eapol, Len, h);
    aic_memcpy(Mic, h, 16);
}

static int
check_mic(const u8 Kck[16], u8 *Eapol, u32 Len)
{
    u8 got[16], want[16];

    aic_memcpy(got, Eapol + EK_MIC, 16);
    aic_memset(Eapol + EK_MIC, 0, 16);
    calc_mic(Kck, Eapol, Len, want);
    aic_memcpy(Eapol + EK_MIC, got, 16);
    return aic_memcmp(got, want, 16) == 0;
}

/* Build and send one EAPOL-Key from us to the AP. */
static int
send_key(aic_dev *Dev, const u8 *Ap, u16 Info, const u8 Replay[8], const u8 *Nonce,
         const u8 *Data, u16 DataLen)
{
    u8 f[14 + EK_DATA + AIC_MAX_RSN_IE];
    u8 *e = f + 14;
    u32 len = EK_DATA + DataLen;

    if (DataLen > AIC_MAX_RSN_IE) {
        return AIC_E_SPACE;
    }
    aic_memset(f, 0, 14 + len);
    aic_memcpy(f, Ap, 6);
    aic_memcpy(f + 6, Dev->Mac, 6);
    f[12] = 0x88;
    f[13] = 0x8E;
    e[0] = 1;                       /* 802.1X-2001, what most APs expect */
    e[1] = 3;
    aic_put_be16(e + 2, len - 4);
    e[4] = 2;
    aic_put_be16(e + EK_INFO, Info);
    aic_memcpy(e + EK_REPLAY, Replay, 8);
    if (Nonce) {
        aic_memcpy(e + EK_NONCE, Nonce, 32);
    }
    aic_put_be16(e + EK_DATALEN, DataLen);
    if (DataLen) {
        aic_memcpy(e + EK_DATA, Data, DataLen);
    }
    calc_mic(Dev->Wpa.Ptk, e, len, e + EK_MIC);
    return aic_tx_eth(Dev, f, 14 + len);
}

/* Find the GTK KDE (dd len 00-0f-ac 01) in decrypted key data. */
static int
find_gtk(aic_dev *Dev, const u8 *D, u32 Len)
{
    u32 off = 0;

    while (off + 2 <= Len) {
        u8 id = D[off], l = D[off + 1];

        if (id == 0xDD && l == 0 && off + 2 <= Len) {
            break;                  /* padding */
        }
        if (off + 2 + l > Len) {
            break;
        }
        if (id == 0xDD && l >= 6 + 16 && D[off + 2] == 0x00 && D[off + 3] == 0x0F &&
            D[off + 4] == 0xAC && D[off + 5] == 0x01) {
            u32 glen = l - 6;

            if (glen > sizeof(Dev->Wpa.Gtk)) {
                return 0;
            }
            Dev->Wpa.GtkIdx = D[off + 6] & 3;
            Dev->Wpa.GtkLen = (u8)glen;
            aic_memcpy(Dev->Wpa.Gtk, D + off + 8, glen);
            return 1;
        }
        off += 2 + l;
    }
    return 0;
}

static int
install_gtk(aic_dev *Dev)
{
    const aic_bss *b = &Dev->Bss[Dev->CurBss];
    u8 cipher = b->GroupCipher == 2 ? AIC_MAC_CIPHER_TKIP : AIC_MAC_CIPHER_CCMP;

    return aic_key_add(Dev, 0, Dev->Wpa.GtkIdx, Dev->Wpa.Gtk, Dev->Wpa.GtkLen, cipher);
}

static int
replay_ok(aic_dev *Dev, const u8 *R)
{
    u32 i;

    if (!Dev->Wpa.ReplayValid) {
        return 1;
    }
    for (i = 0; i < 8; i++) {
        if (R[i] != Dev->Wpa.Replay[i]) {
            return R[i] > Dev->Wpa.Replay[i];
        }
    }
    return 0;                       /* equal: a replay */
}

int
aic_wpa_rx(aic_dev *Dev, const u8 *Eth, u32 Len)
{
    u8 buf[AIC_EAPOL_MAX];
    u8 *e = buf;
    const u8 *ap = Dev->Bssid;     /* the authenticator is the AP itself */
    u32 elen, body, dlen;
    u16 info;
    int rc;

    if (Len < 14 + EK_DATA || Len - 14 > sizeof(buf)) {
        return AIC_E_FORMAT;
    }
    elen = Len - 14;
    aic_memcpy(buf, Eth + 14, elen);
    if (e[1] != 3 || e[4] != 2) {
        return AIC_E_FORMAT;        /* not an RSN EAPOL-Key */
    }
    body = aic_be16(e + 2);
    if (body + 4 > elen || body + 4 < EK_DATA) {
        return AIC_E_FORMAT;
    }
    elen = body + 4;
    info = aic_be16(e + EK_INFO);
    dlen = aic_be16(e + EK_DATALEN);
    if (EK_DATA + dlen > elen || (info & KI_VER_MASK) != 2) {
        return AIC_E_FORMAT;
    }
    if (!Dev->Wpa.HavePmk || Dev->Link == AIC_LINK_DOWN) {
        return AIC_E_STATE;
    }
    if (!replay_ok(Dev, e + EK_REPLAY)) {
        return AIC_E_FORMAT;
    }

    if ((info & KI_PAIRWISE) && (info & KI_ACK) && !(info & KI_MIC)) {
        /* message 1: ANonce; answer with SNonce and our RSN IE */
        aic_memcpy(Dev->Wpa.Anonce, e + EK_NONCE, 32);
        make_nonce(Dev, Dev->Wpa.Snonce);
        derive_ptk(Dev, ap);
        Dev->Wpa.Msg = 1;
        rc = send_key(Dev, ap, 2 | KI_PAIRWISE | KI_MIC, e + EK_REPLAY, Dev->Wpa.Snonce,
                      Dev->AssocRsnIe, Dev->AssocRsnIeLen);
        if (rc == AIC_OK) {
            Dev->Wpa.Msg = 2;
        }
        return rc;
    }

    if ((info & KI_PAIRWISE) && (info & KI_ACK) && (info & KI_MIC) && (info & KI_INSTALL)) {
        /* message 3: verify, take the GTK, answer, install */
        u8 plain[AIC_EAPOL_MAX];

        if (!Dev->Wpa.HavePtk || aic_memcmp(e + EK_NONCE, Dev->Wpa.Anonce, 32) != 0) {
            return AIC_E_STATE;
        }
        if (!check_mic(Dev->Wpa.Ptk, e, elen)) {
            Dev->Stats.MicFailures++;
            aic_set_error(Dev, "message 3 MIC does not verify: wrong passphrase", 0);
            return AIC_E_MIC;
        }
        aic_memcpy(Dev->Wpa.Replay, e + EK_REPLAY, 8);
        Dev->Wpa.ReplayValid = 1;
        Dev->Wpa.Msg = 3;
        if (!(info & KI_ENCDATA) || dlen < 24 || (dlen % 8) != 0 ||
            aic_aes_unwrap(Dev->Wpa.Ptk + 16, e + EK_DATA, dlen, plain) != AIC_OK) {
            aic_set_error(Dev, "message 3 key data does not unwrap (%x bytes)", dlen);
            return AIC_E_AUTH;
        }
        if (!find_gtk(Dev, plain, dlen - 8)) {
            aic_set_error(Dev, "message 3 has no GTK", 0);
            return AIC_E_AUTH;
        }
        rc = send_key(Dev, ap, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, e + EK_REPLAY, NULL, NULL, 0);
        if (rc != AIC_OK) {
            return rc;
        }
        Dev->Wpa.Msg = 4;
        /* let message 4 leave unencrypted before the key is in place */
        Dev->Ops->delay_ms(Dev->Ctx, 20);
        rc = aic_key_add(Dev, 1, 0, Dev->Wpa.Ptk + 32, 16, AIC_MAC_CIPHER_CCMP);
        if (rc == AIC_OK) {
            rc = install_gtk(Dev);
        }
        if (rc == AIC_OK) {
            Dev->Wpa.Done = 1;
        }
        return rc;
    }

    if (!(info & KI_PAIRWISE) && (info & KI_ACK) && (info & KI_MIC) && (info & KI_SECURE)) {
        /* group key handshake message 1: new GTK */
        u8 plain[AIC_EAPOL_MAX];

        if (!Dev->Wpa.HavePtk || !check_mic(Dev->Wpa.Ptk, e, elen)) {
            Dev->Stats.MicFailures++;
            return AIC_E_MIC;
        }
        aic_memcpy(Dev->Wpa.Replay, e + EK_REPLAY, 8);
        Dev->Wpa.ReplayValid = 1;
        if (dlen < 24 || (dlen % 8) != 0 ||
            aic_aes_unwrap(Dev->Wpa.Ptk + 16, e + EK_DATA, dlen, plain) != AIC_OK ||
            !find_gtk(Dev, plain, dlen - 8)) {
            return AIC_E_AUTH;
        }
        rc = send_key(Dev, ap, 2 | KI_MIC | KI_SECURE, e + EK_REPLAY, NULL, NULL, 0);
        if (rc == AIC_OK) {
            rc = install_gtk(Dev);
        }
        return rc;
    }
    return AIC_E_FORMAT;
}
