/*
 * bwltest - host tests for bcmwlsdio's portable core
 * (drivers/net/bcmwlsdio/bwl*.c), run by CI on Linux with gcc.
 *
 *   bwltest                     unit tests, then a full bring-up against
 *                               the simulated chip with synthetic files
 *   bwltest files DIR [--chip 43456|43752] [--version V] [--clm-chunks N]
 *                               the same bring-up with the real
 *                               brcmfmac<chip>-sdio.bin/.txt/.clm_blob
 *                               from DIR, against the simulated chip of
 *                               that kind (default 43456); checks the
 *                               image's trailer version and the CLM
 *                               chunk count
 *
 * Exit status 0 when every check passed.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/net/bcmwlsdio/bwl.h"
#include "../../drivers/net/bcmwlsdio/bwl_regs.h"
#include "fakechip.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    checks++; \
    if (_a != _b) { failures++; printf("FAIL %s:%d: %s == %lld, expected %s == %lld\n", \
                                       __FILE__, __LINE__, #a, _a, #b, _b); } \
} while (0)

/* ------------------------------------------------------------ unit tests */

static void
test_nvram(void)
{
    static const char src[] =
        "# comment line\r\n"
        "sromrev=11\r\n"
        "\n"
        "  boardtype=0x6e4\n"
        "#another\n"
        "xtalfreq=37400";       /* no newline at the end */
    static const unsigned char want[] =
        "sromrev=11\0boardtype=0x6e4\0xtalfreq=37400\0";
    unsigned char out[sizeof(src) + BWL_NVRAM_SLACK];
    unsigned body = sizeof(want) - 1, padded = (body + 1 + 3) & ~3u;
    int n = BwlNvramConvert((const u8 *)src, sizeof(src) - 1, out, sizeof(out));
    unsigned tok;

    CHECK_EQ(n, padded + 4);
    CHECK(memcmp(out, want, body) == 0);
    tok = out[n - 4] | out[n - 3] << 8 | out[n - 2] << 16 | (unsigned)out[n - 1] << 24;
    CHECK_EQ(tok & 0xFFFF, padded / 4);
    CHECK_EQ((tok >> 16) & 0xFFFF, (~(padded / 4)) & 0xFFFF);
    CHECK_EQ(BwlNvramConvert((const u8 *)src, sizeof(src) - 1, out, 10), BWL_E_SPACE);
    CHECK_EQ(BwlNvramConvert((const u8 *)"#only\n\n", 7, out, sizeof(out)), BWL_E_FORMAT);
}

static void
test_versions(void)
{
    unsigned char img[8192];
    static const char trailer[] =
        "43455c5-roml/43455_sdio-ag-pno Version: 7.45.96.0 CRC: 1d0e37c2 Date: Fri 2017-06-16";
    static const char ver[] = "wl0: Jun 16 2017 12:38:26 version 7.45.96 (r707987 CY) FWID 01-1813af84";
    char out[32];

    memset(img, 0xA5, sizeof(img));
    memcpy(img + sizeof(img) - sizeof(trailer), trailer, sizeof(trailer));
    CHECK_EQ(BwlFwImageVersion(img, sizeof(img), out, sizeof(out)), 0);
    CHECK(strcmp(out, "7.45.96.0") == 0);
    CHECK_EQ(BwlFwImageVersion(img, 100, out, sizeof(out)), BWL_E_FORMAT);
    CHECK_EQ(BwlFwImageVersion(img, sizeof(img), out, 4), 0);
    CHECK(strcmp(out, "7.4") == 0);

    /* the 43752's <FW-TAG> line, after an earlier "Version: " string */
    {
        static const char early[] = "built with Version: 1.2.3";
        static const char fwtag[] =
            "<FW-TAG>4362a2-roml/config_sdio_ampak_release_he Version=18.35.387.23.7 (b328500@shgit) FWID=01-f50fdff0";
        memset(img, 0xA5, sizeof(img));
        memcpy(img + 100, early, sizeof(early) - 1);
        memcpy(img + sizeof(img) - sizeof(fwtag), fwtag, sizeof(fwtag));
        CHECK_EQ(BwlFwImageVersion(img, sizeof(img), out, sizeof(out)), 0);
        CHECK(strcmp(out, "18.35.387.23.7") == 0);
        memcpy(img + sizeof(img) - 20, "Version=x", 9);    /* no digit after the tag: skipped */
        CHECK_EQ(BwlFwImageVersion(img, sizeof(img), out, sizeof(out)), 0);
        CHECK(strcmp(out, "18.35.387.23.7") == 0);
        CHECK_EQ(BwlFwImageVersion((const u8 *)"Version=", 8, out, sizeof(out)), BWL_E_FORMAT);
        CHECK_EQ(BwlFwImageVersion((const u8 *)"Version=9", 9, out, sizeof(out)), 0);
        CHECK(strcmp(out, "9") == 0);
    }

    CHECK_EQ(BwlVerString(ver, sizeof(ver), out, sizeof(out)), 0);
    CHECK(strcmp(out, "7.45.96") == 0);
    CHECK_EQ(BwlVerString("no version here", 15, out, sizeof(out)), BWL_E_FORMAT);

    img[0] = 0; img[1] = 0; img[2] = 0; img[3] = 0;
    CHECK_EQ(BwlFwImageCheck(img, sizeof(img), FAKE_RAMSIZE), BWL_E_FORMAT);
    img[0] = 0x3E; img[1] = 0xB8; img[2] = 0x98; img[3] = 0xF1;
    CHECK_EQ(BwlFwImageCheck(img, sizeof(img), FAKE_RAMSIZE), 0);
    CHECK_EQ(BwlFwImageCheck(img, sizeof(img), 4096), BWL_E_SPACE);
    CHECK_EQ(BwlFwImageCheck(img, 100, 0), BWL_E_FORMAT);
}

static void
test_chip_table(void)
{
    const bwl_chip_info *i;

    CHECK(strcmp(BwlChipFwName(0x4345, 9), "43456") == 0);
    CHECK(strcmp(BwlChipFwName(0x4345, 6), "43455") == 0);
    CHECK(strcmp(BwlChipFwName(0xAAE8, 2), "43752") == 0);
    CHECK(strcmp(BwlChipFwName(0xAAE7, 0), "43752") == 0);      /* 43751: brcmfmac loads 43752 files */
    CHECK(BwlChipFwName(0x4339, 1) == NULL);
    i = BwlChipInfo(0x4345, 9);
    CHECK(i != NULL && i->RamBase == 0x198000 && i->F2BlockSize == 0 && i->F2Watermark == 8 &&
          i->MesBusyCtrl == 0 && !i->AosNoDecode && i->SrCheck == BWL_SR_CHIPCTL3);
    i = BwlChipInfo(0xAAE8, 2);
    CHECK(i != NULL && i->RamBase == 0x170000 && i->F2BlockSize == 256 && i->F2Watermark == 0x40 &&
          i->MesBusyCtrl == (0x40 | MESBUSYCTRL_ENAB) && i->AosNoDecode && i->SrCheck == BWL_SR_RETENTION);
}

