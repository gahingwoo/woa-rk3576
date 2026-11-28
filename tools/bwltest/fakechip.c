/*
 * fakechip.c - simulated BCM43456 and BCM43752 for tools/bwltest; see fakechip.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakechip.h"
#include "../../drivers/net/bcmwlsdio/bwl_regs.h"

/* Backplane layout of the fake */
#define CC_BASE     0x18000000u
#define D11_BASE    0x18001000u
#define SDIO_BASE   0x18002000u
#define CR4_BASE    0x18003000u
#define GCI_BASE    0x18004000u
#define PMU_BASE    0x18005000u /* 43752 model only: PMU as a core of its own */
#define WRAP(i)     (0x18100000u + (unsigned)(i) * 0x1000u)
#define EROM_ADDR   0x1810F000u
#define NCORES      4           /* cores with a wrapper: CC, D11, SDIO, CR4 */

#define IDX_CC   0
#define IDX_D11  1
#define IDX_SDIO 2
#define IDX_CR4  3

#define TX_MAX   (64u * 1024u)
#define RX_MAX   (256u * 1024u)

static unsigned erom[64];
static unsigned erom_len;

static void
violation(fake_chip *F, const char *msg, unsigned a, unsigned b)
{
    F->Violations++;
    snprintf(F->LastViolation, sizeof(F->LastViolation), "%s (0x%x, 0x%x)", msg, a, b);
}

static unsigned
comp_a(unsigned id)
{
    return (0x4BFu << 20) | (id << 8) | DMP_DESC_COMPONENT;
}

static unsigned
comp_b(unsigned rev, unsigned nsw, unsigned nmw)
{
    return (rev << 24) | (nsw << 19) | (nmw << 14) | DMP_DESC_COMPONENT;
}

static unsigned
addr_desc(unsigned base, unsigned stype, unsigned sz)
{
    return (base & DMP_SLAVE_ADDR_BASE) | (stype << 6) | (sz << 4) | DMP_DESC_ADDRESS;
}

static void
build_erom(int model)
{
    unsigned n = 0;

    erom[n++] = comp_a(CORE_CHIPCOMMON);
    if (model == FAKE_43752) {
        /* chipcommon rev 49 with the AOB capability: the PMU is a core of its own */
        erom[n++] = comp_b(49, 1, 0);
        erom[n++] = addr_desc(CC_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);
        erom[n++] = addr_desc(WRAP(IDX_CC), DMP_SLAVE_TYPE_SWRAP, DMP_SLAVE_SIZE_4K);
        /* PMU with no wrappers: kept (bwfm and brcmfmac make an exception for
         * it), because the next component ends its descriptor list */
        erom[n++] = comp_a(CORE_PMU);
        erom[n++] = comp_b(28, 0, 0);
        erom[n++] = addr_desc(PMU_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);
    } else {
        /* chipcommon rev 31: below 35, so the PMU registers are in chipcommon */
        erom[n++] = comp_b(31, 1, 0);
        erom[n++] = addr_desc(CC_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);
        erom[n++] = addr_desc(WRAP(IDX_CC), DMP_SLAVE_TYPE_SWRAP, DMP_SLAVE_SIZE_4K);
    }

    /* 802.11 core: a 64-bit, size-described region first, which must be skipped */
    erom[n++] = comp_a(CORE_80211);
    erom[n++] = comp_b(42, 1, 0);
    erom[n++] = addr_desc(0x20000000u, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_DESC) | DMP_DESC_ADDRSIZE_GT32;
    erom[n++] = 0x00000001u;                        /* upper 32 address bits */
    erom[n++] = 0x00004000u;                        /* size descriptor, 32-bit */
    erom[n++] = addr_desc(D11_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_8K);
    erom[n++] = addr_desc(WRAP(IDX_D11), DMP_SLAVE_TYPE_SWRAP, DMP_SLAVE_SIZE_4K);

    /* a core with no wrappers: skipped */
    erom[n++] = comp_a(0x8FF);
    erom[n++] = comp_b(1, 0, 0);
    erom[n++] = addr_desc(0x18008000u, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);

    erom[n++] = comp_a(CORE_SDIO_DEV);
    erom[n++] = comp_b(21, 1, 0);
    erom[n++] = addr_desc(SDIO_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);
    erom[n++] = addr_desc(WRAP(IDX_SDIO), DMP_SLAVE_TYPE_SWRAP, DMP_SLAVE_SIZE_4K);

    /* ARM CR4 with a master wrapper, as on the real chips */
    erom[n++] = comp_a(CORE_ARM_CR4);
    erom[n++] = comp_b(7, 0, 1);
    erom[n++] = (0x01u << 8) | (0u << 4) | DMP_DESC_MASTER_PORT;
    erom[n++] = addr_desc(CR4_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);
    erom[n++] = addr_desc(WRAP(IDX_CR4), DMP_SLAVE_TYPE_MWRAP, DMP_SLAVE_SIZE_4K);

    /* GCI with no wrappers: scanned (bwfm makes an exception for GCI) but dropped,
     * because the scan never finds a wrapper address for it */
    erom[n++] = comp_a(CORE_GCI);
    erom[n++] = comp_b(7, 0, 0);
    erom[n++] = addr_desc(GCI_BASE, DMP_SLAVE_TYPE_SLAVE, DMP_SLAVE_SIZE_4K);

    erom[n++] = DMP_DESC_EOT;
    erom_len = n;
}

void
FakeInitModel(fake_chip *F, int Model)
{
    memset(F, 0, sizeof(*F));
    F->Model = Model;
    if (Model == FAKE_43752) {
        F->ChipIdReg = FAKE43752_CHIPID;
        F->RamBase = FAKE43752_RAMBASE;
        F->RamSize = FAKE43752_RAMSIZE;
    } else {
        F->ChipIdReg = FAKE_CHIPID;
        F->RamBase = FAKE_RAMBASE;
        F->RamSize = FAKE_RAMSIZE;
    }
    build_erom(Model);
    F->Ram = calloc(1, F->RamSize);
    F->Tx = calloc(1, TX_MAX);
    F->Rx = calloc(1, RX_MAX);
    F->Clm = calloc(1, 64 * 1024);
    F->Cccr[CCCR_REV] = 0x43;
    F->Cccr[CCCR_SDREV] = 0x03;
    F->Cccr[CCCR_CAPS] = 0x37;
    F->Cccr[CCCR_IORX] = 0;
    F->FlipRamAt = ~0u;
    F->ExpectSeq = 0xFF;
    F->BlkSize[1] = 64;
    F->BlkSize[2] = 512;
    F->Assoc = -1;
    for (unsigned i = 0; i < NCORES; i++) {
        F->ResetCtl[i] = RESET_CTL_RESET;
    }
}

