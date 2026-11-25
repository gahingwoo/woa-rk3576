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
    memcpy(p + 12, payload, len);
    F->RxLen += padded;
    F->IntStatus |= INTSTATUS_HMB_FRAME_IND;
}

static void
queue_event(fake_chip *F, unsigned type, unsigned status)
{
    unsigned char e[4 + 14 + 10 + 48];
    unsigned char *eth = e + 4, *h = eth + 14, *m = h + 10;

    memset(e, 0, sizeof(e));
    e[0] = BCDC_FLAG_VER(BCDC_PROTO_VER);
    memset(eth, 0xFF, 6);
    put_be16(eth + 12, ETHERTYPE_BRCM);
    put_be16(h + 0, 0x8001);
    put_be16(h + 2, 10 + 48 - 4);
    h[4] = 1;
    h[5] = 0x00; h[6] = 0x10; h[7] = 0x18;
    put_be16(h + 8, BRCM_SUBTYPE_EVENT);
    put_be16(m + 0, 2);
    put_be32(m + 4, type);
    put_be32(m + 8, status);
    memcpy(m + 30, "wlan0", 5);
    queue_frame(F, SDPCM_CH_EVENT, e, sizeof(e));
    F->EventsQueued++;
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
    put_le32(F->Ram + cons + CONSOLE_LOG_BUF, F->RamBase + logbuf);
    put_le32(F->Ram + cons + CONSOLE_LOG_BUFSZ, 0x100);
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

/* A complete control frame arrived on F2: answer it. */
static void
handle_tx_frame(fake_chip *F, const unsigned char *p, unsigned len)
{
    unsigned flen = get_le16(p), off, cmd, dlen, flags;
    unsigned char reply[12 + 16 + 8192];
    const unsigned char *d;
    int err = 0, status = 0;
    unsigned rlen;

    if (((flen ^ get_le16(p + 2)) & 0xFFFF) != 0xFFFF || flen > len || flen < 12) {
        violation(F, "bad SDPCM header from the host", flen, len);
        return;
    }
    if (p[4] != F->ExpectSeq) {
        violation(F, "SDPCM sequence number out of order", p[4], F->ExpectSeq);
    }
    F->ExpectSeq = p[4] + 1;
    if ((p[5] & 0x0F) != SDPCM_CH_CONTROL) {
        violation(F, "host sent a non-control frame", p[5], 0);
        return;
    }
    off = p[7];
    if (off < 12 || off + 16 > flen) {
        violation(F, "bad data offset", off, flen);
        return;
    }
    d = p + off;
    cmd = get_le32(d);
    dlen = get_le32(d + 4);
    flags = get_le32(d + 8);
    if (off + 16 + dlen > flen) {
        violation(F, "dcmd length past the frame", dlen, flen);
        return;
    }
    F->DcmdSeen++;
    memcpy(reply, d, 16 + dlen);
    rlen = 16 + dlen;
    d += 16;

    if (cmd == C_GET_VERSION && !(flags & BCDC_DCMD_SET)) {
        put_le32(reply + 16, 2);
    } else if (cmd == C_GET_VAR && !(flags & BCDC_DCMD_SET)) {
        const char *name = (const char *)d;
        if (strcmp(name, "ver") == 0) {
            static const char v4345[] = "wl0: Nov 24 2026 12:00:00 version 7.45.96.0 (bwltest) FWID 01-12345678\n";
            static const char v43752[] = "wl0: Nov 24 2026 12:00:00 version 18.35.387.23.7 (bwltest) FWID 01-12345678\n";
            const char *v = F->Model == FAKE_43752 ? v43752 : v4345;
            unsigned vl = (unsigned)strlen(v) + 1;
            memset(reply + 16, 0, dlen);
            memcpy(reply + 16, v, vl < dlen ? vl : dlen);
        } else if (strcmp(name, "cur_etheraddr") == 0) {
            static const unsigned char mac[6] = { 0x00, 0x90, 0x4c, 0xc5, 0x12, 0x38 };
            memcpy(reply + 16, mac, 6);
        } else if (strcmp(name, "clmload_status") == 0) {
            int ok = !F->ClmBad && F->ExpectClm && F->ClmLen == F->ExpectClmLen &&
                     memcmp(F->Clm, F->ExpectClm, F->ClmLen) == 0;
            put_le32(reply + 16, ok ? 0 : 1);
        } else {
            err = 1;
            status = -23;   /* BCME_UNSUPPORTED */
        }
    } else if (cmd == C_SET_VAR && (flags & BCDC_DCMD_SET)) {
        const char *name = (const char *)d;
        if (strcmp(name, "clmload") == 0) {
            const unsigned char *c = d + 8;
            unsigned fl = get_le16(c), ty = get_le16(c + 2), n = get_le32(c + 4);
            if (!(fl & DLOAD_FLAG_HANDLER_VER) || ty != DLOAD_TYPE_CLM || n > BWL_DLOAD_MAX ||
                12 + 8 + n > dlen || F->ClmLen + n > 64 * 1024) {
                F->ClmBad = 1;
            } else {
                if (((fl & DLOAD_FLAG_BEGIN) != 0) != (F->ClmLen == 0)) F->ClmBad = 1;
                memcpy(F->Clm + F->ClmLen, c + 12, n);
                F->ClmLen += n;
                if ((fl & DLOAD_FLAG_END) && F->ClmLen != F->ExpectClmLen) F->ClmBad = 1;
            }
        } else {
            err = 1;
            status = -23;
        }
    } else {
        err = 1;
        status = -23;
    }
    if (err) {
        put_le32(reply + 8, flags | BCDC_DCMD_ERROR);
        put_le32(reply + 12, (unsigned)status);
    }
    queue_frame(F, SDPCM_CH_CONTROL, reply, rlen);
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
