/*
 * bwltest - host tests for bcmwlsdio's portable core
 * (drivers/net/bcmwlsdio/bwl*.c), run by CI on Linux with gcc.
 *
 *   bwltest                     unit tests, then a full bring-up against
 *                               the simulated chip with synthetic files
 *   bwltest files DIR [--version V] [--clm-chunks N]
 *                               the same bring-up with the real
 *                               brcmfmac43456-sdio.bin/.txt/.clm_blob
 *                               from DIR; checks the image's trailer
 *                               version and the CLM chunk count
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
bringup(const unsigned char *fw, unsigned fwlen, const unsigned char *nvtxt, unsigned nvtxtlen,
        const unsigned char *clm, unsigned clmlen, const char *expect_ver, unsigned expect_chunks)
{
    fake_chip f;
    bwl_dev d;
    unsigned char *nv = malloc(nvtxtlen + BWL_NVRAM_SLACK);
    unsigned char out[256];
    char ver[64], console[1024];
    unsigned chunks = 0, clmstatus = 0, outlen = 0, v = 0;
    int nvlen, fws = 0, r, before = failures;

    FakeInit(&f);
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
    CHECK_EQ(d.Chip, 0x4345);
    CHECK_EQ(d.ChipRev, 9);
    CHECK(strcmp(BwlChipFwName(d.Chip, d.ChipRev), "43456") == 0);
    CHECK_EQ(d.NumCores, 4);                            /* CC, 802.11, SDIO, CR4 */
    CHECK(BwlChipCore(&d, CORE_ARM_CR4, 0) != NULL && BwlChipCore(&d, CORE_ARM_CR4, 0)->Wrap == 0x18103000u);
    CHECK(BwlChipCore(&d, CORE_80211, 0) != NULL && BwlChipCore(&d, CORE_80211, 0)->Base == 0x18001000u);
    CHECK(BwlChipCore(&d, CORE_GCI, 0) == NULL);        /* no wrapper: dropped, as in bwfm */
    CHECK(BwlChipCore(&d, 0x8FF, 0) == NULL);
    CHECK_EQ(d.RamBase, FAKE_RAMBASE);
    CHECK_EQ(d.RamSize, FAKE_RAMSIZE);
    CHECK_EQ(d.PmuRev, 28);
    CHECK_EQ(d.SrCapable, 1);
    CHECK_EQ(f.Cccr[CCCR_IOEX], 0x02);                  /* F1 on, F2 off */
    CHECK(f.SleepCsr & SLEEPCSR_KSO);
    CHECK(f.Cccr[CCCR_BRCM_CARDCTRL] & CARDCTRL_WLANRESET);
    CHECK_EQ(f.BlkSize[1], 64);
    CHECK_EQ(f.BlkSize[2], 512);

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
    CHECK_EQ(f.Watermark, F2_WATERMARK_DEFAULT);
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
    CHECK(strcmp(ver, "7.45.96.0") == 0);
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
synthetic_files(unsigned char **fw, unsigned *fwlen, unsigned char **nv, unsigned *nvlen,
                unsigned char **clm, unsigned *clmlen)
{
    static const char trailer[] = "43455c5-roml/bwltest Version: 7.45.96.0 CRC: 0 Date: 2026";
    static const char nvram[] = "#test\nsromrev=11\nboardtype=0x6e4\nxtalfreq=37400\nmacaddr=00:90:4c:c5:12:38\n";
    unsigned i, x = 12345;

    *fwlen = 300001;            /* odd on purpose: the tail is padded */
    *fw = malloc(*fwlen);
    for (i = 0; i < *fwlen; i++) {
        x = x * 1103515245u + 12345u;
        (*fw)[i] = (unsigned char)(x >> 16);
    }
    (*fw)[0] = 0x3E; (*fw)[1] = 0xB8; (*fw)[2] = 0x98; (*fw)[3] = 0xF1;
    memcpy(*fw + *fwlen - sizeof(trailer), trailer, sizeof(trailer));
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
        const char *ver = NULL;
        unsigned chunks = 0;
        unsigned char *conv;
        int i, n;

        for (i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--version") == 0) ver = argv[i + 1];
            else if (strcmp(argv[i], "--clm-chunks") == 0) chunks = (unsigned)atoi(argv[i + 1]);
        }
        fw = slurp(argv[2], "brcmfmac43456-sdio.bin", &fwlen);
        nv = slurp(argv[2], "brcmfmac43456-sdio.txt", &nvlen);
        clm = slurp(argv[2], "brcmfmac43456-sdio.clm_blob", &clmlen);
        conv = malloc(nvlen + BWL_NVRAM_SLACK);
        n = BwlNvramConvert(nv, nvlen, conv, nvlen + BWL_NVRAM_SLACK);
        printf("firmware %u bytes, NVRAM %u bytes of text -> %d, CLM %u bytes\n", fwlen, nvlen, n, clmlen);
        CHECK(n > 0);
        CHECK_EQ(BwlFwImageCheck(fw, fwlen, FAKE_RAMSIZE), 0);
        CHECK(fwlen + (unsigned)n + 0x4000 < FAKE_RAMSIZE);
        CHECK_EQ(BwlClmCheck(clm, clmlen), 0);
        free(conv);
        bringup(fw, fwlen, nv, nvlen, clm, clmlen, ver, chunks);
        free(fw);
        free(nv);
        free(clm);
    } else {
        test_nvram();
        test_versions();
        test_clm_chunks();
        test_sdpcm();
        test_bcdc_event();
        test_xfer_split();
        test_faults();
        synthetic_files(&fw, &fwlen, &nv, &nvlen, &clm, &clmlen);
        printf("bring-up with synthetic files\n");
        bringup(fw, fwlen, nv, nvlen, clm, clmlen, "7.45.96.0", 6);
        free(fw);
        free(nv);
        free(clm);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