static void
test_clm_chunks(void)
{
    unsigned char blob[3000], out[2000];
    unsigned off = 0, used, chunks = 0, flags[4] = { 0 };
    int n;

    memset(blob, 0x11, sizeof(blob));
    memcpy(blob, "BLOB", 4);
    CHECK_EQ(BwlClmCheck(blob, sizeof(blob)), 0);
    CHECK_EQ(BwlClmCheck(blob + 1, sizeof(blob) - 1), BWL_E_FORMAT);
    while (off < sizeof(blob)) {
        n = BwlClmChunk(blob, sizeof(blob), off, out, sizeof(out), &used);
        CHECK(n > 0);
        if (n <= 0) {
            return;
        }
        CHECK_EQ(n, DLOAD_HDR_LEN + used);
        CHECK_EQ(out[2] | out[3] << 8, DLOAD_TYPE_CLM);
        CHECK_EQ(out[4] | out[5] << 8, used);
        CHECK(memcmp(out + DLOAD_HDR_LEN, blob + off, used) == 0);
        if (chunks < 4) {
            flags[chunks] = out[0] | out[1] << 8;
        }
        off += used;
        chunks++;
    }
    CHECK_EQ(chunks, 3);
    CHECK_EQ(flags[0], DLOAD_FLAG_HANDLER_VER | DLOAD_FLAG_BEGIN);
    CHECK_EQ(flags[1], DLOAD_FLAG_HANDLER_VER);
    CHECK_EQ(flags[2], DLOAD_FLAG_HANDLER_VER | DLOAD_FLAG_END);
    CHECK_EQ(BwlClmChunk(blob, sizeof(blob), 0, out, 100, &used), BWL_E_SPACE);
    CHECK_EQ(BwlClmChunk(blob, 10, 0, out, sizeof(out), &used), DLOAD_HDR_LEN + 10);
    CHECK_EQ(out[0] | out[1] << 8, DLOAD_FLAG_HANDLER_VER | DLOAD_FLAG_BEGIN | DLOAD_FLAG_END);
}

static void
test_sdpcm(void)
{
    unsigned char buf[2048], pl[600];
    bwl_frame f;
    int n;

    memset(pl, 0x42, sizeof(pl));
    n = BwlSdpcmBuild(buf, sizeof(buf), SDPCM_CH_CONTROL, 7, pl, 21, 512);
    CHECK_EQ(n, 36);                                    /* 12 + 21 rounded to 4 */
    CHECK_EQ(buf[0] | buf[1] << 8, 33);
    CHECK_EQ((buf[0] | buf[1] << 8) ^ (buf[2] | buf[3] << 8), 0xFFFF);
    CHECK_EQ(buf[4], 7);
    CHECK_EQ(buf[7], 12);
    CHECK_EQ(BwlSdpcmParse(buf, (unsigned)n, &f), 0);
    CHECK_EQ(f.PayloadLen, 21);
    CHECK_EQ(f.Channel, SDPCM_CH_CONTROL);
    CHECK(memcmp(f.Payload, pl, 21) == 0);

    n = BwlSdpcmBuild(buf, sizeof(buf), SDPCM_CH_DATA, 8, pl, 600, 512);
    CHECK_EQ(n, 1024);                                  /* longer than a block: whole blocks */
    n = BwlSdpcmBuild(buf, sizeof(buf), SDPCM_CH_DATA, 8, pl, 500, 512);
    CHECK_EQ(n, 512);
    CHECK_EQ(BwlSdpcmBuild(buf, 100, SDPCM_CH_DATA, 8, pl, 500, 512), BWL_E_SPACE);

    memset(buf, 0, 12);
    CHECK_EQ(BwlSdpcmParseHeader(buf, 12, &f), BWL_E_STATE);   /* nothing pending */
    buf[0] = 40; buf[2] = 0x12;
    CHECK_EQ(BwlSdpcmParseHeader(buf, 12, &f), BWL_E_FORMAT);  /* checksum */
    BwlSdpcmBuild(buf, sizeof(buf), SDPCM_CH_EVENT, 1, pl, 8, 512);
    buf[7] = 4;
    CHECK_EQ(BwlSdpcmParseHeader(buf, 12, &f), BWL_E_FORMAT);  /* data offset inside the header */
    BwlSdpcmBuild(buf, sizeof(buf), SDPCM_CH_EVENT, 1, pl, 8, 512);
    CHECK_EQ(BwlSdpcmParse(buf, 12, &f), BWL_E_FORMAT);        /* frame longer than the buffer */

    CHECK(BwlTxCredit(0xFF, 0x00));
    CHECK(!BwlTxCredit(5, 5));
    CHECK(BwlTxCredit(250, 3));
    CHECK(!BwlTxCredit(3, 250));                        /* window would be negative */
}

static void
test_bcdc_event(void)
{
    unsigned char buf[256], data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char ev[4 + 8 + 14 + 10 + 48 + 4];
    bwl_dcmd_reply r;
    bwl_event e;
    const u8 *p;
    unsigned plen, ifidx;
    int n;

    n = BwlBcdcBuildDcmd(buf, sizeof(buf), C_GET_VAR, 0, 0, 0x1234, data, 8);
    CHECK_EQ(n, 24);
    CHECK_EQ(BwlBcdcParseDcmd(buf, 24, &r), 0);
    CHECK_EQ(r.Cmd, C_GET_VAR);
    CHECK_EQ(r.ReqId, 0x1234);
    CHECK_EQ(r.Error, 0);
    CHECK_EQ(r.DataLen, 8);
    n = BwlBcdcBuildDcmd(buf, sizeof(buf), C_SET_VAR, 1, 2, 9, data, 8);
    CHECK_EQ(BwlGetLe32(buf + 8), BCDC_DCMD_SET | BCDC_DCMD_IF(2) | BCDC_DCMD_ID(9));
    CHECK_EQ(BwlBcdcBuildDcmd(buf, 20, C_SET_VAR, 1, 0, 9, data, 8), BWL_E_SPACE);
    BwlPutLe32(buf + 4, 4);                             /* reply shorter than the frame */
    CHECK_EQ(BwlBcdcParseDcmd(buf, 24, &r), 0);
    CHECK_EQ(r.DataLen, 4);

    /* BCDC data header with a 2-word data offset, then an event */
    memset(ev, 0, sizeof(ev));
    ev[0] = 0x20; ev[2] = 1; ev[3] = 2;
    {
        unsigned char *eth = ev + 4 + 8, *h = eth + 14, *m = h + 10;
        eth[12] = 0x88; eth[13] = 0x6C;
        h[5] = 0x00; h[6] = 0x10; h[7] = 0x18; h[9] = 1;
        m[7] = E_ESCAN_RESULT;                          /* type, big endian */
        m[11] = 8;                                      /* status */
        m[23] = 4;                                      /* datalen */
        m[24] = 0xAA; m[29] = 0xBB;
        m[46] = 0; m[47] = 0;
    }
    CHECK_EQ(BwlBcdcData(ev, sizeof(ev), &p, &plen, &ifidx), 0);
    CHECK_EQ(ifidx, 1);
    CHECK(p == ev + 12);
    CHECK_EQ(BwlEventParse(p, plen, &e), 0);
    CHECK_EQ(e.Type, E_ESCAN_RESULT);
    CHECK_EQ(e.Status, 8);
    CHECK_EQ(e.DataLen, 4);
    CHECK_EQ(e.Addr[0], 0xAA);
    CHECK_EQ(e.Addr[5], 0xBB);
    CHECK_EQ(BwlEventParse(p, plen - 1, &e), BWL_E_FORMAT);   /* datalen past the end */
    p = ev + 12;
    ((unsigned char *)p)[13] = 0x00;                    /* ethertype 0x8800: plain data */
    CHECK_EQ(BwlEventParse(p, plen, &e), BWL_E_STATE);
    ev[3] = 200;
    CHECK_EQ(BwlBcdcData(ev, sizeof(ev), &p, &plen, &ifidx), BWL_E_FORMAT);
}

/* ------------------------------------------------------------ bring-up */

static unsigned char bounce[64 * 1024] __attribute__((aligned(64)));

static void
dev_init(bwl_dev *d, fake_chip *f)
{
    BwlInit(d, &FakeOps, f, bounce, sizeof(bounce));
}

static void
test_xfer_split(void)
{
    fake_chip f;
    bwl_dev d;
    static unsigned char buf[8192];
    unsigned char b;

    FakeInit(&f);
    dev_init(&d, &f);
    CHECK_EQ(BwlFuncEnable(&d, 1, 1, 10), 0);
    CHECK_EQ(BwlRead8(&d, CCCR_REV, &b), 0);
    CHECK_EQ(b, 0x43);
    /* 3000 bytes on F1, 64-byte blocks, at most 32 per command: 2048 + 896 + 56 */
    {
        u32 v;
        CHECK_EQ(BwlRead32(&d, FAKE_RAMBASE, &v), 0);   /* points the window at RAM */
    }
    f.Cmd53 = f.Cmd53Block = f.Cmd53Byte = f.MaxXfer = 0;
    CHECK_EQ(BwlXfer(&d, 1, SB_ACCESS_4B, 1, 1, buf, 3000), 0);
    CHECK_EQ(f.Cmd53Block, 2);
    CHECK_EQ(f.Cmd53Byte, 1);
    CHECK_EQ(f.MaxXfer, 2048);
    CHECK_EQ(BwlXfer(&d, 1, 0, 1, 1, buf, 3), BWL_E_STATE);    /* not a multiple of 4 */
    CHECK_EQ(f.Violations, 0);
    FakeFree(&f);
}