void
FakeInit(fake_chip *F)
{
    FakeInitModel(F, FAKE_4345);
}

void
FakeFree(fake_chip *F)
{
    free(F->Ram);
    free(F->Tx);
    free(F->Rx);
    free(F->Clm);
}

static int
wrap_index(unsigned addr)
{
    for (int i = 0; i < NCORES; i++) {
        if ((addr & ~0xFFFu) == WRAP(i)) {
            return i;
        }
    }
    return -1;
}

static void put_le32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static unsigned get_le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static void put_le16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static unsigned get_le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put_be16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static unsigned get_be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put_be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Append one SDPCM frame to the to-host stream and raise FRAME_IND. */
static void
queue_frame(fake_chip *F, unsigned chan, const unsigned char *payload, unsigned len)
{
    unsigned flen = 12 + len, padded = (flen + 3) & ~3u;
    unsigned char *p;

    if (F->RxLen + padded > RX_MAX) {
        violation(F, "fake RX stream overflow", F->RxLen, padded);
        return;
    }
    p = F->Rx + F->RxLen;
    memset(p, 0, padded);
    put_le16(p, flen);
    put_le16(p + 2, (~flen) & 0xFFFF);
    p[4] = 0;
    p[5] = chan;
    p[6] = 0;
    p[7] = 12;
    p[8] = 0;
    p[9] = (unsigned char)(F->ExpectSeq + 4);  /* window of 4 frames */
    F->AdvMaxSeq = p[9];
    memcpy(p + 12, payload, len);
    F->RxLen += padded;
    F->IntStatus |= INTSTATUS_HMB_FRAME_IND;
}

/* A firmware event, if the host's event mask (once it set one) lets it through. */
static void
queue_event_full(fake_chip *F, unsigned type, unsigned status, unsigned reason, unsigned flags,
                 const unsigned char *addr, const unsigned char *data, unsigned dlen)
{
    static unsigned char e[4 + 14 + 10 + 48 + 4096];
    unsigned char *eth = e + 4, *h = eth + 14, *m = h + 10;

    if (F->EventMaskSet && (type / 8 >= sizeof(F->EventMask) || !(F->EventMask[type / 8] & (1u << (type % 8))))) {
        return;
    }
    if (dlen > 4096) {
        violation(F, "fake event too long", type, dlen);
        return;
    }
    memset(e, 0, 4 + 14 + 10 + 48);
    e[0] = BCDC_FLAG_VER(BCDC_PROTO_VER);
    memset(eth, 0xFF, 6);
    put_be16(eth + 12, ETHERTYPE_BRCM);
    put_be16(h + 0, 0x8001);
    put_be16(h + 2, 10 + 48 - 4 + dlen);
    h[4] = 1;
    h[5] = 0x00; h[6] = 0x10; h[7] = 0x18;
    put_be16(h + 8, BRCM_SUBTYPE_EVENT);
    put_be16(m + 0, 2);
    put_be16(m + 2, flags);
    put_be32(m + 4, type);
    put_be32(m + 8, status);
    put_be32(m + 12, reason);
    put_be32(m + 20, dlen);
    if (addr) memcpy(m + 24, addr, 6);
    memcpy(m + 30, "wlan0", 5);
    if (dlen) memcpy(m + 48, data, dlen);
    queue_frame(F, SDPCM_CH_EVENT, e, 4 + 14 + 10 + 48 + dlen);
    F->EventsQueued++;
}

static void
queue_event(fake_chip *F, unsigned type, unsigned status)
{
    queue_event_full(F, type, status, 0, 0, NULL, NULL, 0);
}

/* The CPU was released: is RAM what a real chip would boot? */
static void
boot(fake_chip *F)
{
    unsigned top = F->RamSize, shared, cons, logbuf;
    static const char log4345[] = "\nfake RTE (bwltest) 7.45.96.0 on BCM4345 r9\nwl0: up\n";
    static const char log43752[] = "\nfake RTE (bwltest) 18.35.387.23.7 on BCM43752 r2\nwl0: up\n";
    const char *log = F->Model == FAKE_43752 ? log43752 : log4345;
    unsigned loglen = (unsigned)strlen(log);

    if (F->ExpectFw && (F->ResetVector != get_le32(F->ExpectFw))) {
        F->BootError = 1;
        return;
    }
    if (F->ExpectFw && memcmp(F->Ram, F->ExpectFw, F->ExpectFwLen) != 0) {
        F->BootError = 2;
        return;
    }
    if (F->ExpectNvram && memcmp(F->Ram + top - F->ExpectNvramLen, F->ExpectNvram, F->ExpectNvramLen) != 0) {
        F->BootError = 3;
        return;
    }
    if (F->ExpectNvram) {
        unsigned tok = get_le32(F->Ram + top - 4);
        if ((tok & 0xFFFF) * 4 != F->ExpectNvramLen - 4 || ((~tok >> 16) & 0xFFFF) != (tok & 0xFFFF)) {
            F->BootError = 4;
            return;
        }
    }
    /* firmware data structures below the NVRAM, as the real one puts them */
    shared = top - 0x4000;
    cons = shared + 0x100;
    logbuf = cons + 0x100;
    memset(F->Ram + shared, 0, 0x400);
    put_le32(F->Ram + shared + SHARED_FLAGS, 3);
    put_le32(F->Ram + shared + SHARED_CONSOLE_ADDR, F->RamBase + cons);
    if (F->ConsoleBufSize > 0x100) {
        /* a big ring, below the shared block, full of text */
        unsigned i;

        logbuf = shared - F->ConsoleBufSize - 0x100;
        for (i = 0; i < F->ConsoleBufSize; i++) {
            F->Ram[logbuf + i] = (unsigned char)(i % 64 == 63 ? '\n' : 'a' + i % 26);
        }
    }
    put_le32(F->Ram + cons + CONSOLE_LOG_BUF, F->RamBase + logbuf);
    put_le32(F->Ram + cons + CONSOLE_LOG_BUFSZ, F->ConsoleBufSize > 0x100 ? F->ConsoleBufSize : 0x100);
    memcpy(F->Ram + logbuf, log, loglen);
    put_le32(F->Ram + cons + CONSOLE_LOG_IDX, loglen);
    put_le32(F->Ram + top - 4, F->RamBase + shared);
    F->Running = 1;
    F->ToHostMbData = TOHOST_FWREADY;
    F->IntStatus |= INTSTATUS_HMB_HOST_INT;
    queue_event(F, E_IF, 0);
}

static int
cr4_running(const fake_chip *F)
{
    return F->ResetCtl[IDX_CR4] == 0 && !(F->Ioctl[IDX_CR4] & IOCTL_ARMCR4_CPUHALT);
}

