/*
 * bwl_chip.c - Broadcom AI-interconnect chip handling: clock up for the
 * first backplane access, the chip ID, the enumeration ROM scan, holding
 * and releasing the ARM CR4, RAM size and base, save/restore capability.
 *
 * A port of OpenBSD bwfm (bwfm.c: bwfm_chip_attach, bwfm_chip_dmp_erom_scan,
 * bwfm_chip_dmp_get_regaddr, bwfm_chip_ai_*, bwfm_chip_cr4_set_*,
 * bwfm_chip_tcm_ramsize/rambase, bwfm_chip_sr_capable; if_bwfm_sdio.c:
 * bwfm_sdio_buscore_prepare and _activate), reduced to the ARM CR4 chips
 * with TCM, which is what the BCM4345 family and the BCM43752 are.
 *
 * The chip table: the 4345 values are bwfm's. For the 43752, the RAM base
 * (0x170000) and the save/restore check (retention control only) are in
 * both bwfm (bwfm_chip_tcm_rambase, bwfm_chip_sr_capable, CY_CC_43752) and
 * Linux brcmfmac (chip.c); the SDIO-side differences are brcmfmac's only
 * (sdio.c brcmf_sdio_aos_no_decode and the F2 watermark in
 * brcmf_sdio_bus_preinit, bcmsdh.c SDIO_4373_FUNC2_BLOCKSIZE; ISC,
 * Broadcom). bwfm's SDIO attach writes watermark 8 for every chip.
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

static const bwl_chip_info ChipTable[] = {
    /* Chip        revs    name     RAM base  F2 blk  WM    MESBUSY  NODEC  SR */
    { CHIP_4345,   9, 9,   "43456", 0x198000, 0,      8,    0,       0,     BWL_SR_CHIPCTL3 },
    { CHIP_4345,   0, 255, "43455", 0x198000, 0,      8,    0,       0,     BWL_SR_CHIPCTL3 },
    /* brcmfmac: CY_4373_F2_WATERMARK 0x40, MESBUSYCTRL 0x40 | ENAB, F2 blocks of 256 */
    { CHIP_43751,  0, 255, "43752", 0x170000, 256,    0x40, 0x40 | MESBUSYCTRL_ENAB, 1, BWL_SR_RETENTION },
    { CHIP_43752,  0, 255, "43752", 0x170000, 256,    0x40, 0x40 | MESBUSYCTRL_ENAB, 1, BWL_SR_RETENTION },
};

const bwl_chip_info *
BwlChipInfo(u16 Chip, u8 Rev)
{
    unsigned i;

    for (i = 0; i < sizeof(ChipTable) / sizeof(ChipTable[0]); i++) {
        if (ChipTable[i].Chip == Chip && Rev >= ChipTable[i].RevMin && Rev <= ChipTable[i].RevMax) {
            return &ChipTable[i];
        }
    }
    return NULL;
}

static u32
Rd(bwl_dev *Dev, u32 Addr, int *Err)
{
    u32 v = 0;

    if (*Err == 0 && BwlRead32(Dev, Addr, &v)) {
        *Err = BWL_E_IO;
    }
    return v;
}

static void
Wr(bwl_dev *Dev, u32 Addr, u32 Val, int *Err)
{
    if (*Err == 0 && BwlWrite32(Dev, Addr, Val)) {
        *Err = BWL_E_IO;
    }
}

const bwl_core *
BwlChipCore(const bwl_dev *Dev, u16 Id, unsigned Index)
{
    unsigned i;

    for (i = 0; i < Dev->NumCores; i++) {
        if (Dev->Cores[i].Id == Id && Index-- == 0) {
            return &Dev->Cores[i];
        }
    }
    return NULL;
}