static int
bringup(int model, const unsigned char *fw, unsigned fwlen, const unsigned char *nvtxt, unsigned nvtxtlen,
        const unsigned char *clm, unsigned clmlen, const char *expect_ver, unsigned expect_chunks)
{
    const int is43752 = model == FAKE_43752;
    fake_chip f;
    bwl_dev d;
    unsigned char *nv = malloc(nvtxtlen + BWL_NVRAM_SLACK);
    unsigned char out[256];
    char ver[64], console[1024];
    unsigned chunks = 0, clmstatus = 0, outlen = 0, v = 0;
    int nvlen, fws = 0, r, before = failures;

    FakeInitModel(&f, model);
    dev_init(&d, &f);

    nvlen = BwlNvramConvert(nvtxt, nvtxtlen, nv, nvtxtlen + BWL_NVRAM_SLACK);
    CHECK(nvlen > 0);
    f.ExpectFw = fw;
    f.ExpectFwLen = fwlen;
    f.ExpectNvram = nv;
    f.ExpectNvramLen = (unsigned)nvlen;
    f.ExpectClm = clm;
    f.ExpectClmLen = clmlen;

    r = BwlAttach(&d);
    CHECK_EQ(r, 0);
    if (r) {
        printf("  attach stopped at \"%s\" (detail 0x%x): %s\n", d.Step, d.Detail, f.LastViolation);
        goto out;
    }
    CHECK_EQ(d.Chip, is43752 ? 0xAAE8 : 0x4345);
    CHECK_EQ(d.ChipRev, is43752 ? 2 : 9);
    CHECK(strcmp(BwlChipFwName(d.Chip, d.ChipRev), is43752 ? "43752" : "43456") == 0);
    CHECK(d.Info == BwlChipInfo(d.Chip, d.ChipRev));
    CHECK_EQ(d.NumCores, is43752 ? 5 : 4);              /* CC, (PMU,) 802.11, SDIO, CR4 */
    if (is43752) {
        /* the PMU core, which has no wrapper, is the one BwlChipPmu picks */
        CHECK(BwlChipCore(&d, CORE_PMU, 0) != NULL && BwlChipCore(&d, CORE_PMU, 0)->Base == 0x18005000u);
        CHECK(BwlChipPmu(&d) == BwlChipCore(&d, CORE_PMU, 0));
    } else {
        CHECK(BwlChipPmu(&d) == BwlChipCore(&d, CORE_CHIPCOMMON, 0));
    }
    CHECK(BwlChipCore(&d, CORE_ARM_CR4, 0) != NULL && BwlChipCore(&d, CORE_ARM_CR4, 0)->Wrap == 0x18103000u);
    CHECK(BwlChipCore(&d, CORE_80211, 0) != NULL && BwlChipCore(&d, CORE_80211, 0)->Base == 0x18001000u);
    CHECK(BwlChipCore(&d, CORE_GCI, 0) == NULL);        /* no wrapper: dropped, as in bwfm */
    CHECK(BwlChipCore(&d, 0x8FF, 0) == NULL);
    CHECK_EQ(d.RamBase, f.RamBase);
    CHECK_EQ(d.RamSize, f.RamSize);
    CHECK_EQ(d.PmuRev, 28);
    CHECK_EQ(d.SrCapable, 1);
    CHECK_EQ(f.Cccr[CCCR_IOEX], 0x02);                  /* F1 on, F2 off */
    CHECK(f.SleepCsr & SLEEPCSR_KSO);
    CHECK(f.Cccr[CCCR_BRCM_CARDCTRL] & CARDCTRL_WLANRESET);
    CHECK_EQ(f.BlkSize[1], 64);
    CHECK_EQ(f.BlkSize[2], is43752 ? 256 : 512);
    CHECK_EQ(d.F2BlockSize, is43752 ? 256 : 512);

    r = BwlDownload(&d, fw, fwlen, nv, (unsigned)nvlen, 1);
    CHECK_EQ(r, 0);
    CHECK_EQ(f.BootError, 0);
    CHECK(f.Running);
    if (r || !f.Running) {
        printf("  download stopped at \"%s\" (detail 0x%x), boot error %d: %s\n",
               d.Step, d.Detail, f.BootError, f.LastViolation);
        goto out;
    }

    r = BwlStart(&d, 100);
    CHECK_EQ(r, 0);
    CHECK_EQ(d.SrEnabled, 1);
    CHECK_EQ(f.Watermark, is43752 ? 0x40 : F2_WATERMARK_DEFAULT);
    CHECK_EQ(f.DeviceCtl, is43752 ? DEVCTL_F2WM_ENAB : 0);
    CHECK_EQ(f.MesBusyCtrl, is43752 ? (0x40 | MESBUSYCTRL_ENAB) : 0);
    CHECK_EQ(f.Cccr[CCCR_BRCM_CARDCAP], is43752 ? CARDCAP_CMD_NODEC : (CARDCAP_CMD14_SUPPORT | CARDCAP_CMD14_EXT));
    CHECK_EQ(f.HostIntMask, INTSTATUS_HMB_SW_MASK | INTSTATUS_CHIPACTIVE);
    if (r) {
        printf("  start stopped at \"%s\"\n", d.Step);
        goto out;
    }

    CHECK_EQ(BwlGetInt(&d, C_GET_VERSION, &v, &fws), 0);
    CHECK_EQ(v, 2);
    CHECK_EQ(d.Stats.EventsRx, 1);                      /* the start-up event came first */
    CHECK_EQ(d.Stats.LastEvent, E_IF);
    CHECK(d.SharedAddr != 0);                           /* FWREADY in the mailbox */

    CHECK_EQ(BwlGetVar(&d, "cur_etheraddr", out, 6, &outlen, &fws), 0);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[2], 0x4c);
    CHECK_EQ(out[5], 0x38);

    r = BwlClmLoad(&d, clm, clmlen, &chunks, &clmstatus, &fws);
    CHECK_EQ(r, 0);
    CHECK_EQ(clmstatus, 0);
    CHECK_EQ(chunks, (clmlen + BWL_DLOAD_MAX - 1) / BWL_DLOAD_MAX);
    if (expect_chunks) {
        CHECK_EQ(chunks, expect_chunks);
    }
    CHECK_EQ(f.ClmBad, 0);

    memset(out, 0, sizeof(out));
    CHECK_EQ(BwlGetVar(&d, "ver", out, sizeof(out), &outlen, &fws), 0);
    CHECK_EQ(BwlVerString((const char *)out, outlen, ver, sizeof(ver)), 0);
    CHECK(strcmp(ver, is43752 ? "18.35.387.23.7" : "7.45.96.0") == 0);
    if (expect_ver) {
        char filever[64];
        CHECK_EQ(BwlFwImageVersion(fw, fwlen, filever, sizeof(filever)), 0);
        CHECK(strcmp(filever, expect_ver) == 0);
        printf("  image trailer version %s\n", filever);
    }

    /* an iovar the firmware does not know: error flag and status */
    CHECK_EQ(BwlGetVar(&d, "nosuchvar", out, 16, &outlen, &fws), BWL_E_FIRMWARE);
    CHECK_EQ(fws, -23);

    CHECK_EQ(BwlReadShared(&d), 0);
    CHECK(BwlConsoleRead(&d, console, sizeof(console)) > 0);
    CHECK(strstr(console, "fake RTE") != NULL);

    CHECK_EQ(f.Cmd53F2Block, 0);                        /* F2 byte mode only */
    CHECK_EQ(f.Violations, 0);
    if (f.Violations) {
        printf("  last violation: %s\n", f.LastViolation);
    }
    printf("  %u CMD52, %u CMD53 (%u block, %u byte), %u bytes over F1, %u commands, %u frames in\n",
           f.Cmd52, f.Cmd53, f.Cmd53Block, f.Cmd53Byte, f.BytesF1, f.DcmdSeen, d.Stats.FramesRx);