/* The PMU registers: in chipcommon on the 4345, a core of its own on the 43752. */
static unsigned
pmu_base(const fake_chip *F)
{
    return F->Model == FAKE_43752 ? PMU_BASE : CC_BASE;
}

static unsigned
bp_read32(fake_chip *F, unsigned a)
{
    int w = wrap_index(a);
    unsigned pmu = pmu_base(F);

    if (a >= F->RamBase && a + 4 <= F->RamBase + F->RamSize) {
        return get_le32(F->Ram + (a - F->RamBase));
    }
    if (a == pmu + CC_PMUCONTROL) return F->PmuControl;
    if (a == pmu + CC_PMUCAPS) return 0x1C;     /* PMU rev 28 */
    if (F->Model == FAKE_4345) {
        /* brcmfmac's 4345 save/restore check: chip control 3, bit 2 */
        if (a == pmu + CC_CHIPCTL_ADDR) return F->ChipCtlAddr;
        if (a == pmu + CC_CHIPCTL_DATA) return F->ChipCtlAddr == 3 ? (1u << 2) : 0;
    } else {
        /* brcmfmac's 43752 check: the retention control only; logic and MAC/PHY kept */
        if (a == pmu + CC_RETENTION_CTL) return 0;
    }
    if (w >= 0) {
        if ((a & 0xFFF) == AGENT_IOCTL) return F->Ioctl[w];
        if ((a & 0xFFF) == AGENT_RESET_CTL) return F->ResetCtl[w];
        return 0;
    }
    if (a >= EROM_ADDR && a < EROM_ADDR + 4 * 64) {
        unsigned i = (a - EROM_ADDR) / 4;
        return i < erom_len ? erom[i] : 0;
    }
    switch (a) {
    case CC_BASE + CC_CHIPID:       return F->ChipIdReg;
    case CC_BASE + CC_CAPS:         return CC_CAPS_PMU | 0x1u;
    case CC_BASE + CC_CAPS_EXT:     return F->Model == FAKE_43752 ? CC_CAPS_EXT_AOB : 0;
    case CC_BASE + CC_EROMPTR:      return EROM_ADDR;
    case CR4_BASE + ARMCR4_CAP:     return 0x02;    /* 2 A banks, 0 B banks */
    case CR4_BASE + ARMCR4_BANKIDX: return F->BankIdx;
    case CR4_BASE + ARMCR4_BANKINFO:                /* 50 or 64 x 8 KB */
        return F->BankIdx < 2 ? (F->Model == FAKE_43752 ? 63u : 49u) : 0u;
    case SDIO_BASE + SDCORE_INTSTATUS:      return F->IntStatus;
    case SDIO_BASE + SDCORE_HOSTINTMASK:    return F->HostIntMask;
    case SDIO_BASE + SDCORE_TOSBMAILBOXDATA:return F->ToSbMbData;
    case SDIO_BASE + SDCORE_TOHOSTMAILBOXDATA: return F->ToHostMbData;
    case 0:                         return F->ResetVector;
    }
    violation(F, "read of an unmodelled backplane address", a, 0);
    return 0;
}

static void
bp_write32(fake_chip *F, unsigned a, unsigned v)
{
    int w = wrap_index(a);
    unsigned pmu = pmu_base(F);

    if (a >= F->RamBase && a + 4 <= F->RamBase + F->RamSize) {
        if (cr4_running(F) && a < F->RamBase + F->ExpectFwLen) {
            violation(F, "firmware image overwritten while running", a, v);
        }
        put_le32(F->Ram + (a - F->RamBase), v);
        if (F->FlipRamAt != ~0u && a - F->RamBase <= F->FlipRamAt && F->FlipRamAt < a - F->RamBase + 4) {
            F->Ram[F->FlipRamAt] ^= 0x5A;
        }
        return;
    }
    if (a == pmu + CC_PMUCONTROL) { F->PmuControl = v; return; }
    if (F->Model == FAKE_4345 && a == pmu + CC_CHIPCTL_ADDR) { F->ChipCtlAddr = v; return; }
    if (w >= 0) {
        int was = cr4_running(F);
        if ((a & 0xFFF) == AGENT_IOCTL) F->Ioctl[w] = v;
        else if ((a & 0xFFF) == AGENT_RESET_CTL) F->ResetCtl[w] = v & RESET_CTL_RESET;
        if (w == IDX_CR4) {
            if (!was && cr4_running(F)) boot(F);
            if (was && !cr4_running(F)) F->Running = 0;
        }
        return;
    }
    switch (a) {
    case CR4_BASE + ARMCR4_BANKIDX: F->BankIdx = v; return;
    case SDIO_BASE + SDCORE_INTSTATUS:      F->IntStatus &= ~v; return;
    case SDIO_BASE + SDCORE_HOSTINTMASK:    F->HostIntMask = v; return;
    case SDIO_BASE + SDCORE_TOSBMAILBOXDATA:F->ToSbMbData = v; return;
    case SDIO_BASE + SDCORE_TOSBMAILBOX:
        if (v & TOSBMAILBOX_INT_ACK) F->ToHostMbData = 0;
        return;
    case 0:
        if (cr4_running(F)) violation(F, "reset vector written while running", a, v);
        F->ResetVector = v;
        return;
    }
    violation(F, "write of an unmodelled backplane address", a, v);
}

static void
update_iorx(fake_chip *F)
{
    unsigned char rx = 0, ex = F->Cccr[CCCR_IOEX];

    if (ex & 0x02) rx |= 0x02;
    if ((ex & 0x04) && F->Running && F->ToSbMbData == (SDPCM_PROT_VERSION << SDPCM_PROT_VERSION_SHIFT)) {
        rx |= 0x04;
    }
    F->Cccr[CCCR_IORX] = rx;
}

static unsigned char
clkcsr_read(const fake_chip *F)
{
    unsigned char v = F->ClkCsr & 0x3F;

    if (F->ClkCsr & (CLKCSR_ALP_AVAIL_REQ | CLKCSR_FORCE_ALP | CLKCSR_HT_AVAIL_REQ | CLKCSR_FORCE_HT)) {
        v |= CLKCSR_ALP_AVAIL;
    }
    if (F->ClkCsr & (CLKCSR_HT_AVAIL_REQ | CLKCSR_FORCE_HT)) {
        v |= CLKCSR_HT_AVAIL;
    }
    return v;
}