const bwl_core *
BwlChipPmu(const bwl_dev *Dev)
{
    const bwl_core *cc = BwlChipCore(Dev, CORE_CHIPCOMMON, 0);
    const bwl_core *pmu;

    if (cc != NULL && cc->Rev >= 35 && (Dev->CcCapsExt & CC_CAPS_EXT_AOB)) {
        pmu = BwlChipCore(Dev, CORE_PMU, 0);
        if (pmu != NULL) {
            return pmu;
        }
    }
    return cc;
}

/* bwfm_sdio_buscore_prepare: ALP clock on, so that the backplane answers. */
static int
BuscorePrepare(bwl_dev *Dev)
{
    u8 set, val = 0, mask = CLKCSR_ALP_AVAIL | CLKCSR_HT_AVAIL;
    int i;

    set = CLKCSR_ALP_AVAIL_REQ | CLKCSR_FORCE_HW_CLKREQ_OFF;
    if (BwlWrite8(Dev, F1_CHIPCLKCSR, set) || BwlRead8(Dev, F1_CHIPCLKCSR, &val)) {
        return BWL_E_IO;
    }
    if ((val & ~mask) != set) {
        Dev->Detail = ((u32)set << 8) | val;
        return BWL_E_NOCHIP;
    }
    for (i = 0; i < 1000; i++) {
        if (BwlRead8(Dev, F1_CHIPCLKCSR, &val)) {
            return BWL_E_IO;
        }
        if (val & mask) {
            break;
        }
        Dev->Ops->delay_us(Dev->Ctx, 10);
    }
    if ((val & mask) == 0) {
        Dev->Detail = val;
        return BWL_E_TIMEOUT;
    }
    if (BwlWrite8(Dev, F1_CHIPCLKCSR, CLKCSR_FORCE_HW_CLKREQ_OFF | CLKCSR_FORCE_ALP)) {
        return BWL_E_IO;
    }
    Dev->Ops->delay_us(Dev->Ctx, 65);
    return BwlWrite8(Dev, F1_SDIOPULLUP, 0) ? BWL_E_IO : BWL_OK;
}