out:
    FakeFree(&f);
    free(nv);
    return failures == before ? 0 : 1;
}

/* Firmware, NVRAM and CLM made up for the self-test. */
static void
synthetic_files_trailer(const char *trailer, unsigned char **fw, unsigned *fwlen, unsigned char **nv,
                        unsigned *nvlen, unsigned char **clm, unsigned *clmlen)
{
    static const char nvram[] = "#test\nsromrev=11\nboardtype=0x6e4\nxtalfreq=37400\nmacaddr=00:90:4c:c5:12:38\n";
    unsigned i, x = 12345;

    *fwlen = 300001;            /* odd on purpose: the tail is padded */
    *fw = malloc(*fwlen);
    for (i = 0; i < *fwlen; i++) {
        x = x * 1103515245u + 12345u;
        (*fw)[i] = (unsigned char)(x >> 16);
    }
    (*fw)[0] = 0x3E; (*fw)[1] = 0xB8; (*fw)[2] = 0x98; (*fw)[3] = 0xF1;
    memcpy(*fw + *fwlen - strlen(trailer) - 1, trailer, strlen(trailer) + 1);
    *nvlen = sizeof(nvram) - 1;
    *nv = malloc(*nvlen);
    memcpy(*nv, nvram, *nvlen);
    *clmlen = 7163;
    *clm = malloc(*clmlen);
    for (i = 0; i < *clmlen; i++) {
        (*clm)[i] = (unsigned char)(i * 7);
    }
    memcpy(*clm, "BLOB", 4);
}

static void
synthetic_files(unsigned char **fw, unsigned *fwlen, unsigned char **nv, unsigned *nvlen,
                unsigned char **clm, unsigned *clmlen)
{
    synthetic_files_trailer("43455c5-roml/bwltest Version: 7.45.96.0 CRC: 0 Date: 2026",
                            fw, fwlen, nv, nvlen, clm, clmlen);
}

static void
test_faults(void)
{
    unsigned char *fw, *nv, *clm, conv[256];
    unsigned fwlen, nvlen, clmlen;
    fake_chip f;
    bwl_dev d;
    int n;

    synthetic_files(&fw, &fwlen, &nv, &nvlen, &clm, &clmlen);
    n = BwlNvramConvert(nv, nvlen, conv, sizeof(conv));

    /* a RAM byte corrupted on its way in must fail the verify, and the CPU must stay halted */
    FakeInit(&f);
    dev_init(&d, &f);
    CHECK_EQ(BwlAttach(&d), 0);
    f.FlipRamAt = 77777;
    CHECK_EQ(BwlDownload(&d, fw, fwlen, conv, (unsigned)n, 1), BWL_E_VERIFY);
    CHECK_EQ(d.Detail, FAKE_RAMBASE + 65536u);     /* start of the 32 KB chunk that differs */
    CHECK(!f.Running);
    CHECK_EQ(BwlStart(&d, 10), BWL_E_STATE);
    FakeFree(&f);

    /* an I/O error in the middle of the download */
    FakeInit(&f);
    dev_init(&d, &f);
    CHECK_EQ(BwlAttach(&d), 0);
    f.FailCmd53At = f.Cmd53 + 40;
    CHECK_EQ(BwlDownload(&d, fw, fwlen, conv, (unsigned)n, 1), BWL_E_IO);
    CHECK(!f.Running);
    FakeFree(&f);

    /* a firmware image bigger than RAM is refused before anything is written */
    FakeInit(&f);
    dev_init(&d, &f);
    CHECK_EQ(BwlAttach(&d), 0);
    CHECK_EQ(BwlDownload(&d, fw, FAKE_RAMSIZE, conv, (unsigned)n, 1), BWL_E_SPACE);
    FakeFree(&f);

    /* no bounce buffer: refused before any bus access */
    FakeInit(&f);
    dev_init(&d, &f);
    d.Bounce = NULL;
    CHECK_EQ(BwlAttach(&d), BWL_E_SPACE);
    FakeFree(&f);

    free(fw);
    free(nv);
    free(clm);
}

/* ------------------------------------------------------------ WLAN (scan, join, data) */

/* What the core hands the "front end" through bwl_net_ops. */
static struct {
    bwl_bss Bss[16];
    unsigned char Body[16][512];
    unsigned BodyLen[16];
    unsigned NBss;
    unsigned char Rx[8][1600];
    unsigned RxLen[8];
    unsigned NRx;
} seen;

static void
net_rx(void *Ctx, const u8 *Eth, u32 Len)
{
    (void)Ctx;
    if (seen.NRx < 8 && Len <= sizeof(seen.Rx[0])) {
        memcpy(seen.Rx[seen.NRx], Eth, Len);
        seen.RxLen[seen.NRx] = Len;
    }
    seen.NRx++;
}

static void
net_scan(void *Ctx, const bwl_bss *Bss, const u8 *Body, u32 Len)
{
    (void)Ctx;
    if (seen.NBss < 16 && Len <= sizeof(seen.Body[0])) {
        seen.Bss[seen.NBss] = *Bss;
        memcpy(seen.Body[seen.NBss], Body, Len);
        seen.BodyLen[seen.NBss] = Len;
    }
    seen.NBss++;
}

static const bwl_net_ops test_net = { net_rx, net_scan };

/* Attach, download, start and load the CLM with synthetic files. */
static int
start_fw(fake_chip *f, bwl_dev *d, int model)
{
    unsigned char *fw, *nvt, *clm, nv[512];
    unsigned fwlen, nvtlen, clmlen, chunks, cst;
    int n, fws, r;

    if (model == FAKE_43752) {
        synthetic_files_trailer("<FW-TAG>4362a2-roml/bwltest Version=18.35.387.23.7 (bwltest) FWID=01-0",
                                &fw, &fwlen, &nvt, &nvtlen, &clm, &clmlen);
    } else {
        synthetic_files(&fw, &fwlen, &nvt, &nvtlen, &clm, &clmlen);
    }
    FakeInitModel(f, model);
    dev_init(d, f);
    n = BwlNvramConvert(nvt, nvtlen, nv, sizeof(nv));
    f->ExpectClm = clm;
    f->ExpectClmLen = clmlen;
    r = BwlAttach(d);
    if (r == 0) r = BwlDownload(d, fw, fwlen, nv, (unsigned)n, 0);
    if (r == 0) r = BwlStart(d, 100);
    if (r == 0) r = BwlClmLoad(d, clm, clmlen, &chunks, &cst, &fws);
    free(fw);
    free(nvt);
    /* the CLM stays allocated: the fake compares against it at clmload_status (done) */
    free(clm);
    f->ExpectClm = NULL;
    return r;
}

/* Run the poller until one of the events in Want is pending, at most N times. */
static int
service_until(bwl_dev *d, unsigned Want, int N)
{
    int i;

    for (i = 0; i < N; i++) {
        int r = BwlService(d);
        if (r < 0) {
            return r;
        }
        if (d->Wlan.Events & Want) {
            return 0;
        }
    }
    return BWL_E_TIMEOUT;
}