static int
fake_cmd52(void *Ctx, unsigned Fn, unsigned Addr, int Write, unsigned char *Val)
{
    fake_chip *F = Ctx;

    F->Cmd52++;
    if (Addr < 0x800) {
        if (Fn != 0) { violation(F, "CCCR/FBR access not on F0", Addr, Fn); return -1; }
        if (Addr >= 0x100 && Addr < 0x300) {
            unsigned fn = Addr >> 8, r = Addr & 0xFF;
            if (r == FBR_BLKSIZE0 || r == FBR_BLKSIZE1) {
                unsigned char *b = (unsigned char *)&F->BlkSize[fn];
                if (Write) b[r - FBR_BLKSIZE0] = *Val; else *Val = b[r - FBR_BLKSIZE0];
                return 0;
            }
            if (!Write) *Val = 0;
            return 0;
        }
        if (Addr >= 0x100) { if (!Write) *Val = 0; return 0; }
        if (Write) {
            if (Addr == CCCR_IORX || Addr == CCCR_REV) { violation(F, "write to a read-only CCCR byte", Addr, *Val); return 0; }
            F->Cccr[Addr] = *Val;
            if (Addr == CCCR_IOEX) update_iorx(F);
            if (Addr == CCCR_ABORT) F->RxPos = F->RxLen;  /* drop what is pending */
        } else {
            if (Addr == CCCR_IORX) update_iorx(F);
            *Val = F->Cccr[Addr];
        }
        return 0;
    }
    if (Addr < 0x10000 || Addr > 0x1001F || Fn != 1) {
        violation(F, "CMD52 outside F0 and the F1 register block", Addr, Fn);
        return -1;
    }
    if (!(F->Cccr[CCCR_IOEX] & 0x02)) {
        violation(F, "F1 register access with F1 disabled", Addr, 0);
        return -1;
    }
    switch (Addr) {
    case F1_SBADDRLOW:  if (Write) F->Window = (F->Window & 0xFFFF0000u) | ((*Val & 0x80u) << 8); else *Val = (F->Window >> 8) & 0x80; return 0;
    case F1_SBADDRMID:  if (Write) F->Window = (F->Window & 0xFF00FFFFu) | ((unsigned)*Val << 16); else *Val = F->Window >> 16; return 0;
    case F1_SBADDRHIGH: if (Write) F->Window = (F->Window & 0x00FFFFFFu) | ((unsigned)*Val << 24); else *Val = F->Window >> 24; return 0;
    case F1_CHIPCLKCSR: if (Write) F->ClkCsr = *Val; else *Val = clkcsr_read(F); return 0;
    case F1_SLEEPCSR:   if (Write) F->SleepCsr = *Val; else *Val = F->SleepCsr | SLEEPCSR_DEVON; return 0;
    case F1_WATERMARK:  if (Write) F->Watermark = *Val; else *Val = F->Watermark; return 0;
    case F1_DEVICE_CTL: if (Write) F->DeviceCtl = *Val; else *Val = F->DeviceCtl; return 0;
    case F1_MESBUSYCTRL: if (Write) F->MesBusyCtrl = *Val; else *Val = F->MesBusyCtrl; return 0;
    case F1_WAKEUPCTRL: if (Write) F->WakeupCtrl = *Val; else *Val = F->WakeupCtrl; return 0;
    case F1_FRAMECTRL:  if (Write) { F->FrameCtrl = *Val; if (*Val & FRAMECTRL_RF_TERM) F->RxPos = F->RxLen; } else *Val = 0; return 0;
    case F1_RFRAMEBCLO: case F1_RFRAMEBCHI: if (!Write) *Val = 0; return 0;
    default:
        if (!Write) *Val = 0;
        return 0;
    }
}

/* ------------------------------------------------------------ the air */

const fake_ap FakeAps[FAKE_NAPS] = {
    { "bwltest-open", { 0x02, 0x11, 0x22, 0x33, 0x44, 0x01 }, 6, 0, 0x1006, 0, -48 },
    /* 80 MHz around 42, control channel 36 (lower-lower): only the chanspec says so */
    { "bwltest-psk", { 0x02, 0x11, 0x22, 0x33, 0x44, 0x02 }, 36, 1, 0xE02A, 1, -61 },
    { "bwltest-wep", { 0x02, 0x11, 0x22, 0x33, 0x44, 0x03 }, 11, 0, 0x100B, 2, -70 },
};
const unsigned char FakeGatewayMac[6] = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01 };
const unsigned char FakeGatewayIp[4] = { 192, 168, 7, 1 };
const unsigned char FakeStaIp[4] = { 192, 168, 7, 50 };
static const unsigned char sta_mac[6] = { 0x00, 0x90, 0x4c, 0xc5, 0x12, 0x38 };
static const unsigned char rsn_psk[22] = {
    48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0
};

