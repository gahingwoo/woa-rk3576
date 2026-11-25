/*
 * aictest - host test of the aic8800usb core (drivers/net/aic8800usb/aic_*.c)
 * against a simulated AIC8800D80 and access point (fakechip.c).
 *
 *   aictest                 crypto vectors, then loader, runtime, WPA2,
 *                           data path and error cases with synthetic files
 *   aictest files DIR [--fw-version HEX]
 *                           parse and download the real firmware files in
 *                           DIR (as install-firmware.ps1 fetches them)
 *                           into the simulated chip
 *
 * Exit status 0 when every check passes.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakechip.h"
#include "../../drivers/net/aic8800usb/aic_proto.h"

static int g_fail, g_checks, g_verbose;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
hex(const char *S, u8 *Out, u32 N)
{
    u32 i;

    for (i = 0; i < N; i++) {
        unsigned v;

        sscanf(S + 2 * i, "%2x", &v);
        Out[i] = (u8)v;
    }
}

static int
eqhex(const u8 *Got, const char *Want, u32 N)
{
    u8 w[256];

    hex(Want, w, N);
    return memcmp(Got, w, N) == 0;
}

/* -------------------------------------------------------------- crypto */

static void
test_crypto(void)
{
    aic_sha1 s;
    u8 out[64], key[20], in[16], k16[16];

    aic_sha1_init(&s);
    aic_sha1_update(&s, (const u8 *)"abc", 3);
    aic_sha1_final(&s, out);
    CHECK(eqhex(out, "a9993e364706816aba3e25717850c26c9cd0d89d", 20), "SHA-1 abc");

    aic_sha1_init(&s);
    {
        int i;

        u8 blk[1000];

        memset(blk, 'a', sizeof(blk));
        for (i = 0; i < 1000; i++) {
            aic_sha1_update(&s, blk, 13);           /* odd sizes across block edges */
            aic_sha1_update(&s, blk, 987);
        }
    }
    aic_sha1_final(&s, out);
    CHECK(eqhex(out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", 20), "SHA-1 one million a");

    memset(key, 0x0b, 20);
    aic_hmac_sha1(key, 20, (const u8 *)"Hi There", 8, out);
    CHECK(eqhex(out, "b617318655057264e28bc0b6fb378c8ef146be00", 20), "HMAC-SHA1 RFC 2202 case 1");
    aic_hmac_sha1((const u8 *)"Jefe", 4, (const u8 *)"what do ya want for nothing?", 28, out);
    CHECK(eqhex(out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20), "HMAC-SHA1 RFC 2202 case 2");

    aic_pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 1, out, 20);
    CHECK(eqhex(out, "0c60c80f961f0e71f3a9b524af6012062fe037a6", 20), "PBKDF2 RFC 6070 c=1");
    aic_pbkdf2_sha1((const u8 *)"password", 8, (const u8 *)"salt", 4, 4096, out, 20);
    CHECK(eqhex(out, "4b007901b765489abead49d926f721d065a429c1", 20), "PBKDF2 RFC 6070 c=4096");
    aic_wpa_pmk("password", 8, (const u8 *)"IEEE", 4, out);
    CHECK(eqhex(out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32),
          "PSK IEEE 802.11 annex J");

    aic_prf_sha1(key, 20, "prefix", (const u8 *)"Hi There", 8, out, 64);
    CHECK(eqhex(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                     "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a", 64),
          "PRF-512 IEEE 802.11 annex J");

    hex("000102030405060708090a0b0c0d0e0f", k16, 16);
    hex("00112233445566778899aabbccddeeff", in, 16);
    aic_aes128_encrypt(k16, in, out);
    CHECK(eqhex(out, "69c4e0d86a7b0430d8cdb78070b4c55a", 16), "AES-128 FIPS 197 C.1");
    aic_aes128_decrypt(k16, out, out);
    CHECK(memcmp(out, in, 16) == 0, "AES-128 decrypt");

    aic_aes_wrap(k16, in, 16, out);
    CHECK(eqhex(out, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24), "key wrap RFC 3394 4.1");
    {
        u8 plain[16];

        CHECK(aic_aes_unwrap(k16, out, 24, plain) == AIC_OK && memcmp(plain, in, 16) == 0, "key unwrap");
        out[3] ^= 1;
        CHECK(aic_aes_unwrap(k16, out, 24, plain) == AIC_E_MIC, "key unwrap detects corruption");
    }
}

/* ------------------------------------------------------------ the bus */

typedef struct harness {
    fakechip F;
    aic_dev D;
    aic_bus_ops Ops;
    u8 Tx[4096];
    u8 Rx[16][2048];
    u32 RxLen[16];
    u32 RxCount;
    aic_frameq Q;           /* what the driver's RX queue would hand Windows */
    int UseQ;
} harness;

static harness *g_h;

static void
hlog(void *Ctx, const char *Msg)
{
    (void)Ctx;
    if (g_verbose) {
        printf("   core: %s\n", Msg);
    }
}

static void
hrx(void *Ctx, const u8 *Frame, u32 Len)
{
    (void)Ctx;
    if (g_h->UseQ) {
        (void)aic_frameq_push(&g_h->Q, Frame, Len);
        return;
    }
    if (g_h->RxCount < 16 && Len <= 2048) {
        memcpy(g_h->Rx[g_h->RxCount], Frame, Len);
        g_h->RxLen[g_h->RxCount++] = Len;
    }
}

static harness *
harness_new(void)
{
    harness *h = calloc(1, sizeof(*h));

    g_h = h;
    fake_init(&h->F, &h->D);
    h->Ops.send = fake_send;
    h->Ops.wait = fake_wait;
    h->Ops.delay_ms = fake_delay;
    h->Ops.log = hlog;
    h->Ops.rx_eth = hrx;
    h->D.Ops = &h->Ops;
    h->D.Ctx = &h->F;
    h->D.TxBuf = h->Tx;
    h->D.TxBufSize = sizeof(h->Tx);
    aic_init(&h->D);
    return h;
}

static void
harness_free(harness *h)
{
    if (h->F.Error[0]) {
        g_fail++;
        printf("FAIL fake chip: %s\n", h->F.Error);
    }
    fake_free(&h->F);
    free(h);
}

/* --------------------------------------------------------------- loader */

typedef struct synth {
    u8 Table[512];
    u32 TableLen;
    u8 Adid[1708];
    u8 Patch[5000];
    u8 Ext0[2100];
    u8 Fmac[9000];
} synth;

static u32
pt_section(u8 *T, u32 Off, const char *Name, u32 Type, const u32 *Pairs, u32 Count)
{
    memset(T + Off, 0, 16);
    memcpy(T + Off, Name, strlen(Name));
    aic_put32(T + Off + 16, Type);
    aic_put32(T + Off + 20, Count);
    memcpy(T + Off + 24, Pairs, Count * 8);     /* host is little endian */
    return Off + 24 + Count * 8;
}

static void
make_synth(synth *S)
{
    static const u32 inf[] = { 0x1E7E9C, 0x201940, 0x1E7EA0, 0x1E0000, 0x40500150, 1, 0x40500150, 1,
                               1, 1, 0, 0x20B43C };
    static const u32 trap[] = { 0x20F600, 0xF0223201, 0x40690000, 0x9F5FC };
    static const u32 mode[] = { 0x201FD4, 0, 0x201FD8, 0, 0x201FD0, 0xFF, 0x201FC8, 9, 0x201FC4, 9,
                                0x201FE8, 0, 0x201FEC, 0, 0x201FF0, 0, 0x201FF4, 0, 0x201FF8, 0x77 };
    static const u32 pwr[] = { 0x40500128, 0x80 };
    u32 ver[6] = { 0 }, o, i;

    memset(S, 0, sizeof(*S));
    memcpy(S->Table, "AICBT_PT_TAG", 12);
    o = 16;
    o = pt_section(S->Table, o, "AICBT_PINF_T", 0, inf, 6);
    o = pt_section(S->Table, o, "AICBT_TRAP_T", 1, trap, 2);
    o = pt_section(S->Table, o, "AICBT_MODE_T", 3, mode, 10);
    o = pt_section(S->Table, o, "AICBT_POWER_ON", 4, pwr, 1);
    memcpy(ver, "u02 2025-11-05 test", 20);
    o = pt_section(S->Table, o, "AICBT_VER_INFO", 6, ver, 3);
    S->TableLen = o;
    for (i = 0; i < sizeof(S->Adid); i++) S->Adid[i] = (u8)(i * 7);
    for (i = 0; i < sizeof(S->Patch); i++) S->Patch[i] = (u8)(i * 13 + 1);
    for (i = 0; i < sizeof(S->Ext0); i++) S->Ext0[i] = (u8)(i * 3 + 5);
    for (i = 0; i < sizeof(S->Fmac); i++) S->Fmac[i] = (u8)(i ^ 0x5A);
    /* the pointers the patch config reads from the firmware image */
    aic_put32(S->Fmac + 0x01C, 0x06090200);         /* version: new patch map */
    aic_put32(S->Fmac + 0x198, 0x00201000);         /* config_base */
    aic_put32(S->Fmac + 0x1A0, 0x001E8000);         /* aic_patch_t */
    aic_put32(S->Fmac + 0x1A4, 0x001E9000);         /* patch pair buffer */
}

static void
fill_files(aic_fw_files *Fw, const synth *S)
{
    memset(Fw, 0, sizeof(*Fw));
    Fw->PatchTable.Data = S->Table; Fw->PatchTable.Len = S->TableLen;
    Fw->Adid.Data = S->Adid; Fw->Adid.Len = sizeof(S->Adid);
    Fw->Patch.Data = S->Patch; Fw->Patch.Len = sizeof(S->Patch);
    Fw->Ext[0].Data = S->Ext0; Fw->Ext[0].Len = sizeof(S->Ext0);
    Fw->Fmac.Data = S->Fmac; Fw->Fmac.Len = sizeof(S->Fmac);
}

static void
test_loader(void)
{
    static synth s;
    aic_fw_files fw;
    aic_patch_info info;
    harness *h = harness_new();
    fakechip *F = &h->F;
    int rc;

    make_synth(&s);
    fill_files(&fw, &s);
    rc = aic_fw_parse_table(&fw.PatchTable, &info);
    CHECK(rc == AIC_OK && info.AddrAdid == 0x201940 && info.AddrPatch == 0x1E0000 && info.ExtCount == 1 &&
          info.ExtId[0] == 0 && info.ExtAddr[0] == 0x20B43C && info.Sections == 5 &&
          strcmp(info.Version, "u02 2025-11-05 test") == 0,
          "table parse: rc %d adid %x patch %x ext %u sections %u '%s'", rc, info.AddrAdid, info.AddrPatch,
          info.ExtCount, info.Sections, info.Version);

    rc = aic_identify(&h->D);
    CHECK(rc == AIC_OK && h->D.ChipRev == AIC_CHIP_REV_U02 && !h->D.ChipIsH && !h->D.ChipMcu &&
          h->D.ChipSubRev == 2, "identify: rc %d rev %x", rc, h->D.ChipRev);
    rc = aic_fw_download(&h->D, &fw, NULL);
    CHECK(rc == AIC_OK, "download rc %d (%s)", rc, h->D.LastError);
    /* the table's mode section lies inside the adid image (0x201FD4..), as
     * in the real files, so only the part before it is compared */
    CHECK(memcmp(F->Ram + (0x201940 - FAKE_RAM_BASE), s.Adid, 0x201FC4 - 0x201940) == 0, "adid in place");
    CHECK(memcmp(F->Ram + (0x1E0000 - FAKE_RAM_BASE), s.Patch, sizeof(s.Patch)) == 0, "patch in place");
    CHECK(memcmp(F->Ram + (0x20B43C - FAKE_RAM_BASE), s.Ext0, sizeof(s.Ext0)) == 0, "ext0 in place");
    CHECK(memcmp(F->Ram + (0x120000 - FAKE_RAM_BASE), s.Fmac, sizeof(s.Fmac)) == 0, "fmacfw in place");
    CHECK(F->BlockWrites == (1708 + 1023) / 1024 + (5000 + 1023) / 1024 + (2100 + 1023) / 1024 + (9000 + 1023) / 1024,
          "block writes %u", F->BlockWrites);
    /* the INF section: only its first four pairs are written */
    CHECK(fake_reg(F, 0x1E7E9C) == 0x201940 && fake_reg(F, 0x1E7EA0) == 0x1E0000 && fake_reg(F, 0x40500150) == 1,
          "INF pairs written");
    CHECK(fake_reg(F, 1) == 0, "INF pairs past the base length not written");
    CHECK(fake_reg(F, 0x20F600) == 0xF0223201 && fake_reg(F, 0x40690000) == 0x9F5FC, "trap pairs");
    CHECK(fake_reg(F, 0x201FD4) == 1 && fake_reg(F, 0x201FD8) == 0xFFFFFFFF && fake_reg(F, 0x201FD0) == 0 &&
          fake_reg(F, 0x201FC8) == 5 && fake_reg(F, 0x201FC4) == 1 && fake_reg(F, 0x201FE8) == 1500000 &&
          fake_reg(F, 0x201FEC) == 1 && fake_reg(F, 0x201FF0) == 0 && fake_reg(F, 0x201FF4) == 0x6F2F &&
          fake_reg(F, 0x201FF8) == 0x77, "BT mode section patched");
    CHECK(F->PatchMagicOk, "patch config magic numbers");
    CHECK(fake_reg(F, 0x1E9000) == 0x201000 + 0xB4 && fake_reg(F, 0x1E9004) == 0xF3010000 &&
          fake_reg(F, 0x1E9008) == 0x201000 + 0x170 && fake_reg(F, 0x1E900C) == 0x0001000A &&
          fake_reg(F, 0x1E9010) == 0x201000 + 0x188 && fake_reg(F, 0x1E9014) == 1 &&
          fake_reg(F, 0x1E8004) == 0x1E9000, "patch pairs at the firmware's buffer");
    CHECK(F->StartAppAddr == 0x120000 && F->StartAppType == 1 && F->Reenumerated, "start app");
    CHECK(h->D.FwVersionWord == 0x06090200, "firmware version word");
    harness_free(h);

    /* the chip may drop off the bus before START_APP is confirmed */
    h = harness_new();
    h->F.DropStartAppCfm = 1;
    h->D.CfmTimeoutMs = 100;
    rc = aic_identify(&h->D);
    rc = rc ? rc : aic_fw_download(&h->D, &fw, NULL);
    CHECK(rc == AIC_OK && h->F.Reenumerated, "download without START_APP confirm: rc %d", rc);
    harness_free(h);

    /* U01 is refused, a broken table is refused */
    h = harness_new();
    h->F.ChipWord = AIC_CHIP_REV_U01 << 16;
    rc = aic_identify(&h->D);
    CHECK(rc == AIC_OK && aic_fw_download(&h->D, &fw, NULL) == AIC_E_NOCHIP, "U01 refused");
    harness_free(h);
    {
        aic_file bad = fw.PatchTable;
        u8 t[512];

        memcpy(t, s.Table, s.TableLen);
        t[0] = 'X';
        bad.Data = t;
        CHECK(aic_fw_parse_table(&bad, &info) == AIC_E_FORMAT, "bad tag refused");
        memcpy(t, s.Table, s.TableLen);
        bad.Len = s.TableLen - 5;
        CHECK(aic_fw_parse_table(&bad, &info) == AIC_E_FORMAT, "truncated table refused");
    }
}

/* -------------------------------------------------------------- runtime */

static void
add_aps(fakechip *F, const char *HomePass)
{
    static const u8 b1[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    static const u8 b2[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x61 };
    static const u8 b3[6] = { 0x70, 0x20, 0x30, 0x40, 0x50, 0x62 };

    F->Aps[0].Ssid = "home"; memcpy(F->Aps[0].Bssid, b1, 6); F->Aps[0].Freq = 2437; F->Aps[0].Rssi = -48;
    F->Aps[0].Wpa2 = 1; F->Aps[0].Passphrase = HomePass;
    F->Aps[1].Ssid = "home"; memcpy(F->Aps[1].Bssid, b2, 6); F->Aps[1].Freq = 2462; F->Aps[1].Rssi = -71;
    F->Aps[1].Wpa2 = 1; F->Aps[1].Passphrase = HomePass;
    F->Aps[2].Ssid = "cafe"; memcpy(F->Aps[2].Bssid, b3, 6); F->Aps[2].Freq = 5180; F->Aps[2].Rssi = -60;
    F->ApCount = 3;
}

static harness *
runtime_up(const char *HomePass)
{
    harness *h = harness_new();
    int rc;

    h->F.Runtime = 1;
    add_aps(&h->F, HomePass);
    rc = aic_wlan_init(&h->D);
    CHECK(rc == AIC_OK, "wlan init rc %d (%s)", rc, h->D.LastError);
    return h;
}

static void
test_runtime(void)
{
    harness *h = runtime_up("correct horse battery");
    aic_dev *d = &h->D;
    fakechip *F = &h->F;
    int rc, i;
    u8 frame[1600];

    CHECK(strcmp(d->FwVersion, "fake 6.4.3.0") == 0, "firmware version '%s'", d->FwVersion);
    CHECK(d->Mac[0] == 0x88 && d->Mac[5] == 0x01 && d->Is5gSupported && d->VifUp && d->VifIdx == 0,
          "MAC, 5 GHz, VIF");
    CHECK(F->CalibSeen && F->ResetSeen && F->MeConfigSeen && F->ChanConfigSeen && F->Started && F->VifAdded,
          "init sequence complete");
    CHECK(d->LmacVersion == 0x06040300 && d->MaxSta == 10 && d->MaxVif == 4, "version confirm parsed");
    CHECK(d->Stats.RxPrint == 1, "console print skipped (%u)", d->Stats.RxPrint);

    rc = aic_scan(d, 5000);
    CHECK(rc == AIC_OK && d->BssCount == 3, "scan rc %d, %u networks", rc, d->BssCount);
    i = aic_find_bss(d, (const u8 *)"home", 4);
    CHECK(i == 0 && d->Bss[0].Security == AIC_SEC_WPA2_PSK && d->Bss[0].GroupCipher == 4 &&
          d->Bss[0].Freq == 2437 && d->Bss[0].Rssi == -48 && d->Bss[0].Seen == 2, "strongest home network");
    CHECK(d->Bss[2].Security == AIC_SEC_OPEN && d->Bss[2].Band == 1 && d->Bss[2].SsidLen == 4, "open 5 GHz network");

    aic_wpa_pmk("correct horse battery", 21, (const u8 *)"home", 4, d->Wpa.Pmk);
    d->Wpa.HavePmk = 1;
    memset(d->Wpa.Snonce, 0x11, 32);
    rc = aic_connect(d, i, 5000);
    CHECK(rc == AIC_OK && d->Link == AIC_LINK_UP, "connect rc %d link %d (%s)", rc, d->Link, d->LastError);
    CHECK(F->Msg2Ok && F->Msg4Ok && F->PairwiseKeyOk && F->GroupKeyOk && F->PortOpen == 1,
          "4-way handshake: msg2 %d msg4 %d ptk %d gtk %d port %d", F->Msg2Ok, F->Msg4Ok, F->PairwiseKeyOk,
          F->GroupKeyOk, F->PortOpen);
    CHECK(d->ApIdx == 3 && d->ApQos == 1 && d->LinkFreq == 2437 && d->Wpa.GtkIdx == 1, "link details");

    /* RX: a broadcast ARP from the router, QoS data, CCMP */
    {
        static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        u8 arp[28];

        for (i = 0; i < 28; i++) arp[i] = (u8)(0x90 + i);
        h->RxCount = 0;
        fake_ap_send_data(F, bcast, 0x0806, arp, sizeof(arp), 1, 1);
        fake_ap_send_amsdu(F, d->Mac);
        fake_wait(F, 0);
        CHECK(h->RxCount == 3, "rx frames delivered: %u", h->RxCount);
        CHECK(h->RxLen[0] == 14 + 28 && memcmp(h->Rx[0], bcast, 6) == 0 && h->Rx[0][6] == 0x00 &&
              h->Rx[0][11] == 0x55 && h->Rx[0][12] == 0x08 && h->Rx[0][13] == 0x06 &&
              memcmp(h->Rx[0] + 14, arp, 28) == 0, "ARP frame converted");
        CHECK(h->RxLen[1] == 14 + 20 && h->RxLen[2] == 14 + 21 && h->Rx[1][14] == 0x40 && h->Rx[2][34] == 0x41 &&
              memcmp(h->Rx[1], d->Mac, 6) == 0, "A-MSDU split into two frames");
    }

    /* TX: a frame, and one that fills whole 512-byte USB packets */
    memset(frame, 0, sizeof(frame));
    memcpy(frame, "\x00\x11\x22\x33\x44\x55", 6);
    memcpy(frame + 6, d->Mac, 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    for (i = 14; i < 100; i++) frame[i] = (u8)i;
    rc = aic_tx_eth(d, frame, 100);
    CHECK(rc == AIC_OK && F->LastTxEthLen == 100 && memcmp(F->LastTxEth, frame, 100) == 0, "TX frame round trip");
    rc = aic_tx_eth(d, frame, 1024 - 4 - 28 + 14);
    CHECK(rc == AIC_OK && F->ZeroPacketSeen, "TX of a 1024-byte transfer gets the extra byte");
    CHECK(F->TxDataBad == 0, "no malformed data frames");

    /* the AP goes away */
    fake_ap_deauth(F, 7);
    fake_wait(F, 0);
    CHECK(d->Link == AIC_LINK_DOWN && d->DisconnectReason == 7, "deauthentication seen");
    harness_free(h);
}

static void
test_wrong_passphrase(void)
{
    harness *h = runtime_up("the right one");
    aic_dev *d = &h->D;
    int rc;

    d->CfmTimeoutMs = 200;
    CHECK(aic_scan(d, 5000) == AIC_OK, "scan");
    aic_wpa_pmk("the wrong one", 13, (const u8 *)"home", 4, d->Wpa.Pmk);
    d->Wpa.HavePmk = 1;
    rc = aic_connect(d, aic_find_bss(d, (const u8 *)"home", 4), 1000);
    CHECK(rc == AIC_E_AUTH && h->F.Msg2MicBad && !h->F.PairwiseKeyOk && d->Link == AIC_LINK_DOWN,
          "wrong passphrase: rc %d (%s)", rc, d->LastError);
    CHECK(strstr(d->LastError, "last message") != NULL, "error names the handshake step: %s", d->LastError);
    harness_free(h);
}

static void
test_open_network(void)
{
    harness *h = runtime_up("x");
    aic_dev *d = &h->D;
    int rc;

    CHECK(aic_scan(d, 5000) == AIC_OK, "scan");
    rc = aic_connect(d, aic_find_bss(d, (const u8 *)"cafe", 4), 1000);
    CHECK(rc == AIC_OK && d->Link == AIC_LINK_UP && h->F.KeyAdds == 0 && h->F.TxEapol == 0,
          "open network: rc %d", rc);
    CHECK(aic_find_bss(d, (const u8 *)"nowhere", 7) < 0, "unknown SSID not found");
    harness_free(h);
}


/* ------------------------------------------------- Ethernet end to end
 *
 * What Windows does over the adapter, done here by hand: DHCP discover,
 * offer, request, ack; ARP for the gateway; ping it. Frames go out the way
 * the driver's TX queue sends them (aic_build_tx, data pipe) and come back
 * through the core's receive path into an aic_frameq, as the driver's RX
 * queue takes them.
 */

static void
os_send(harness *h, const u8 *Eth, u32 Len)
{
    u8 b[2100];
    int n = aic_build_tx(&h->D, Eth, Len, b, sizeof(b));

    CHECK(n > 0, "build tx %d", n);
    if (n > 0) {
        fake_send(&h->F, 0, b, (u32)n);
    }
}

/* Next queued frame of the given ethertype (others are dropped), or 0. */
static u32
os_recv(harness *h, u16 Type, u8 *Out)
{
    const u8 *f;
    u32 len;

    fake_wait(&h->F, 0);
    while ((f = aic_frameq_peek(&h->Q, &len)) != NULL) {
        int match = aic_be16(f + 12) == Type;

        if (match) {
            memcpy(Out, f, len);
        }
        aic_frameq_pop(&h->Q);
        if (match) {
            return len;
        }
    }
    return 0;
}

static u32
ip_udp(u8 *F, const u8 *Src, u32 SrcIp, u32 DstIp, const u8 *Udp, u32 UdpLen)
{
    u8 *ip = F + 14;

    memset(F, 0xFF, 6);
    memcpy(F + 6, Src, 6);
    aic_put_be16(F + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45;
    aic_put_be16(ip + 2, 20 + UdpLen);
    ip[8] = 128;
    ip[9] = 17;
    ip[12] = (u8)(SrcIp >> 24); ip[13] = (u8)(SrcIp >> 16); ip[14] = (u8)(SrcIp >> 8); ip[15] = (u8)SrcIp;
    ip[16] = (u8)(DstIp >> 24); ip[17] = (u8)(DstIp >> 16); ip[18] = (u8)(DstIp >> 8); ip[19] = (u8)DstIp;
    aic_put_be16(ip + 10, fake_ip_checksum(ip, 20));
    memcpy(ip + 20, Udp, UdpLen);
    return 14 + 20 + UdpLen;
}

static u32
dhcp_msg(u8 *F, const u8 *Mac, u8 Type, u32 Want)
{
    u8 u[8 + 260], *b = u + 8;
    u32 n = 240;

    memset(u, 0, sizeof(u));
    aic_put_be16(u, 68);
    aic_put_be16(u + 2, 67);
    b[0] = 1; b[1] = 1; b[2] = 6;
    b[4] = 0x12; b[5] = 0x34; b[6] = 0x56; b[7] = 0x78;
    aic_put_be16(b + 10, 0x8000);   /* broadcast replies */
    memcpy(b + 28, Mac, 6);
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    b[n++] = 53; b[n++] = 1; b[n++] = Type;
    if (Want) {
        b[n++] = 50; b[n++] = 4;
        b[n++] = (u8)(Want >> 24); b[n++] = (u8)(Want >> 16); b[n++] = (u8)(Want >> 8); b[n++] = (u8)Want;
    }
    b[n++] = 55; b[n++] = 3; b[n++] = 1; b[n++] = 3; b[n++] = 6;
    b[n++] = 255;
    aic_put_be16(u + 4, 8 + n);
    return ip_udp(F, Mac, 0, 0xFFFFFFFF, u, 8 + n);
}

/* yiaddr and the option 53 type of a DHCP reply, 0 if it is not one. */
static u8
dhcp_reply(const u8 *F, u32 Len, u32 *Yi)
{
    const u8 *ip = F + 14, *b = ip + 28, *o;

    if (Len < 14 + 28 + 240 || ip[9] != 17 || aic_be16(ip + 20) != 67 || aic_be16(ip + 22) != 68 ||
        fake_ip_checksum(ip, 20) != 0 || b[0] != 2) {
        return 0;
    }
    *Yi = (u32)b[16] << 24 | (u32)b[17] << 16 | (u32)b[18] << 8 | b[19];
    for (o = b + 240; o < F + Len && *o != 255; o += (*o == 0) ? 1 : 2 + o[1]) {
        if (*o == 53) {
            return o[2];
        }
    }
    return 0;
}

static void
test_ethernet(void)
{
    harness *h = runtime_up("correct horse battery");
    aic_dev *d = &h->D;
    u8 f[1600], r[2048];
    u32 n, yi = 0, i;
    u8 gw_mac[6];

    CHECK(aic_scan(d, 5000) == AIC_OK, "scan");
    aic_wpa_pmk("correct horse battery", 21, (const u8 *)"home", 4, d->Wpa.Pmk);
    d->Wpa.HavePmk = 1;
    CHECK(aic_connect(d, aic_find_bss(d, (const u8 *)"home", 4), 5000) == AIC_OK, "connect");
    h->UseQ = 1;

    n = dhcp_msg(f, d->Mac, 1, 0);
    os_send(h, f, n);
    n = os_recv(h, 0x0800, r);
    CHECK(n && dhcp_reply(r, n, &yi) == 2 && yi == FAKE_LEASE_IP, "DHCP offer: len %u yiaddr %08x", n, yi);
    CHECK(n && memcmp(r + 6, "\x00\x11\x22\x33\x44\x55", 6) == 0 && r[0] == 0xFF, "offer is a broadcast from the gateway");
    n = dhcp_msg(f, d->Mac, 3, yi);
    os_send(h, f, n);
    n = os_recv(h, 0x0800, r);
    CHECK(n && dhcp_reply(r, n, &yi) == 5 && yi == FAKE_LEASE_IP, "DHCP ack");
    CHECK(h->F.DhcpOffers == 1 && h->F.DhcpAcks == 1, "server saw discover and request");

    /* ARP for the gateway */
    memset(f, 0xFF, 6);
    memcpy(f + 6, d->Mac, 6);
    aic_put_be16(f + 12, 0x0806);
    {
        u8 *a = f + 14;

        aic_put_be16(a, 1); aic_put_be16(a + 2, 0x0800); a[4] = 6; a[5] = 4; aic_put_be16(a + 6, 1);
        memcpy(a + 8, d->Mac, 6);
        a[14] = 192; a[15] = 168; a[16] = 50; a[17] = 23;
        memset(a + 18, 0, 6);
        a[24] = 192; a[25] = 168; a[26] = 50; a[27] = 1;
    }
    memset(f + 42, 0, 18);
    os_send(h, f, 60);
    n = os_recv(h, 0x0806, r);
    CHECK(n >= 42 && aic_be16(r + 20) == 2 && memcmp(r, d->Mac, 6) == 0, "ARP reply for the gateway");
    memcpy(gw_mac, r + 22, 6);

    /* ping the gateway, 1400-byte payload so the frame spans USB packets */
    {
        u8 *ip = f + 14, *ic = ip + 20;
        u32 ilen = 8 + 1400;

        memcpy(f, gw_mac, 6);
        memcpy(f + 6, d->Mac, 6);
        aic_put_be16(f + 12, 0x0800);
        memset(ip, 0, 20);
        ip[0] = 0x45; aic_put_be16(ip + 2, 20 + ilen); ip[8] = 128; ip[9] = 1;
        ip[12] = 192; ip[13] = 168; ip[14] = 50; ip[15] = 23;
        ip[16] = 192; ip[17] = 168; ip[18] = 50; ip[19] = 1;
        aic_put_be16(ip + 10, fake_ip_checksum(ip, 20));
        memset(ic, 0, 8);
        ic[0] = 8; ic[5] = 1; ic[7] = 1;
        for (i = 0; i < 1400; i++) ic[8 + i] = (u8)(i * 7);
        aic_put_be16(ic + 2, fake_ip_checksum(ic, ilen));
        os_send(h, f, 14 + 20 + ilen);
        n = os_recv(h, 0x0800, r);
        CHECK(n == 14 + 20 + ilen && r[14 + 9] == 1 && r[34] == 0 && fake_ip_checksum(r + 34, ilen) == 0 &&
              memcmp(r + 42, ic + 8, 1400) == 0, "echo reply, %u bytes", n);
    }
    CHECK(h->F.IpChecksumBad == 0 && h->F.TxDataBad == 0 && h->Q.Drops == 0, "no bad frames, no drops");

    /* the FIFO itself: full, too long, order */
    {
        aic_frameq *q = calloc(1, sizeof(*q));
        u32 l;

        for (i = 0; i < AIC_FRAMEQ_SLOTS + 3; i++) {
            f[0] = (u8)i;
            (void)aic_frameq_push(q, f, 60);
        }
        CHECK(q->Drops == 4 && aic_frameq_peek(q, &l)[0] == 0 && l == 60, "frame FIFO full and ordered");
        CHECK(aic_frameq_push(q, f, AIC_FRAMEQ_MAX + 1) == 0, "frame FIFO refuses long frames");
        free(q);
    }
    harness_free(h);
}

/* Garbage must never crash the receive path (ASan build). */
static void
test_rx_fuzz(void)
{
    harness *h = runtime_up("x");
    u8 b[2048];
    u32 i, n, seed = 12345;

    for (i = 0; i < 20000; i++) {
        u32 j;

        n = (seed >> 16) % sizeof(b) + 1;
        for (j = 0; j < n; j++) {
            seed = seed * 1103515245 + 12345;
            b[j] = (u8)(seed >> 16);
        }
        if (i & 1) {
            b[2] = (u8)(i & 0x10 ? 0x11 : 0x00);
        }
        aic_rx(&h->D, b, n);
        (void)aic_rx_to_eth(b, n, b, 64);
    }
    aic_poll(&h->D);
    h->F.Error[0] = 0;
    CHECK(1, "fuzz survived");
    harness_free(h);
}

/* ------------------------------------------------------------ real files */

static u8 *
slurp(const char *Dir, const char *Name, u32 *Len)
{
    char path[1024];
    FILE *f;
    long n;
    u8 *b;

    snprintf(path, sizeof(path), "%s/%s", Dir, Name);
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *Len = (u32)n;
    return b;
}

static int
test_files(const char *Dir, unsigned long WantVer)
{
    aic_fw_files fw;
    aic_patch_info info;
    harness *h;
    u8 *buf[5];
    u32 len[5] = { 0 }, i;
    int rc;
    static const char *names[5] = {
        "fw_patch_table_8800d80_u02.bin", "fw_adid_8800d80_u02.bin", "fw_patch_8800d80_u02.bin",
        "fw_patch_8800d80_u02_ext0.bin", "fmacfw_8800d80_u02.bin",
    };

    for (i = 0; i < 5; i++) {
        buf[i] = slurp(Dir, names[i], &len[i]);
        CHECK(buf[i] != NULL, "%s/%s readable", Dir, names[i]);
        if (!buf[i]) {
            return 1;
        }
    }
    memset(&fw, 0, sizeof(fw));
    fw.PatchTable.Data = buf[0]; fw.PatchTable.Len = len[0];
    fw.Adid.Data = buf[1]; fw.Adid.Len = len[1];
    fw.Patch.Data = buf[2]; fw.Patch.Len = len[2];
    fw.Ext[0].Data = buf[3]; fw.Ext[0].Len = len[3];
    fw.Fmac.Data = buf[4]; fw.Fmac.Len = len[4];
    rc = aic_fw_parse_table(&fw.PatchTable, &info);
    printf("   table: rc %d, %u sections, %u writes, adid @%06x, patch @%06x, %u ext (ext0 @%06x), '%s'\n",
           rc, info.Sections, info.Writes, info.AddrAdid, info.AddrPatch, info.ExtCount, info.ExtAddr[0],
           info.Version);
    CHECK(rc == AIC_OK && info.ExtCount == 1 && info.ExtId[0] == 0, "real table parses");

    h = harness_new();
    rc = aic_identify(&h->D);
    rc = rc ? rc : aic_fw_download(&h->D, &fw, &info);
    CHECK(rc == AIC_OK && h->F.Reenumerated && h->F.PatchMagicOk, "real download: rc %d (%s)", rc, h->D.LastError);
    CHECK(memcmp(h->F.Ram + (0x120000 - FAKE_RAM_BASE), buf[4], len[4]) == 0, "real fmacfw in place");
    CHECK(h->F.MemWrites == info.Writes + 4 + 6 + 4, "pair writes %u (table %u)", h->F.MemWrites, info.Writes);
    printf("   fmacfw version word %08x, config base %08x, patch block %08x\n", h->D.FwVersionWord,
           aic_get32(buf[4] + 0x198), aic_get32(buf[4] + 0x1A0));
    if (WantVer) {
        CHECK(h->D.FwVersionWord == WantVer, "firmware version word %08x, want %08lx", h->D.FwVersionWord, WantVer);
    }
    harness_free(h);
    for (i = 0; i < 5; i++) {
        free(buf[i]);
    }
    return 0;
}

int
main(int argc, char **argv)
{
    if (getenv("AICTEST_VERBOSE")) {
        g_verbose = 1;
    }
    if (argc >= 3 && strcmp(argv[1], "files") == 0) {
        unsigned long ver = 0;

        if (argc >= 5 && strcmp(argv[3], "--fw-version") == 0) {
            ver = strtoul(argv[4], NULL, 16);
        }
        test_files(argv[2], ver);
    } else {
        test_crypto();
        test_loader();
        test_runtime();
        test_wrong_passphrase();
        test_open_network();
        test_ethernet();
        test_rx_fuzz();
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
