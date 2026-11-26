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

/*
 * The driver allocates exactly SrcLen + BWL_NVRAM_SLACK bytes for the
 * converted NVRAM (driver.c). Do the same here, on the heap, for every
 * shape of input up to 24 bytes over a small alphabet, so that ASan sees a
 * write one byte past it. The worst case is a single line with no newline
 * whose kept length + 1 is a multiple of 4 ("abc"): SrcLen + 9 bytes.
 */
static void
test_nvram_exact(void)
{
    static const unsigned char alpha[] = { 'a', '=', '\n', '#', ' ', '\r' };
    unsigned len, iter, worst = 0;
    unsigned x = 1;

    for (len = 1; len <= 24; len++) {
        for (iter = 0; iter < 400; iter++) {
            unsigned char *src = malloc(len), *dst = malloc(len + BWL_NVRAM_SLACK);
            unsigned i;
            int n;

            for (i = 0; i < len; i++) {
                x = x * 1103515245u + 12345u;
                /* mostly plain bytes, so that long kept lines come up often */
                src[i] = (unsigned char)((x >> 16) % 4 ? 'a' + (x >> 20) % 26 : alpha[(x >> 20) % sizeof(alpha)]);
            }
            if (iter == 0) {
                memset(src, 'a', len);  /* one line, every byte kept, no newline */
            }
            n = BwlNvramConvert(src, len, dst, len + BWL_NVRAM_SLACK);
            if (n > 0) {
                CHECK((unsigned)n <= len + BWL_NVRAM_SLACK);
                CHECK_EQ(n % 4, 0);
                if (n > (int)(len + worst)) worst = (unsigned)n - len;
            } else {
                CHECK_EQ(n, BWL_E_FORMAT);  /* only comments and blanks */
            }
            free(src);
            free(dst);
        }
    }
    CHECK_EQ(worst, 9);
    {
        unsigned char *dst = malloc(3 + BWL_NVRAM_SLACK);

        CHECK_EQ(BwlNvramConvert((const u8 *)"abc", 3, dst, 3 + BWL_NVRAM_SLACK), 12);
        CHECK_EQ(BwlNvramConvert((const u8 *)"abc", 3, dst, 3 + 9 - 1), BWL_E_SPACE);
        free(dst);
    }
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

/*
 * The bounce buffer on the heap and exactly BWL_BOUNCE_MIN bytes, as the
 * driver allocates it (pool.c): ASan's redzone sits right behind it.
 */
static unsigned char *bounce;

static void
dev_init(bwl_dev *d, fake_chip *f)
{
    if (bounce == NULL) {
        bounce = malloc(BWL_BOUNCE_MIN);
    }
    BwlInit(d, &FakeOps, f, bounce, BWL_BOUNCE_MIN);
}

/* Like driver.c: RecordConsole's buffer is PUT_MAX + 1 bytes, "ver" goes into 255, the MAC into 6. */
#define DRV_PUT_MAX 2048u

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

    /* the same calls with buffers of the driver's exact sizes, on the heap */
    {
        unsigned char *mac = malloc(6), *text = malloc(255);
        char *cons = malloc(DRV_PUT_MAX + 1);

        CHECK_EQ(BwlGetVar(&d, "cur_etheraddr", mac, 6, &outlen, &fws), 0);
        CHECK_EQ(mac[5], 0x38);
        CHECK_EQ(BwlGetVar(&d, "ver", text, 255, &outlen, &fws), 0);
        CHECK(BwlConsoleRead(&d, cons, DRV_PUT_MAX + 1) > 0);
        free(mac);
        free(text);
        free(cons);
    }

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

/*
 * A firmware that sends more than was asked for: every control reply
 * carries 4000 extra bytes and claims them in its length, and the console
 * ring is the largest the driver reads (32 KB). Everything lands in
 * buffers of the driver's exact sizes, on the heap, under ASan.
 */
static void
test_hostile_firmware(void)
{
    unsigned char *fw, *nv, *clm, *conv, *mac, *text;
    unsigned fwlen, nvlen, clmlen, outlen = 0, v = 0;
    char *cons;
    fake_chip f;
    bwl_dev d;
    int n, fws = 0;

    synthetic_files(&fw, &fwlen, &nv, &nvlen, &clm, &clmlen);
    conv = malloc(nvlen + BWL_NVRAM_SLACK);
    n = BwlNvramConvert(nv, nvlen, conv, nvlen + BWL_NVRAM_SLACK);
    CHECK(n > 0);
    FakeInit(&f);
    f.ConsoleBufSize = 0x8000;
    dev_init(&d, &f);
    CHECK_EQ(BwlAttach(&d), 0);
    CHECK_EQ(BwlDownload(&d, fw, fwlen, conv, (unsigned)n, 1), 0);
    CHECK_EQ(BwlStart(&d, 100), 0);
    f.ReplyExtra = 4000;

    mac = malloc(6);
    text = malloc(255);
    cons = malloc(DRV_PUT_MAX + 1);
    CHECK_EQ(BwlGetInt(&d, C_GET_VERSION, &v, &fws), 0);
    CHECK_EQ(v, 2);
    CHECK_EQ(BwlGetVar(&d, "cur_etheraddr", mac, 6, &outlen, &fws), 0);
    CHECK_EQ(outlen, 4000 + 14);        /* the reply's claim, reported; only 6 copied */
    CHECK_EQ(mac[0], 0x00);
    CHECK_EQ(BwlGetVar(&d, "ver", text, 255, &outlen, &fws), 0);
    CHECK_EQ(BwlReadShared(&d), 0);
    CHECK_EQ(BwlConsoleRead(&d, cons, DRV_PUT_MAX + 1), DRV_PUT_MAX);
    CHECK_EQ(cons[DRV_PUT_MAX], 0);
    CHECK_EQ(f.Violations, 0);
    free(mac);
    free(text);
    free(cons);
    FakeFree(&f);
    free(conv);
    free(fw);
    free(nv);
    free(clm);
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
        test_nvram_exact();
        test_versions();
        test_chip_table();
        test_clm_chunks();
        test_sdpcm();
        test_bcdc_event();
        test_xfer_split();
        test_faults();
        test_hostile_firmware();
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
    free(bounce);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