static unsigned
ap_ies(const fake_ap *ap, unsigned char *p, int assoc_req, const fake_chip *F)
{
    static const unsigned char rates[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    unsigned n = 0, l = (unsigned)strlen(ap->Ssid);

    p[n++] = 0; p[n++] = (unsigned char)l; memcpy(p + n, ap->Ssid, l); n += l;
    p[n++] = 1; p[n++] = sizeof(rates); memcpy(p + n, rates, sizeof(rates)); n += sizeof(rates);
    if (!assoc_req) {
        p[n++] = 3; p[n++] = 1; p[n++] = (unsigned char)ap->Channel;
    }
    if (ap->Sec == 1) {
        if (assoc_req) {
            memcpy(p + n, F->WpaIe, F->WpaIeLen);
            n += F->WpaIeLen;
        } else {
            memcpy(p + n, rsn_psk, sizeof(rsn_psk));
            n += sizeof(rsn_psk);
        }
    }
    return n;
}

static unsigned
ap_cap(const fake_ap *ap)
{
    return 0x0001 | 0x0400 | (ap->Sec ? 0x0010 : 0);
}

/* escan: one partial result per AP, then the end */
static void
escan(fake_chip *F, unsigned sync)
{
    unsigned char d[12 + 128 + 256];
    unsigned i;

    F->Escans++;
    for (i = 0; i < FAKE_NAPS; i++) {
        const fake_ap *ap = &FakeAps[i];
        unsigned char *bi = d + 12;
        unsigned ielen, l = (unsigned)strlen(ap->Ssid);

        memset(d, 0, sizeof(d));
        put_le16(d + 8, sync);
        put_le16(d + 10, 1);
        ielen = ap_ies(ap, bi + 128, 0, F);
        put_le32(bi + 0, 109);
        put_le32(bi + 4, 128 + ielen);
        memcpy(bi + 8, ap->Bssid, 6);
        put_le16(bi + 14, 100);
        put_le16(bi + 16, ap_cap(ap));
        bi[18] = (unsigned char)l;
        memcpy(bi + 19, ap->Ssid, l);
        put_le32(bi + 52, 8);
        put_le16(bi + 72, ap->Chanspec);
        put_le16(bi + 78, (unsigned)(ap->Rssi & 0xFFFF));
        bi[88] = ap->Chanspec == 0xE02A ? 0 : (unsigned char)ap->Channel;
        bi[96] = i == FAKE_AP_OPEN ? 0x01 : 0;     /* the open AP's record is from a beacon */
        put_le16(bi + 116, 128);
        put_le32(bi + 120, ielen);
        put_le32(d + 0, 12 + 128 + ielen);
        queue_event_full(F, E_ESCAN_RESULT, E_STATUS_PARTIAL, 0, 0, NULL, d, 12 + 128 + ielen);
    }
    memset(d, 0, 12);
    put_le32(d, 12);
    put_le16(d + 8, sync);
    queue_event_full(F, E_ESCAN_RESULT, E_STATUS_SUCCESS, 0, 0, NULL, d, 12);
}

/* A data frame from the AP side (ap is the BSSID, src the sender behind it). */
static void
queue_data(fake_chip *F, const unsigned char *src, unsigned type, const unsigned char *body, unsigned blen)
{
    unsigned char f[4 + 14 + 1600];

    memset(f, 0, 4 + 14);
    f[0] = BCDC_FLAG_VER(BCDC_PROTO_VER);
    memcpy(f + 4, sta_mac, 6);
    memcpy(f + 10, src, 6);
    put_be16(f + 16, type);
    memcpy(f + 18, body, blen);
    queue_frame(F, SDPCM_CH_DATA, f, 4 + 14 + blen);
}

/* EAPOL-Key "messages" of the simulated handshake: the bytes are not real
 * 802.1X, only distinguishable (byte 1 is the message number). */
static void
queue_eapol(fake_chip *F, unsigned msg)
{
    unsigned char b[99];

    memset(b, 0, sizeof(b));
    b[0] = 2;                   /* 802.1X version */
    b[1] = (unsigned char)msg;  /* here: the message number */
    b[3] = sizeof(b) - 4;
    queue_data(F, FakeAps[F->Assoc].Bssid, ETHERTYPE_EAPOL, b, sizeof(b));
}

static void
leave(fake_chip *F, unsigned type, unsigned reason)
{
    const unsigned char *b = FakeAps[F->Assoc].Bssid;

    queue_event_full(F, type, 0, reason, 0, b, NULL, 0);
    queue_event_full(F, E_LINK, 0, reason, 0, b, NULL, 0);
    F->Assoc = -1;
    F->Authorized = 0;
    F->Eapol3Sent = 0;
}

void
FakeApDeauth(fake_chip *F, unsigned Reason)
{
    if (F->Assoc >= 0) {
        leave(F, E_DEAUTH_IND, Reason);
    }
}

static void
join(fake_chip *F, const unsigned char *d, unsigned dlen)
{
    unsigned sl = get_le32(d), n, cs, i;
    const fake_ap *ap = NULL;
    int ok;

    F->Joins++;
    if (dlen < 52 || sl == 0 || sl > 32) {
        violation(F, "bad join parameters", dlen, sl);
        return;
    }
    n = get_le32(d + 44);
    cs = get_le16(d + 48);
    for (i = 0; i < FAKE_NAPS; i++) {
        if (memcmp(FakeAps[i].Bssid, d + 36, 6) == 0) {
            ap = &FakeAps[i];
            break;
        }
    }
    if (ap == NULL || strlen(ap->Ssid) != sl || memcmp(ap->Ssid, d + 4, sl) != 0) {
        queue_event_full(F, E_SET_SSID, E_STATUS_NO_NETWORKS, 0, 0, NULL, NULL, 0);
        return;
    }
    /* the chanspec the host asks for: the 20 MHz control channel, d11ac encoding */
    if (n != 1 || (cs & 0xFF) != ap->Channel || (cs & 0x3800) != 0x1000 ||
        ((cs & 0xC000) == 0xC000) != (ap->Band == 1)) {
        violation(F, "join chanspec", cs, ap->Channel);
    }
    if (!F->Up || F->Infra != 1 || F->Auth != 0) {
        violation(F, "join before C_UP, infra 1 and open authentication", F->Up, F->Infra);
    }
    ok = ap->Sec == 0 ? (F->Wsec == 0 && F->WpaAuth == 0)
       : ap->Sec == 1 ? ((F->Wsec & 4) && F->WpaAuth == WPA2_AUTH_PSK &&
                         F->WpaIeLen == sizeof(rsn_psk) && memcmp(F->WpaIe, rsn_psk, sizeof(rsn_psk)) == 0)
       : 0;
    if (!ok) {
        queue_event_full(F, E_SET_SSID, E_STATUS_FAIL, 0, 0, ap->Bssid, NULL, 0);
        return;
    }
    F->Assoc = (int)(ap - FakeAps);
    F->Authorized = ap->Sec == 0;
    F->Eapol3Sent = 0;
    queue_event_full(F, E_AUTH, 0, 0, 0, ap->Bssid, NULL, 0);
    queue_event_full(F, E_ASSOC, 0, 0, 0, ap->Bssid, NULL, 0);
    queue_event_full(F, E_LINK, 0, 0, EVENT_FLAG_LINK, ap->Bssid, NULL, 0);
    queue_event_full(F, E_SET_SSID, E_STATUS_SUCCESS, 0, 0, ap->Bssid, NULL, 0);
    if (ap->Sec == 1 && F->SupWpa) {
        if (F->PmkLen == strlen(FAKE_PSK_PASS) && memcmp(F->Pmk, FAKE_PSK_PASS, F->PmkLen) == 0) {
            F->Authorized = 1;
            queue_event_full(F, E_PSK_SUP, E_STATUS_FWSUP_DONE, 0, 0, ap->Bssid, NULL, 0);
        } else {
            queue_event_full(F, E_PSK_SUP, 7, 0, 0, ap->Bssid, NULL, 0);    /* timeout: wrong key */
            leave(F, E_DEAUTH_IND, 15);
        }
    } else if (ap->Sec == 1) {
        queue_eapol(F, 1);          /* message 1, for the host's supplicant */
    }
}

static void
set_key(fake_chip *F, const unsigned char *k, unsigned len)
{
    static const unsigned char zero[6];
    unsigned idx, klen, algo, flags;

    if (len < WSEC_KEY_LEN) {
        violation(F, "wsec_key too short", len, WSEC_KEY_LEN);
        F->KeyErrors++;
        return;
    }
    idx = get_le32(k);
    klen = get_le32(k + 4);
    algo = get_le32(k + WSEC_KEY_ALGO);
    flags = get_le32(k + WSEC_KEY_FLAGS);
    if (F->Assoc < 0 || algo != CRYPTO_ALGO_AES_CCM || klen != 16) {
        F->KeyErrors++;
        return;
    }
    if (memcmp(k + WSEC_KEY_EA, zero, 6) != 0) {
        if (memcmp(k + WSEC_KEY_EA, FakeAps[F->Assoc].Bssid, 6) != 0 || idx != 0 || flags != 0) {
            F->KeyErrors++;
            return;
        }
        memcpy(F->PairKey, k + WSEC_KEY_DATA, 16);
        F->Authorized = 1;
    } else {
        if (flags != WSEC_PRIMARY_KEY || idx == 0 || idx > 3) {
            F->KeyErrors++;
            return;
        }
        memcpy(F->GroupKey, k + WSEC_KEY_DATA, 16);
        F->GroupIdx = idx;
    }
    F->KeysSet++;
}

/* A data frame from the host (BCDC header, then Ethernet). */
static void
host_data(fake_chip *F, const unsigned char *d, unsigned dlen)
{
    const unsigned char *eth;
    unsigned elen, type;

    if (dlen < 4 + 14 || d[0] != BCDC_FLAG_VER(BCDC_PROTO_VER) || d[3] != 0) {
        violation(F, "bad BCDC data header", dlen, d[0]);
        return;
    }
    eth = d + 4;
    elen = dlen - 4;
    type = get_be16(eth + 12);
    F->DataFromHost++;
    if (memcmp(eth + 6, sta_mac, 6) != 0) {
        violation(F, "data frame not from the station's address", eth[6], eth[11]);
    }
    if (F->Assoc < 0) {
        F->DataDropped++;
        return;
    }
    if (type == ETHERTYPE_EAPOL) {
        F->EapolFromHost++;
        F->LastEapolLen = elen - 14 < sizeof(F->LastEapol) ? elen - 14 : sizeof(F->LastEapol);
        memcpy(F->LastEapol, eth + 14, F->LastEapolLen);
        if (memcmp(eth, FakeAps[F->Assoc].Bssid, 6) != 0) {
            violation(F, "EAPOL not to the AP", eth[0], eth[5]);
        }
        if (F->Authorized) {
            /* message 4 behind the pairwise key would go out encrypted */
            violation(F, "EAPOL after the pairwise key", eth[15], 0);
        } else if (eth[15] == 2 && !F->Eapol3Sent) {
            F->Eapol3Sent = 1;
            queue_eapol(F, 3);
        }
        return;
    }
    if (!F->Authorized) {
        F->DataDropped++;           /* the port is closed until the pairwise key */
        return;
    }
    /* ARP request for the gateway: answer it */
    if (type == 0x0806 && elen >= 14 + 28 && get_be16(eth + 20) == 1 && memcmp(eth + 38, FakeGatewayIp, 4) == 0) {
        unsigned char r[28];

        memcpy(r, eth + 14, 8);
        put_be16(r + 6, 2);
        memcpy(r + 8, FakeGatewayMac, 6);
        memcpy(r + 14, FakeGatewayIp, 4);
        memcpy(r + 18, eth + 22, 10);   /* the asker's MAC and IP */
        queue_data(F, FakeGatewayMac, 0x0806, r, sizeof(r));
        F->ArpReplies++;
    }
}

static int
get_var(fake_chip *F, const char *name, unsigned char *out, unsigned dlen, int *status)
{
    const fake_ap *ap = F->Assoc >= 0 ? &FakeAps[F->Assoc] : NULL;

    if (strcmp(name, "ver") == 0) {
        static const char v4345[] = "wl0: Nov 24 2026 12:00:00 version 7.45.96.0 (bwltest) FWID 01-12345678\n";
        static const char v43752[] = "wl0: Nov 24 2026 12:00:00 version 18.35.387.23.7 (bwltest) FWID 01-12345678\n";
        const char *v = F->Model == FAKE_43752 ? v43752 : v4345;
        unsigned vl = (unsigned)strlen(v) + 1;
        memset(out, 0, dlen);
        memcpy(out, v, vl < dlen ? vl : dlen);
    } else if (strcmp(name, "cur_etheraddr") == 0) {
        memcpy(out, sta_mac, 6);
    } else if (strcmp(name, "clmload_status") == 0) {
        int ok = !F->ClmBad && F->ExpectClm && F->ClmLen == F->ExpectClmLen &&
                 memcmp(F->Clm, F->ExpectClm, F->ClmLen) == 0;
        put_le32(out, ok ? 0 : 1);
    } else if (strcmp(name, "cap") == 0) {
        static const char cap4345[] = "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu ampdu_tx ampdu_rx "
                                      "amsdurx radio_pwrsave p2p proptxstatus mchan wds dwds p2po anqpo "
                                      "vht-prop-rates mfp idsup idauth";
        static const char cap43752[] = "ap sta wme 802.11d 802.11h dualband ampdu amsdurx p2p mfp sae";
        const char *c = F->Model == FAKE_43752 ? cap43752 : cap4345;
        unsigned cl = (unsigned)strlen(c) + 1;
        memset(out, 0, dlen);
        memcpy(out, c, cl < dlen ? cl : dlen);
    } else if (strcmp(name, "event_msgs") == 0) {
        memset(out, 0, dlen);
        memcpy(out, F->EventMask, dlen < sizeof(F->EventMask) ? dlen : sizeof(F->EventMask));
    } else if (strcmp(name, "assoc_info") == 0 && ap != NULL && dlen >= 28) {
        unsigned char tmp[300];
        memset(out, 0, dlen);
        put_le32(out + 0, ap_ies(ap, tmp, 1, F));
        put_le32(out + 4, 10);
        put_le16(out + 12, ap_cap(ap));
        put_le16(out + 14, 10);
        memcpy(out + 16, ap->Bssid, 6);
        put_le16(out + 22, ap_cap(ap));
        put_le16(out + 26, 0xC005);
    } else if (strcmp(name, "assoc_req_ies") == 0 && ap != NULL) {
        unsigned char tmp[300];
        unsigned n = ap_ies(ap, tmp, 1, F);
        memset(out, 0, dlen);
        memcpy(out, tmp, n < dlen ? n : dlen);
    } else if (strcmp(name, "assoc_resp_ies") == 0 && ap != NULL) {
        static const unsigned char rsp[10] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
        memset(out, 0, dlen);
        memcpy(out, rsp, dlen < 10 ? dlen : 10);
    } else {
        *status = -23;      /* BCME_UNSUPPORTED */
        return 1;
    }
    return 0;
}

static int
set_var(fake_chip *F, const char *name, const unsigned char *v, unsigned vlen, int *status)
{
    unsigned iv = vlen >= 4 ? get_le32(v) : 0;

    if (strcmp(name, "clmload") == 0) {
        const unsigned char *c = v;
        unsigned fl = get_le16(c), ty = get_le16(c + 2), n = get_le32(c + 4);
        if (!(fl & DLOAD_FLAG_HANDLER_VER) || ty != DLOAD_TYPE_CLM || n > BWL_DLOAD_MAX ||
            12 + n > vlen || F->ClmLen + n > 64 * 1024) {
            F->ClmBad = 1;
        } else {
            if (((fl & DLOAD_FLAG_BEGIN) != 0) != (F->ClmLen == 0)) F->ClmBad = 1;
            memcpy(F->Clm + F->ClmLen, c + 12, n);
            F->ClmLen += n;
            if ((fl & DLOAD_FLAG_END) && F->ClmLen != F->ExpectClmLen) F->ClmBad = 1;
        }
    } else if (strcmp(name, "event_msgs") == 0) {
        if (vlen < sizeof(F->EventMask)) {
            *status = -14;  /* BCME_BUFTOOSHORT */
            return 1;
        }
        memcpy(F->EventMask, v, sizeof(F->EventMask));
        F->EventMaskSet = 1;
    } else if (strcmp(name, "mpc") == 0) {
        F->Mpc = iv;
    } else if (strcmp(name, "roam_off") == 0) {
        F->RoamOff = iv;
    } else if (strcmp(name, "wsec") == 0) {
        F->Wsec = iv;
    } else if (strcmp(name, "wpa_auth") == 0) {
        F->WpaAuth = iv;
    } else if (strcmp(name, "sup_wpa") == 0) {
        if (F->Model == FAKE_43752 && iv) {
            *status = -23;  /* this model's firmware has no idsup */
            return 1;
        }
        F->SupWpa = iv;
    } else if (strcmp(name, "wpaie") == 0) {
        F->WpaIeLen = vlen < sizeof(F->WpaIe) ? vlen : sizeof(F->WpaIe);
        memcpy(F->WpaIe, v, F->WpaIeLen);
    } else if (strcmp(name, "country") == 0 && vlen >= 12) {
        F->Country = (unsigned)v[0] << 8 | v[1];
    } else if (strcmp(name, "escan") == 0) {
        if (vlen < ESCAN_PARAMS_FIXED || get_le32(v) != ESCAN_REQ_VERSION || get_le16(v + 4) != ESCAN_ACTION_START ||
            v[8 + 42] != BSSTYPE_ANY || get_le32(v + 8 + 60) != 0) {
            violation(F, "bad escan parameters", vlen, get_le32(v));
        }
        if (!F->Up) {
            *status = -4;   /* BCME_NOTUP */
            return 1;
        }
        /* the results come after the reply (handle_tx_frame) */
        F->EscanPending = 1;
        F->EscanSync = get_le16(v + 6);
    } else if (strcmp(name, "wsec_key") == 0) {
        set_key(F, v, vlen);
    } else {
        *status = -23;
        return 1;
    }
    return 0;
}

/* A complete frame arrived on F2: a command to answer, or data. */
static void
handle_tx_frame(fake_chip *F, const unsigned char *p, unsigned len)
{
    unsigned flen = get_le16(p), off, cmd, dlen, flags;
    static unsigned char reply[12 + 16 + 8192];
    const unsigned char *d;
    unsigned char *out;
    int err = 0, status = 0;
    unsigned rlen, before;

    if (((flen ^ get_le16(p + 2)) & 0xFFFF) != 0xFFFF || flen > len || flen < 12) {
        violation(F, "bad SDPCM header from the host", flen, len);
        return;
    }
    if (p[4] != F->ExpectSeq) {
        violation(F, "SDPCM sequence number out of order", p[4], F->ExpectSeq);
    }
    if ((unsigned char)(F->AdvMaxSeq - p[4]) == 0 || ((unsigned char)(F->AdvMaxSeq - p[4]) & 0x80)) {
        violation(F, "frame beyond the credit window", p[4], F->AdvMaxSeq);
    }
    F->ExpectSeq = p[4] + 1;
    off = p[7];
    if (off < 12 || off > flen) {
        violation(F, "bad data offset", off, flen);
        return;
    }
    if ((p[5] & 0x0F) == SDPCM_CH_DATA) {
        before = F->RxLen;
        host_data(F, p + off, flen - off);
        if (F->RxLen == before) {
            /* no answer to carry the credit: a header-only frame does */
            unsigned char none[1];
            queue_frame(F, SDPCM_CH_DATA, none, 0);
        }
        return;
    }
    if ((p[5] & 0x0F) != SDPCM_CH_CONTROL) {
        violation(F, "host sent a frame on neither the control nor the data channel", p[5], 0);
        return;
    }
    if (off + 16 > flen) {
        violation(F, "bad data offset", off, flen);
        return;
    }
    d = p + off;
    cmd = get_le32(d);
    dlen = get_le32(d + 4);
    flags = get_le32(d + 8);
    if (off + 16 + dlen > flen || dlen > 8192) {
        violation(F, "dcmd length past the frame", dlen, flen);
        return;
    }
    F->DcmdSeen++;
    memcpy(reply, d, 16 + dlen);
    rlen = 16 + dlen;
    d += 16;
    out = reply + 16;

    if (cmd == C_GET_VAR && !(flags & BCDC_DCMD_SET)) {
        err = get_var(F, (const char *)d, out, dlen, &status);
    } else if (cmd == C_SET_VAR && (flags & BCDC_DCMD_SET)) {
        unsigned nl = 0;
        while (nl < dlen && d[nl]) nl++;
        nl++;
        err = nl > dlen ? (status = -2, 1) : set_var(F, (const char *)d, d + nl, dlen - nl, &status);
    } else if (cmd == C_GET_VERSION && !(flags & BCDC_DCMD_SET)) {
        put_le32(out, 2);
    } else if (!(flags & BCDC_DCMD_SET) && cmd == C_GET_BANDLIST && dlen >= 12) {
        put_le32(out, 2); put_le32(out + 4, 1); put_le32(out + 8, 2);
    } else if (!(flags & BCDC_DCMD_SET) && cmd == C_GET_RATE && dlen >= 4) {
        if (F->Assoc < 0) { err = 1; status = -17; } else put_le32(out, 130);  /* 65 Mbit/s */
    } else if (!(flags & BCDC_DCMD_SET) && cmd == C_GET_RSSI && dlen >= 4) {
        if (F->Assoc < 0) { err = 1; status = -17; } else put_le32(out, (unsigned)-52);
    } else if ((flags & BCDC_DCMD_SET) && dlen >= 4 &&
               (cmd == C_UP || cmd == C_SET_PM || cmd == C_SET_INFRA || cmd == C_SET_AUTH)) {
        unsigned v = get_le32(d);
        if (cmd == C_UP) F->Up = 1;
        else if (cmd == C_SET_PM) F->Pm = v;
        else if (cmd == C_SET_INFRA) F->Infra = v;
        else F->Auth = v;
    } else if ((flags & BCDC_DCMD_SET) && cmd == C_SET_WSEC_PMK) {
        unsigned kl = dlen >= 4 ? get_le16(d) : 0;
        if (dlen < WSEC_PMK_LEN || kl > 64 || get_le16(d + 2) != WSEC_PASSPHRASE) {
            err = 1; status = -2;
        } else {
            memcpy(F->Pmk, d + 4, kl);
            F->Pmk[kl] = 0;
            F->PmkLen = kl;
        }
    } else if ((flags & BCDC_DCMD_SET) && cmd == C_SET_SSID) {
        /* the reply first, the join's events after it */
        queue_frame(F, SDPCM_CH_CONTROL, reply, rlen);
        join(F, d, dlen);
        return;
    } else if ((flags & BCDC_DCMD_SET) && cmd == C_DISASSOC) {
        queue_frame(F, SDPCM_CH_CONTROL, reply, rlen);
        if (F->Assoc >= 0) {
            leave(F, E_DISASSOC, dlen >= 4 ? get_le32(d) : 0);
        }
        return;
    } else if ((flags & BCDC_DCMD_SET) && cmd == C_SCAN) {
        F->ScanAborts++;
        if (F->EscanPending) {
            unsigned char e[12];
            memset(e, 0, sizeof(e));
            put_le32(e, 12);
            put_le16(e + 8, F->EscanSync);
            queue_frame(F, SDPCM_CH_CONTROL, reply, rlen);
            F->EscanPending = 0;
            queue_event_full(F, E_ESCAN_RESULT, 4, 0, 0, NULL, e, 12);  /* aborted */
            return;
        }
    } else {
        err = 1;
        status = -23;
    }
    if (err) {
        put_le32(reply + 8, flags | BCDC_DCMD_ERROR);
        put_le32(reply + 12, (unsigned)status);
    }
    if (F->ReplyExtra) {
        unsigned extra = F->ReplyExtra;

        if (rlen + extra > sizeof(reply)) {
            extra = (unsigned)sizeof(reply) - rlen;
        }
        memset(reply + rlen, 0xEE, extra);
        rlen += extra;
        put_le32(reply + 4, dlen + extra);     /* and say so in the length */
    }
    queue_frame(F, SDPCM_CH_CONTROL, reply, rlen);
    if (F->EscanPending && !F->HoldEscan) {
        F->EscanPending = 0;
        escan(F, F->EscanSync);
    }
}

static int
fake_cmd53(void *Ctx, unsigned Fn, unsigned Addr, int Incr, int Write, int Block,
           unsigned char *Buf, unsigned Len)
{
    fake_chip *F = Ctx;
    unsigned bs;

    F->Cmd53++;
    if (F->FailCmd53At && F->Cmd53 == F->FailCmd53At) {
        return -1;
    }
    if (Fn < 1 || Fn > 2) { violation(F, "CMD53 to F0", Addr, Fn); return -1; }
    bs = F->BlkSize[Fn];
    if (Block) {
        F->Cmd53Block++;
        if (Fn == 2) F->Cmd53F2Block++;
        if (bs == 0 || Len % bs || Len / bs > 511 || Len == 0) { violation(F, "bad block-mode CMD53", Len, bs); return -1; }
    } else {
        F->Cmd53Byte++;
        if (Len == 0 || Len > 512 || Len > bs) { violation(F, "byte-mode CMD53 over a block", Len, bs); return -1; }
    }
    if (Len > F->MaxXfer) F->MaxXfer = Len;
    if (Len & 3) { violation(F, "CMD53 length not a multiple of 4", Len, 0); return -1; }
    if (!(F->Cccr[CCCR_IOEX] & (1u << Fn))) { violation(F, "CMD53 to a disabled function", Fn, Addr); return -1; }

    if (Fn == 2) {
        if (!(F->Cccr[CCCR_IORX] & 0x04)) { violation(F, "F2 access before F2 ready", Addr, Len); return -1; }
        if (Write) {
            if (!Incr) violation(F, "F2 write with a fixed address", Addr, 0);
            if (F->TxLen + Len > TX_MAX) { violation(F, "F2 TX overflow", F->TxLen, Len); return -1; }
            memcpy(F->Tx + F->TxLen, Buf, Len);
            F->TxLen += Len;
            if (F->TxLen >= 4) {
                unsigned flen = get_le16(F->Tx);
                unsigned need = flen;
                unsigned fbs = F->BlkSize[2];
                if (fbs && flen > fbs && flen % fbs) need = (flen + fbs - 1) / fbs * fbs; else need = (flen + 3) & ~3u;
                if (F->TxLen >= need) {
                    handle_tx_frame(F, F->Tx, F->TxLen);
                    F->TxLen = 0;
                }
            }
        } else {
            unsigned n = 0;
            if (Incr) violation(F, "F2 read with an incrementing address", Addr, 0);
            if (F->RxPos < F->RxLen) {
                n = F->RxLen - F->RxPos;
                if (n > Len) n = Len;
                memcpy(Buf, F->Rx + F->RxPos, n);
                F->RxPos += n;
            }
            memset(Buf + n, 0, Len - n);
            if (F->RxPos == F->RxLen) F->RxPos = F->RxLen = 0;
        }
        return 0;
    }

    /* F1: the backplane window */
    if (!(Addr & SB_ACCESS_4B)) { violation(F, "F1 CMD53 without the 4-byte access flag", Addr, 0); return -1; }
    if (!Incr) { violation(F, "F1 CMD53 with a fixed address", Addr, 0); return -1; }
    if ((Addr & SB_WINDOW_MASK) + Len > SB_WINDOW_SIZE) { violation(F, "F1 CMD53 crosses the window", Addr, Len); return -1; }
    F->BytesF1 += Len;
    for (unsigned i = 0; i < Len; i += 4) {
        unsigned a = F->Window + ((Addr & SB_WINDOW_MASK) + i);
        if (Write) bp_write32(F, a, get_le32(Buf + i));
        else put_le32(Buf + i, bp_read32(F, a));
    }
    return 0;
}

static void
fake_delay(void *Ctx, unsigned Us)
{
    (void)Ctx;
    (void)Us;
}

static void
fake_log(void *Ctx, const char *Msg)
{
    (void)Ctx;
    printf("  log: %s\n", Msg);
}

const bwl_bus_ops FakeOps = {
    fake_cmd52,
    fake_cmd53,
    fake_delay,
    fake_log,
};