/* bwfm_chip_dmp_get_regaddr. Returns 0 with Base/Wrap set, 1 to skip the core. */
static int
DmpGetRegAddr(bwl_dev *Dev, u32 *Erom, u32 End, u32 *Base, u32 *Wrap, int *Err)
{
    u32 val, type, stype, sztype, wraptype;

    *Base = 0;
    *Wrap = 0;

    val = Rd(Dev, *Erom, Err);
    type = val & DMP_DESC_MASK;
    if (type == DMP_DESC_MASTER_PORT) {
        wraptype = DMP_SLAVE_TYPE_MWRAP;
        *Erom += 4;
    } else if ((type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS) {
        wraptype = DMP_SLAVE_TYPE_SWRAP;
    } else {
        return 1;
    }

    do {
        do {
            if (*Err || *Erom >= End) {
                return 1;
            }
            val = Rd(Dev, *Erom, Err);
            type = val & DMP_DESC_MASK;
            if (type == DMP_DESC_COMPONENT) {
                return 0;
            }
            if (type == DMP_DESC_EOT) {
                return 1;
            }
            *Erom += 4;
        } while ((type & ~DMP_DESC_ADDRSIZE_GT32) != DMP_DESC_ADDRESS);

        if (type & DMP_DESC_ADDRSIZE_GT32) {
            *Erom += 4;
        }
        sztype = DMP_SLAVE_SIZE_TYPE(val);
        if (sztype == DMP_SLAVE_SIZE_DESC) {
            u32 sz = Rd(Dev, *Erom, Err);

            *Erom += (sz & DMP_DESC_ADDRSIZE_GT32) ? 8 : 4;
        }
        if (sztype != DMP_SLAVE_SIZE_4K && sztype != DMP_SLAVE_SIZE_8K) {
            continue;
        }
        stype = DMP_SLAVE_TYPE(val);
        if (*Base == 0 && stype == DMP_SLAVE_TYPE_SLAVE) {
            *Base = val & DMP_SLAVE_ADDR_BASE;
        }
        if (*Wrap == 0 && stype == wraptype) {
            *Wrap = val & DMP_SLAVE_ADDR_BASE;
        }
    } while (*Base == 0 || *Wrap == 0);

    return 0;
}

/* bwfm_chip_dmp_erom_scan. The EROM is at most 4 KB; stop there in any case. */
static int
EromScan(bwl_dev *Dev)
{
    int err = 0;
    u32 erom = Rd(Dev, CHIP_BASE + CC_EROMPTR, &err);
    u32 end = erom + 0x1000;
    u32 val, type = 0, base, wrap;

    Dev->NumCores = 0;
    while (err == 0 && type != DMP_DESC_EOT && erom < end) {
        u16 id;
        u8 nmw, nsw, rev;

        val = Rd(Dev, erom, &err);
        type = val & DMP_DESC_MASK;
        erom += 4;
        if (type != DMP_DESC_COMPONENT) {
            continue;
        }
        id = (u16)DMP_COMP_PARTNUM(val);

        val = Rd(Dev, erom, &err);
        type = val & DMP_DESC_MASK;
        erom += 4;
        if (type != DMP_DESC_COMPONENT) {
            return err ? err : BWL_E_NOCHIP;
        }
        nmw = (u8)DMP_COMP_NUM_MWRAP(val);
        nsw = (u8)DMP_COMP_NUM_SWRAP(val);
        rev = (u8)DMP_COMP_REVISION(val);

        if (nmw + nsw == 0 && id != CORE_PMU && id != CORE_GCI) {
            continue;
        }
        if (DmpGetRegAddr(Dev, &erom, end, &base, &wrap, &err)) {
            continue;
        }
        if (Dev->NumCores == BWL_MAX_CORES) {
            return BWL_E_SPACE;
        }
        Dev->Cores[Dev->NumCores].Id = id;
        Dev->Cores[Dev->NumCores].Rev = rev;
        Dev->Cores[Dev->NumCores].Base = base;
        Dev->Cores[Dev->NumCores].Wrap = wrap;
        Dev->NumCores++;
    }
    return err;
}

/* bwfm_chip_ai_disable */
static void
AiDisable(bwl_dev *Dev, const bwl_core *Core, u32 PreReset, u32 Reset, int *Err)
{
    u32 val;
    int i;

    val = Rd(Dev, Core->Wrap + AGENT_RESET_CTL, Err);
    if ((val & RESET_CTL_RESET) == 0) {
        Wr(Dev, Core->Wrap + AGENT_IOCTL, PreReset | IOCTL_FGC | IOCTL_CLK, Err);
        (void)Rd(Dev, Core->Wrap + AGENT_IOCTL, Err);
        Wr(Dev, Core->Wrap + AGENT_RESET_CTL, RESET_CTL_RESET, Err);
        Dev->Ops->delay_us(Dev->Ctx, 20);
        for (i = 300; i > 0 && *Err == 0; i--) {
            if (Rd(Dev, Core->Wrap + AGENT_RESET_CTL, Err) == RESET_CTL_RESET) {
                break;
            }
        }
        if (i == 0 && *Err == 0) {
            *Err = BWL_E_TIMEOUT;
        }
    }
    Wr(Dev, Core->Wrap + AGENT_IOCTL, Reset | IOCTL_FGC | IOCTL_CLK, Err);
    (void)Rd(Dev, Core->Wrap + AGENT_IOCTL, Err);
}

/* bwfm_chip_ai_reset */
static void
AiReset(bwl_dev *Dev, const bwl_core *Core, u32 PreReset, u32 Reset, u32 PostReset, int *Err)
{
    int i;

    AiDisable(Dev, Core, PreReset, Reset, Err);
    for (i = 50; i > 0 && *Err == 0; i--) {
        if ((Rd(Dev, Core->Wrap + AGENT_RESET_CTL, Err) & RESET_CTL_RESET) == 0) {
            break;
        }
        Wr(Dev, Core->Wrap + AGENT_RESET_CTL, 0, Err);
        Dev->Ops->delay_us(Dev->Ctx, 60);
    }
    if (i == 0 && *Err == 0) {
        *Err = BWL_E_TIMEOUT;
    }
    Wr(Dev, Core->Wrap + AGENT_IOCTL, PostReset | IOCTL_CLK, Err);
    (void)Rd(Dev, Core->Wrap + AGENT_IOCTL, Err);
}

/* bwfm_chip_cr4_set_passive: halt the CPU, hold the 802.11 cores in reset. */
int
BwlChipSetPassive(bwl_dev *Dev)
{
    const bwl_core *cr4 = BwlChipCore(Dev, CORE_ARM_CR4, 0);
    const bwl_core *d11;
    unsigned i = 0;
    int err = 0;
    u32 val;

    if (cr4 == NULL) {
        return BWL_E_NOCHIP;
    }
    val = Rd(Dev, cr4->Wrap + AGENT_IOCTL, &err);
    AiReset(Dev, cr4, val & IOCTL_ARMCR4_CPUHALT, IOCTL_ARMCR4_CPUHALT, IOCTL_ARMCR4_CPUHALT, &err);
    while ((d11 = BwlChipCore(Dev, CORE_80211, i++)) != NULL) {
        AiDisable(Dev, d11, IOCTL_D11_PHYRESET | IOCTL_D11_PHYCLOCKEN, IOCTL_D11_PHYCLOCKEN, &err);
    }
    Dev->FwRunning = 0;
    return err;
}

/*
 * bwfm_chip_cr4_set_active with bwfm_sdio_buscore_activate: clear the SDIO
 * core's interrupt status, put the reset vector at address 0, release the CPU.
 */
int
BwlChipSetActive(bwl_dev *Dev, u32 ResetVector)
{
    const bwl_core *cr4 = BwlChipCore(Dev, CORE_ARM_CR4, 0);
    u8 rv[4];
    int err = 0;

    if (cr4 == NULL) {
        return BWL_E_NOCHIP;
    }
    if (BwlSdioCoreWrite(Dev, SDCORE_INTSTATUS, 0xFFFFFFFFu)) {
        return BWL_E_IO;
    }
    if (ResetVector) {
        BwlPutLe32(rv, ResetVector);
        if (BwlRamRw(Dev, 0, rv, 4, 1)) {
            return BWL_E_IO;
        }
    }
    AiReset(Dev, cr4, IOCTL_ARMCR4_CPUHALT, 0, 0, &err);
    if (err == 0) {
        Dev->FwRunning = 1;
    }
    return err;
}

/* bwfm_chip_tcm_ramsize */
static int
TcmRamSize(bwl_dev *Dev, const bwl_core *Cr4)
{
    int err = 0;
    u32 cap, totb, i, info, blk, size = 0;

    cap = Rd(Dev, Cr4->Base + ARMCR4_CAP, &err);
    totb = ARMCR4_CAP_TCBANB(cap) + ARMCR4_CAP_TCBBNB(cap);
    for (i = 0; i < totb && err == 0; i++) {
        Wr(Dev, Cr4->Base + ARMCR4_BANKIDX, i, &err);
        info = Rd(Dev, Cr4->Base + ARMCR4_BANKINFO, &err);
        blk = (info & ARMCR4_BANKINFO_BLK_1K) ? 1024u : 8192u;
        size += (ARMCR4_BANKINFO_BSZ(info) + 1) * blk;
    }
    Dev->RamSize = size;
    return err;
}

/* bwfm_chip_tcm_rambase, for the chips this driver knows. */
static int
TcmRamBase(bwl_dev *Dev)
{
    if (Dev->Info == NULL) {
        return BWL_E_NOCHIP;
    }
    Dev->RamBase = Dev->Info->RamBase;
    return BWL_OK;
}

/* bwfm_chip_sr_capable / brcmf_chip_sr_capable, by the table's method. */
int
BwlChipSrCapable(bwl_dev *Dev)
{
    const bwl_core *pmu = BwlChipPmu(Dev);
    int err = 0;
    u32 reg;

    if (Dev->PmuRev < 17 || pmu == NULL) {
        return 0;
    }
    switch (Dev->Info ? Dev->Info->SrCheck : BWL_SR_GENERIC) {
    case BWL_SR_CHIPCTL3:
        Wr(Dev, pmu->Base + CC_CHIPCTL_ADDR, 3, &err);
        reg = Rd(Dev, pmu->Base + CC_CHIPCTL_DATA, &err);
        return err == 0 && (reg & (1u << 2)) != 0;
    case BWL_SR_RETENTION:
        break;
    default:
        reg = Rd(Dev, pmu->Base + CC_PMUCAPS_EXT, &err);
        if (err || (reg & PMUCAPS_EXT_SR_SUPP) == 0) {
            return 0;
        }
        break;
    }
    reg = Rd(Dev, pmu->Base + CC_RETENTION_CTL, &err);
    return err == 0 && (reg & (RETENTION_MACPHY_DIS | RETENTION_LOGIC_DIS)) == 0;
}

int
BwlChipAttach(bwl_dev *Dev)
{
    const bwl_core *core;
    int err = 0;
    unsigned i;
    int cpu = 0;

    Dev->Step = "buscore prepare";
    err = BuscorePrepare(Dev);
    if (err) {
        return err;
    }

    Dev->Step = "chip id";
    Dev->ChipIdReg = Rd(Dev, CHIP_BASE + CC_CHIPID, &err);
    if (err) {
        return err;
    }
    Dev->Chip = (u16)CHIPID_ID(Dev->ChipIdReg);
    Dev->ChipRev = (u8)CHIPID_REV(Dev->ChipIdReg);
    Dev->Info = BwlChipInfo(Dev->Chip, Dev->ChipRev);
    if (CHIPID_TYPE(Dev->ChipIdReg) != CHIPID_TYPE_SOCI_AI) {
        Dev->Detail = Dev->ChipIdReg;
        return BWL_E_NOCHIP;
    }

    Dev->Step = "erom scan";
    err = EromScan(Dev);
    if (err) {
        return err;
    }
    for (i = 0; i < Dev->NumCores; i++) {
        if (Dev->Cores[i].Id == CORE_ARM_CR4) {
            cpu = 1;
        }
    }
    if (!cpu || BwlChipCore(Dev, CORE_CHIPCOMMON, 0) == NULL ||
        BwlChipCore(Dev, CORE_SDIO_DEV, 0) == NULL) {
        /* Only ARM CR4 chips with TCM are handled here (the 4345 family, the 43752). */
        Dev->Detail = Dev->NumCores;
        return BWL_E_NOCHIP;
    }

    Dev->Step = "cpu halt";
    err = BwlChipSetPassive(Dev);
    if (err) {
        return err;
    }

    Dev->Step = "ram size";
    core = BwlChipCore(Dev, CORE_ARM_CR4, 0);
    err = TcmRamSize(Dev, core);
    if (err) {
        return err;
    }
    err = TcmRamBase(Dev);
    if (err) {
        return err;
    }

    Dev->Step = "caps";
    core = BwlChipCore(Dev, CORE_CHIPCOMMON, 0);
    Dev->CcCaps = Rd(Dev, core->Base + CC_CAPS, &err);
    Dev->CcCapsExt = Rd(Dev, core->Base + CC_CAPS_EXT, &err);
    core = BwlChipPmu(Dev);
    if (err == 0 && (Dev->CcCaps & CC_CAPS_PMU)) {
        Dev->PmuCaps = Rd(Dev, core->Base + CC_PMUCAPS, &err);
        Dev->PmuRev = (u8)(Dev->PmuCaps & PMUCAPS_REV_MASK);
    }
    return err;
}

const char *
BwlChipFwName(u16 Chip, u8 Rev)
{
    const bwl_chip_info *info = BwlChipInfo(Chip, Rev);

    return info ? info->FwName : NULL;
}