static int
find_seen(const char *ssid)
{
    unsigned i;

    for (i = 0; i < seen.NBss && i < 16; i++) {
        if (seen.Bss[i].SsidLen == strlen(ssid) && memcmp(seen.Bss[i].Ssid, ssid, seen.Bss[i].SsidLen) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* An ARP request from the station for the gateway, as Windows would send it. */
static unsigned
arp_request(const unsigned char *mac, unsigned char *eth)
{
    memset(eth, 0xFF, 6);
    memcpy(eth + 6, mac, 6);
    eth[12] = 0x08; eth[13] = 0x06;
    eth[14] = 0; eth[15] = 1; eth[16] = 0x08; eth[17] = 0; eth[18] = 6; eth[19] = 4; eth[20] = 0; eth[21] = 1;
    memcpy(eth + 22, mac, 6);
    memcpy(eth + 28, FakeStaIp, 4);
    memset(eth + 32, 0, 6);
    memcpy(eth + 38, FakeGatewayIp, 4);
    return 42;
}

/* The station's EAPOL frame number Msg to the AP, as an 802.11 frame from
 * Windows converted the way the driver does it. */
static int
send_eapol(bwl_dev *d, unsigned msg)
{
    unsigned char w[200], eth[200];
    int n;

    memset(w, 0, sizeof(w));
    w[0] = 0x08; w[1] = 0x01;                       /* data, To DS */
    memcpy(w + 4, d->Wlan.Bssid, 6);                /* A1 BSSID */
    memcpy(w + 10, d->Wlan.Mac, 6);                 /* A2 SA */
    memcpy(w + 16, d->Wlan.Bssid, 6);               /* A3 DA */
    memcpy(w + 24, "\xAA\xAA\x03\x00\x00\x00\x88\x8E", 8);
    w[32] = 2; w[33] = (unsigned char)msg; w[35] = 95;
    n = Bwl80211ToEth(w, 32 + 99, eth, sizeof(eth));
    CHECK_EQ(n, 14 + 99);
    CHECK_EQ(eth[12], 0x88);
    return BwlTxEth(d, eth, (unsigned)n, 7);
}

static void
test_wlan_pure(void)
{
    u8 ch, band, buf[512], eth[1600], w[1700];
    static const u8 rsn_tkip[] = { 48, 20, 1, 0, 0, 0x0F, 0xAC, 2, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0 };
    static const u8 rsn_sae[] = { 48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 8, 0, 0 };
    static const u8 rsn_mfpr[] = { 48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0xC0, 0 };
    static const u8 rsn_mixed[] = { 48, 24, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 2, 0, 0, 0x0F, 0xAC, 8,
                                    0, 0x0F, 0xAC, 2, 0x80, 0 };
    bwl_frameq *q;
    bwl_bss b;
    unsigned len, prio, i;
    int n;

    /* chanspecs, both encodings */
    CHECK_EQ(BwlChanspec(IOTYPE_D11AC, 6, 0), 0x1006);
    CHECK_EQ(BwlChanspec(IOTYPE_D11AC, 36, 1), 0xD024);
    CHECK_EQ(BwlChanspec(IOTYPE_D11N, 6, 0), 0x2B06);
    CHECK_EQ(BwlChanspec(IOTYPE_D11N, 149, 1), 0x1B95);
    BwlChanspecDecode(IOTYPE_D11AC, 0xE02A, &ch, &band);        /* 80 MHz at 42, lower-lower */
    CHECK_EQ(ch, 36); CHECK_EQ(band, 1);
    BwlChanspecDecode(IOTYPE_D11AC, 0xE32A, &ch, &band);        /* upper-upper */
    CHECK_EQ(ch, 48);
    BwlChanspecDecode(IOTYPE_D11AC, 0xD926, &ch, &band);        /* 40 MHz at 38, upper */
    CHECK_EQ(ch, 40);
    BwlChanspecDecode(IOTYPE_D11N, 0x2B0B, &ch, &band);
    CHECK_EQ(ch, 11); CHECK_EQ(band, 0);
    BwlChanspecDecode(IOTYPE_D11N, 0x1D26, &ch, &band);         /* 40 MHz at 38, lower */
    CHECK_EQ(ch, 36); CHECK_EQ(band, 1);

    /* security from the RSN element */
    CHECK_EQ(BwlRsnSecurity(NULL, 0x0001), BWL_SEC_OPEN);
    CHECK_EQ(BwlRsnSecurity(NULL, 0x0011), BWL_SEC_OTHER);
    CHECK_EQ(BwlBuildRsnIe(buf, sizeof(buf)), 22);
    CHECK_EQ(BwlRsnSecurity(buf, 0x0011), BWL_SEC_WPA2_PSK);
    CHECK_EQ(BwlRsnSecurity(rsn_tkip, 0x0011), BWL_SEC_OTHER);  /* TKIP group */
    CHECK_EQ(BwlRsnSecurity(rsn_sae, 0x0011), BWL_SEC_OTHER);   /* SAE only */
    CHECK_EQ(BwlRsnSecurity(rsn_mfpr, 0x0011), BWL_SEC_OTHER);  /* MFP required */
    CHECK_EQ(BwlRsnSecurity(rsn_mixed, 0x0011), BWL_SEC_WPA2_PSK);  /* WPA2/WPA3 transition */
    buf[8] = 5;                                                 /* a pairwise count past the element */
    CHECK_EQ(BwlRsnSecurity(buf, 0x0011), BWL_SEC_OTHER);

    /* a cached record without elements: SSID and rates built from it */
    memset(buf, 0, sizeof(buf));
    BwlPutLe32(buf + 4, 128);
    memcpy(buf + 8, "\x02\x01\x02\x03\x04\x05", 6);
    BwlPutLe16(buf + 14, 100);
    BwlPutLe16(buf + 16, 0x0401);
    buf[18] = 4; memcpy(buf + 19, "home", 4);
    BwlPutLe32(buf + 52, 2); buf[56] = 0x82; buf[57] = 0x84;
    BwlPutLe16(buf + 72, 0x100B);
    BwlPutLe16(buf + 78, (u16)-77);
    {
        u8 body[64];
        u32 bl = 0;
        CHECK_EQ(BwlBssInfoParse(buf, 128, IOTYPE_D11AC, &b, body, sizeof(body), &bl), 0);
        CHECK_EQ(b.Channel, 11); CHECK_EQ(b.Band, 0); CHECK_EQ(b.Rssi, -77);
        CHECK_EQ(b.Security, BWL_SEC_OPEN);
        CHECK_EQ(bl, 12 + 6 + 4);
        CHECK_EQ(body[8], 100); CHECK_EQ(body[10], 0x01); CHECK_EQ(body[11], 0x04);
        CHECK_EQ(body[12], 0); CHECK_EQ(body[13], 4); CHECK(memcmp(body + 14, "home", 4) == 0);
        CHECK_EQ(body[18], 1); CHECK_EQ(body[19], 2); CHECK_EQ(body[20], 0x82);
        CHECK_EQ(BwlBssFromBody(b.Bssid, 11, 0, -77, body, bl, &b), 0);
        CHECK_EQ(b.SsidLen, 4);
        CHECK_EQ(BwlBssInfoParse(buf, 120, IOTYPE_D11AC, &b, body, sizeof(body), &bl), BWL_E_FORMAT);
        BwlPutLe32(buf + 120, 400);                             /* elements past the record */
        BwlPutLe16(buf + 116, 128);
        CHECK_EQ(BwlBssInfoParse(buf, 128, IOTYPE_D11AC, &b, body, sizeof(body), &bl), BWL_E_FORMAT);
    }

    /* Ethernet and native 802.11 data frames */
    {
        static const u8 bssid[6] = { 2, 0x11, 0x22, 0x33, 0x44, 1 };
        static const u8 ipx[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x81, 0x37, 0xAB, 0xCD };
        static const u8 llc[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x00, 0x03, 0x42, 0x42, 0x03, 0, 0, 0 };
        u8 back[1600];

        memset(eth, 0, sizeof(eth));
        memcpy(eth, "\x00\x90\x4c\xc5\x12\x38\x02\xaa\xbb\xcc\xdd\x01\x08\x00", 14);
        for (i = 14; i < 1514; i++) eth[i] = (u8)i;
        n = BwlEthTo80211(bssid, eth, 1514, w, sizeof(w));
        CHECK_EQ(n, 24 + 8 + 1500);
        CHECK_EQ(w[0], 0x08); CHECK_EQ(w[1], 0x02);
        CHECK(memcmp(w + 4, eth, 6) == 0 && memcmp(w + 10, bssid, 6) == 0 && memcmp(w + 16, eth + 6, 6) == 0);
        CHECK(memcmp(w + 24, "\xAA\xAA\x03\x00\x00\x00\x08\x00", 8) == 0);
        /* back again as Windows would send it: To DS, QoS with +HTC */
        {
            u8 t[1700];
            memset(t, 0, 30);
            t[0] = 0x88; t[1] = 0x81;
            memcpy(t + 4, bssid, 6); memcpy(t + 10, eth + 6, 6); memcpy(t + 16, eth, 6);
            memcpy(t + 30, w + 24, (size_t)n - 24);
            CHECK_EQ(Bwl80211ToEth(t, 30 + (unsigned)n - 24, back, sizeof(back)), 1514);
            CHECK(memcmp(back, eth, 6) == 0 && memcmp(back + 6, eth + 6, 6) == 0);
            CHECK(memcmp(back + 12, eth + 12, 1502) == 0);
            t[1] = 0x41;                                        /* protected: refused */
            CHECK_EQ(Bwl80211ToEth(t, 30 + (unsigned)n - 24, back, sizeof(back)), 0);
            t[0] = 0x80; t[1] = 0x01;                           /* a beacon: not data */
            CHECK_EQ(Bwl80211ToEth(t, 60, back, sizeof(back)), 0);
            t[0] = 0x08; t[1] = 0x03;                           /* four addresses: refused */
            CHECK_EQ(Bwl80211ToEth(t, 60, back, sizeof(back)), 0);
            CHECK_EQ(Bwl80211ToEth(t, 20, back, sizeof(back)), 0);
        }
        n = BwlEthTo80211(bssid, ipx, sizeof(ipx), w, sizeof(w));
        CHECK_EQ(n, 24 + 8 + 2);
        CHECK(memcmp(w + 24, "\xAA\xAA\x03\x00\x00\xF8\x81\x37", 8) == 0);   /* 802.1H */
        n = BwlEthTo80211(bssid, llc, sizeof(llc), w, sizeof(w));       /* an 802.3 length of 3 */
        CHECK_EQ(n, 24 + 3);
        CHECK_EQ(BwlEthTo80211(bssid, eth, 1514, w, 100), BWL_E_SPACE);
        CHECK_EQ(BwlEthTo80211(bssid, eth, 10, w, sizeof(w)), BWL_E_FORMAT);
        /* an LLC frame without SNAP from Windows goes as 802.3 with its length */
        memset(w, 0, 24); w[0] = 0x08; w[1] = 0x01;
        memcpy(w + 24, "\x42\x42\x03\x01", 4);
        n = Bwl80211ToEth(w, 28, back, sizeof(back));
        CHECK_EQ(n, 18);
        CHECK_EQ(back[12], 0); CHECK_EQ(back[13], 4);
    }

    /* the frame FIFO */
    q = calloc(1, sizeof(*q));
    CHECK(BwlFrameqPeek(q, &len, &prio) == NULL);
    for (i = 0; i < BWL_FRAMEQ_SLOTS - 1; i++) {
        eth[0] = (u8)i;
        CHECK_EQ(BwlFrameqPush(q, eth, 60 + i, i), 1);
    }
    CHECK_EQ(BwlFrameqPush(q, eth, 60, 0), 0);                  /* full */
    CHECK_EQ(BwlFrameqCount(q), BWL_FRAMEQ_SLOTS - 1);
    CHECK(BwlFrameqPeek(q, &len, &prio) != NULL);
    CHECK_EQ(len, 60); CHECK_EQ(prio, 0);
    BwlFrameqPop(q);
    CHECK(BwlFrameqPeek(q, &len, &prio)[0] == 1);
    CHECK_EQ(prio, 1);
    CHECK_EQ(BwlFrameqPush(q, eth, BWL_FRAMEQ_MAX + 1, 0), 0);  /* too long */
    CHECK_EQ(q->Drops, 2);
    free(q);
}

static void
test_wlan(int model)
{
    const int is43752 = model == FAKE_43752;
    fake_chip f;
    bwl_dev d;
    unsigned char eth[64], body[600];
    unsigned i, before;
    int open, psk, wep, r;
    bwl_bss b;

    printf("WLAN: %s\n", is43752 ? "simulated BCM43752" : "simulated BCM43456");
    memset(&seen, 0, sizeof(seen));
    r = start_fw(&f, &d, model);
    CHECK_EQ(r, 0);
    if (r) {
        printf("  start stopped at \"%s\": %s\n", d.Step, f.LastViolation);
        FakeFree(&f);
        return;
    }
    d.Wlan.Net = &test_net;

    /* firmware set-up */
    CHECK_EQ(BwlWlanInit(&d, "DE"), 0);
    CHECK(d.Wlan.Up && f.Up);
    CHECK_EQ(d.Wlan.IoType, IOTYPE_D11AC);
    CHECK_EQ(d.Wlan.Has5g, 1);
    CHECK_EQ(d.Wlan.FwSupCapable, is43752 ? 0 : 1);
    CHECK_EQ(d.Wlan.Mac[2], 0x4c);
    CHECK_EQ(f.Mpc, 0); CHECK_EQ(f.Pm, 0); CHECK_EQ(f.Infra, 1); CHECK_EQ(f.RoamOff, 1);
    CHECK_EQ(f.Country, ('D' << 8) | 'E');
    CHECK(f.EventMaskSet);
    CHECK(f.EventMask[E_ESCAN_RESULT / 8] & (1u << (E_ESCAN_RESULT % 8)));
    CHECK(f.EventMask[E_PSK_SUP / 8] & (1u << (E_PSK_SUP % 8)));
    CHECK(f.EventMask[E_LINK / 8] & (1u << (E_LINK % 8)));
    CHECK(f.EventMask[E_SET_SSID / 8] & 1u);

    /* scan */
    CHECK_EQ(BwlScanStart(&d), 0);
    CHECK_EQ(service_until(&d, BWL_EV_SCAN_DONE, 20), 0);
    CHECK_EQ(d.Wlan.ScanStatus, 0);
    CHECK_EQ(seen.NBss, FAKE_NAPS);
    open = find_seen("bwltest-open");
    psk = find_seen("bwltest-psk");
    wep = find_seen("bwltest-wep");
    CHECK(open >= 0 && psk >= 0 && wep >= 0);
    if (open < 0 || psk < 0 || wep < 0) {
        FakeFree(&f);
        return;
    }
    CHECK_EQ(seen.Bss[open].Channel, 6); CHECK_EQ(seen.Bss[open].Band, 0);
    CHECK_EQ(seen.Bss[open].Rssi, -48); CHECK_EQ(seen.Bss[open].Security, BWL_SEC_OPEN);
    CHECK_EQ(seen.Bss[open].Beacon, 1);
    CHECK_EQ(seen.Bss[psk].Channel, 36); CHECK_EQ(seen.Bss[psk].Band, 1);   /* from the 80 MHz chanspec */
    CHECK_EQ(seen.Bss[psk].Security, BWL_SEC_WPA2_PSK);
    CHECK_EQ(seen.Bss[psk].Beacon, 0);
    CHECK_EQ(seen.Bss[wep].Security, BWL_SEC_OTHER);
    /* the bodies, as Windows gets them: Timestamp, interval, capability, SSID first */
    for (i = 0; i < 8; i++) CHECK_EQ(seen.Body[psk][i], 0);
    CHECK_EQ(seen.Body[psk][8], 100);
    CHECK_EQ(seen.Body[psk][10] & 0x11, 0x11);
    CHECK_EQ(seen.Body[psk][12], 0);
    CHECK_EQ(seen.Body[psk][13], 11);
    /* the network as Windows hands it back for the connect */
    CHECK_EQ(BwlBssFromBody(seen.Bss[psk].Bssid, 36, 1, -61, seen.Body[psk], seen.BodyLen[psk], &b), 0);
    CHECK_EQ(b.Security, BWL_SEC_WPA2_PSK);
    CHECK(memcmp(b.Ssid, "bwltest-psk", 11) == 0);
    /* a second scan, the results again */
    seen.NBss = 0;
    CHECK_EQ(BwlScanStart(&d), 0);
    CHECK_EQ(service_until(&d, BWL_EV_SCAN_DONE, 20), 0);
    CHECK_EQ(seen.NBss, FAKE_NAPS);

    /* open network: join, ARP to the gateway, leave */
    d.Wlan.Events = 0;
    CHECK_EQ(BwlConnect(&d, &seen.Bss[open], NULL), 0);
    CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
    CHECK_EQ(d.Wlan.ConnStatus, 0);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_UP);
    CHECK_EQ(f.Assoc, FAKE_AP_OPEN);
    CHECK_EQ(f.Wsec, 0); CHECK_EQ(f.WpaAuth, 0); CHECK_EQ(f.SupWpa, 0);
    CHECK(memcmp(d.Wlan.Bssid, FakeAps[FAKE_AP_OPEN].Bssid, 6) == 0);
    CHECK_EQ(d.Wlan.AssocAid, 5);
    {
        u32 n = BwlAssocReqBody(&d, body, sizeof(body));
        CHECK(n > 4 + 2 + 12);
        CHECK_EQ(body[4], 0); CHECK_EQ(body[5], 12);            /* the SSID element the firmware sent */
        CHECK(BwlFindIe(body + 4, n - 4, 48) == NULL);
        n = BwlAssocRspBody(&d, body, sizeof(body));
        CHECK_EQ(n, 6 + 10);
        CHECK_EQ(body[4], 5); CHECK_EQ(body[5], 0xC0);
    }
    CHECK_EQ(BwlLinkInfo(&d), 0);
    CHECK_EQ(d.Wlan.RateKbps, 65000);
    CHECK_EQ(d.Wlan.Rssi, -52);
    seen.NRx = 0;
    CHECK_EQ(BwlTxEth(&d, eth, arp_request(d.Wlan.Mac, eth), 0), 0);
    for (i = 0; i < 5 && seen.NRx == 0; i++) {
        CHECK(BwlService(&d) >= 0);
    }
    CHECK_EQ(f.ArpReplies, 1);
    CHECK_EQ(seen.NRx, 1);
    CHECK_EQ(seen.RxLen[0], 14 + 28);
    CHECK(memcmp(seen.Rx[0], d.Wlan.Mac, 6) == 0);
    CHECK(memcmp(seen.Rx[0] + 6, FakeGatewayMac, 6) == 0);
    CHECK(memcmp(seen.Rx[0] + 22, FakeGatewayMac, 6) == 0);  /* the answer: the gateway's address */
    /* many frames in a row: the credit window (4 here) holds the rest back
     * until the next frame from the firmware */
    before = f.ArpReplies;
    for (i = 0; i < 40; i++) {
        int k;
        for (k = 0; k < 50 && (r = BwlTxEth(&d, eth, 42, 0)) == BWL_E_CREDIT; k++) {
            CHECK(BwlService(&d) >= 0);
        }
        CHECK_EQ(r, 0);
    }
    for (i = 0; i < 10; i++) {
        CHECK(BwlService(&d) >= 0);
    }
    CHECK_EQ(f.ArpReplies - before, 40);
    CHECK_EQ(d.Wlan.RxData, 41);
    CHECK_EQ(BwlDisconnect(&d, 3), 0);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_DOWN);
    CHECK_EQ(f.Assoc, -1);
    for (i = 0; i < 3; i++) {
        CHECK(BwlService(&d) >= 0);
    }
    CHECK_EQ(d.Wlan.Events & BWL_EV_DISCONNECT, 0);         /* our own leaving is not a drop */
    CHECK_EQ(BwlTxEth(&d, eth, 42, 0), 0);                  /* sent, and dropped by the firmware */
    CHECK(BwlService(&d) >= 0);
    CHECK_EQ(f.DataDropped, 1);

    /* WPA2-PSK, the host's supplicant (WiFiCx): EAPOL as data, then the keys */
    d.Wlan.Events = 0;
    seen.NRx = 0;
    CHECK_EQ(BwlConnect(&d, &b, NULL), 0);
    CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
    CHECK_EQ(d.Wlan.ConnStatus, 0);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_ASSOCIATED);
    CHECK_EQ(d.Wlan.Events & BWL_EV_KEYED, 0);
    CHECK_EQ(f.SupWpa, 0); CHECK_EQ(f.Wsec, 4); CHECK_EQ(f.WpaAuth, 0x80);
    CHECK_EQ(d.Wlan.FwSup, 0);
    {
        u32 n = BwlAssocReqBody(&d, body, sizeof(body));
        const u8 *rsn = BwlFindIe(body + 4, n - 4, 48);
        u8 ours[24];
        CHECK(rsn != NULL);
        CHECK_EQ(BwlBuildRsnIe(ours, sizeof(ours)), 22);
        CHECK(rsn != NULL && memcmp(rsn, ours, 22) == 0);       /* what message 2 must carry */
    }
    for (i = 0; i < 3 && seen.NRx == 0; i++) {
        CHECK(BwlService(&d) >= 0);
    }
    CHECK_EQ(seen.NRx, 1);                                  /* message 1 */
    CHECK_EQ(seen.Rx[0][12], 0x88); CHECK_EQ(seen.Rx[0][13], 0x8E);
    CHECK_EQ(seen.Rx[0][15], 1);
    CHECK_EQ(d.Wlan.RxEapol, 1);
    /* a key is taken as soon as the link is associated (the order is
     * Windows' business); leave, and go through the handshake properly */
    CHECK_EQ(BwlSetKey(&d, 1, 0, (const u8 *)"0123456789abcdef", 16), 0);
    CHECK_EQ(BwlDisconnect(&d, 3), 0);
    for (i = 0; i < 3; i++) CHECK(BwlService(&d) >= 0);
    d.Wlan.Events = 0;
    seen.NRx = 0;
    CHECK_EQ(BwlConnect(&d, &b, NULL), 0);
    CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_ASSOCIATED);
    for (i = 0; i < 3 && seen.NRx == 0; i++) CHECK(BwlService(&d) >= 0);
    CHECK_EQ(seen.NRx, 1);
    CHECK_EQ(send_eapol(&d, 2), 0);                         /* message 2 */
    for (i = 0; i < 3 && seen.NRx < 2; i++) CHECK(BwlService(&d) >= 0);
    CHECK_EQ(seen.NRx, 2);
    CHECK_EQ(seen.Rx[1][15], 3);                            /* message 3 */
    CHECK_EQ(send_eapol(&d, 4), 0);                         /* message 4, then the keys */
    CHECK_EQ(BwlSetKey(&d, 1, 0, (const u8 *)"0123456789abcdef", 16), 0);
    CHECK_EQ(BwlSetKey(&d, 0, 1, (const u8 *)"fedcba9876543210", 16), 0);
    CHECK_EQ(f.KeysSet, 3);                                 /* the early one and these two */
    CHECK_EQ(f.KeyErrors, 0);
    CHECK(memcmp(f.PairKey, "0123456789abcdef", 16) == 0);
    CHECK(memcmp(f.GroupKey, "fedcba9876543210", 16) == 0);
    CHECK_EQ(f.GroupIdx, 1);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_UP);
    CHECK_EQ(f.EapolFromHost, 2);
    CHECK_EQ(d.Wlan.TxEapol, 2);
    CHECK_EQ(BwlSetKey(&d, 0, 4, (const u8 *)"fedcba9876543210", 16), BWL_E_FORMAT);
    before = f.ArpReplies;
    seen.NRx = 0;
    CHECK_EQ(BwlTxEth(&d, eth, arp_request(d.Wlan.Mac, eth), 0), 0);
    for (i = 0; i < 3 && seen.NRx == 0; i++) CHECK(BwlService(&d) >= 0);
    CHECK_EQ(f.ArpReplies - before, 1);
    CHECK_EQ(seen.NRx, 1);
    /* the AP leaves */
    FakeApDeauth(&f, 7);
    CHECK_EQ(service_until(&d, BWL_EV_DISCONNECT, 5), 0);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_DOWN);
    CHECK_EQ(d.Wlan.DisconnectReason, 7);
    CHECK_EQ(d.Wlan.DisconnectEvent, E_DEAUTH_IND);
    CHECK_EQ(d.Wlan.DisconnectByPeer, 1);
    CHECK_EQ(BwlSetKey(&d, 1, 0, (const u8 *)"0123456789abcdef", 16), BWL_E_STATE);

    /* WPA2-PSK, the firmware's supplicant */
    d.Wlan.Events = 0;
    r = BwlConnect(&d, &b, FAKE_PSK_PASS);
    if (is43752) {
        /* this simulated firmware has no idsup: sup_wpa 1 is refused */
        CHECK_EQ(r, BWL_E_FIRMWARE);
        CHECK_EQ(d.Wlan.Link, BWL_LINK_DOWN);
    } else {
        CHECK_EQ(r, 0);
        CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
        CHECK_EQ(d.Wlan.ConnStatus, 0);
        CHECK_EQ(d.Wlan.Events & BWL_EV_KEYED, BWL_EV_KEYED);
        CHECK_EQ(d.Wlan.Link, BWL_LINK_UP);
        CHECK_EQ(f.SupWpa, 1);
        CHECK(strcmp(f.Pmk, FAKE_PSK_PASS) == 0);
        CHECK_EQ(BwlSetKey(&d, 1, 0, (const u8 *)"0123456789abcdef", 16), BWL_E_STATE);   /* not ours to set */
        before = f.ArpReplies;
        CHECK_EQ(BwlTxEth(&d, eth, arp_request(d.Wlan.Mac, eth), 0), 0);
        CHECK(BwlService(&d) >= 0);
        CHECK_EQ(f.ArpReplies - before, 1);
        CHECK_EQ(BwlDisconnect(&d, 3), 0);
        for (i = 0; i < 3; i++) CHECK(BwlService(&d) >= 0);
        /* a wrong passphrase: the handshake fails */
        d.Wlan.Events = 0;
        CHECK_EQ(BwlConnect(&d, &b, "not the passphrase"), 0);
        CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
        CHECK_EQ(d.Wlan.ConnStatus, 0x107);
        CHECK_EQ(d.Wlan.ConnEvent, E_PSK_SUP);
        CHECK_EQ(d.Wlan.Link, BWL_LINK_DOWN);
        for (i = 0; i < 3; i++) CHECK(BwlService(&d) >= 0);
        CHECK_EQ(d.Wlan.Events & BWL_EV_DISCONNECT, 0);
        CHECK_EQ(BwlConnect(&d, &b, "short"), BWL_E_FORMAT);
    }

    /* what cannot be joined */
    CHECK_EQ(BwlConnect(&d, &seen.Bss[wep], NULL), BWL_E_FORMAT);
    d.Wlan.Events = 0;
    b = seen.Bss[open];
    b.Bssid[5] = 0x99;                                      /* no such AP */
    CHECK_EQ(BwlConnect(&d, &b, NULL), 0);
    CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
    CHECK_EQ(d.Wlan.ConnStatus, E_STATUS_NO_NETWORKS);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_DOWN);
    /* a scan still running when a join is wanted is aborted */
    f.HoldEscan = 1;
    CHECK_EQ(BwlScanStart(&d), 0);
    CHECK(BwlService(&d) >= 0);
    CHECK_EQ(d.Wlan.Scanning, 1);
    d.Wlan.Events = 0;
    CHECK_EQ(BwlConnect(&d, &seen.Bss[open], NULL), 0);
    CHECK_EQ(f.ScanAborts, 1);
    CHECK(d.Wlan.Events & BWL_EV_SCAN_DONE);
    CHECK_EQ(d.Wlan.Scanning, 0);
    CHECK_EQ(service_until(&d, BWL_EV_ASSOC, 20), 0);
    CHECK_EQ(d.Wlan.Link, BWL_LINK_UP);

    CHECK_EQ(f.Violations, 0);
    if (f.Violations) {
        printf("  last violation: %s\n", f.LastViolation);
    }
    printf("  %u commands, %u frames out, %u in, %u events, %u scans, %u joins\n", f.DcmdSeen,
           d.Stats.FramesTx, d.Stats.FramesRx, d.Stats.EventsRx, f.Escans, f.Joins);
    FakeFree(&f);
}

static unsigned char *
slurp(const char *dir, const char *name, unsigned *len)
{
    char path[1024];
    unsigned char *b;
    long n;
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (n <= 0 || fread(b, 1, (size_t)n, fp) != (size_t)n) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    fclose(fp);
    *len = (unsigned)n;
    return b;
}

int
main(int argc, char **argv)
{
    unsigned char *fw, *nv, *clm;
    unsigned fwlen, nvlen, clmlen;

    if (argc >= 3 && strcmp(argv[1], "files") == 0) {
        const char *ver = NULL, *chip = "43456";
        unsigned chunks = 0, ramsize;
        unsigned char *conv;
        char name[64];
        int i, n, model;

        for (i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--version") == 0) ver = argv[i + 1];
            else if (strcmp(argv[i], "--clm-chunks") == 0) chunks = (unsigned)atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--chip") == 0) chip = argv[i + 1];
        }
        if (strcmp(chip, "43456") == 0) {
            model = FAKE_4345;
            ramsize = FAKE_RAMSIZE;
        } else if (strcmp(chip, "43752") == 0) {
            model = FAKE_43752;
            ramsize = FAKE43752_RAMSIZE;
        } else {
            printf("unknown --chip %s\n", chip);
            return 2;
        }
        snprintf(name, sizeof(name), "brcmfmac%s-sdio.bin", chip);
        fw = slurp(argv[2], name, &fwlen);
        snprintf(name, sizeof(name), "brcmfmac%s-sdio.txt", chip);
        nv = slurp(argv[2], name, &nvlen);
        snprintf(name, sizeof(name), "brcmfmac%s-sdio.clm_blob", chip);
        clm = slurp(argv[2], name, &clmlen);
        conv = malloc(nvlen + BWL_NVRAM_SLACK);
        n = BwlNvramConvert(nv, nvlen, conv, nvlen + BWL_NVRAM_SLACK);
        printf("firmware %u bytes, NVRAM %u bytes of text -> %d, CLM %u bytes\n", fwlen, nvlen, n, clmlen);
        CHECK(n > 0);
        CHECK_EQ(BwlFwImageCheck(fw, fwlen, ramsize), 0);
        CHECK(fwlen + (unsigned)n + 0x4000 < ramsize);
        CHECK_EQ(BwlClmCheck(clm, clmlen), 0);
        free(conv);
        bringup(model, fw, fwlen, nv, nvlen, clm, clmlen, ver, chunks);
        free(fw);
        free(nv);
        free(clm);
    } else {
        test_nvram();
        test_versions();
        test_chip_table();
        test_clm_chunks();
        test_sdpcm();
        test_bcdc_event();
        test_xfer_split();
        test_faults();
        test_wlan_pure();
        test_wlan(FAKE_4345);
        test_wlan(FAKE_43752);
        synthetic_files(&fw, &fwlen, &nv, &nvlen, &clm, &clmlen);
        printf("bring-up with synthetic files\n");
        bringup(FAKE_4345, fw, fwlen, nv, nvlen, clm, clmlen, "7.45.96.0", 6);
        free(fw);
        free(nv);
        free(clm);
        synthetic_files_trailer("<FW-TAG>4362a2-roml/bwltest Version=18.35.387.23.7 (bwltest) FWID=01-0",
                                &fw, &fwlen, &nv, &nvlen, &clm, &clmlen);
        printf("bring-up with synthetic files, simulated BCM43752\n");
        bringup(FAKE_43752, fw, fwlen, nv, nvlen, clm, clmlen, "18.35.387.23.7", 6);
        free(fw);
        free(nv);
        free(clm);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
