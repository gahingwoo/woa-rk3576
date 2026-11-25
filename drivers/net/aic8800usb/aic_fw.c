/*
 * aic_fw.c - identify the AIC8800D80 and download its firmware through the
 * ROM loader (USB PID 8D80), the sequence of aic_load_fw's
 * aicfw_download_fw_8800d80() for a U02/U03 chip in normal mode:
 *
 *   1. read the chip ID (0x40500000, 0x00000004)
 *   2. parse fw_patch_table: its AICBT_PINF_T section says where the adid
 *      and patch files go and which extra patch files there are
 *   3. write fw_adid, fw_patch and each fw_patch_..._ext<n> to chip RAM
 *   4. write every (address, value) pair of the table, with the Bluetooth
 *      settings put into its AICBT_MODE_T section (this is also what makes
 *      the Bluetooth half come up as a USB HCI interface)
 *   5. write fmacfw (the Wi-Fi firmware) at 0x120000
 *   6. the "patch config": find the firmware's config and patch blocks
 *      through pointers in its image, write the magic numbers and three
 *      (offset, value) pairs
 *   7. DBG_START_APP at 0x120000: the chip runs the firmware and comes back
 *      on the bus as PID 8D81
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic.h"
#include "aic_proto.h"

int
aic_identify(aic_dev *Dev)
{
    u32 w = 0, sub = 0;
    int rc;

    rc = aic_mem_read(Dev, AIC_CHIP_ID_ADDR, &w);
    if (rc != AIC_OK) {
        return rc;
    }
    rc = aic_mem_read(Dev, AIC_CHIP_SUBID_ADDR, &sub);
    if (rc != AIC_OK) {
        return rc;
    }
    Dev->ChipWord = w;
    Dev->ChipRev = (u8)(w >> 16);
    Dev->ChipSubRev = (u8)(sub >> 4);
    Dev->ChipMcu = ((w >> 25) & 1) == 0;
    Dev->ChipIsH = (Dev->ChipRev & AIC_CHIP_ID_H_MASK) == AIC_CHIP_ID_H_MASK;
    aic_log(Dev, "chip word %x sub %x", w, sub);
    return AIC_OK;
}

/* -------------------------------------------------------- patch table */

typedef struct pt_iter {
    const u8 *Data;
    u32 Len;
    u32 Off;
} pt_iter;

/* Next section: name, type, pair count, pairs. Returns 1, 0 at the end,
 * or a negative error. A type >= 1000 or a zero count carries no pairs
 * (the vendor's "temp workaround"). */
static int
pt_next(pt_iter *It, const u8 **Name, u32 *Type, u32 *Count, const u8 **Pairs)
{
    u32 t, n;

    if (It->Off >= It->Len) {
        return 0;
    }
    if (It->Len - It->Off < 24) {
        return AIC_E_FORMAT;
    }
    *Name = It->Data + It->Off;
    t = aic_get32(It->Data + It->Off + 16);
    n = aic_get32(It->Data + It->Off + 20);
    It->Off += 24;
    if (t >= 1000 || n == 0) {
        n = 0;
    }
    if (n > (It->Len - It->Off) / 8) {
        return AIC_E_FORMAT;
    }
    *Type = t;
    *Count = n;
    *Pairs = It->Data + It->Off;
    It->Off += n * 8;
    return 1;
}

static int
pt_open(const aic_file *Table, pt_iter *It)
{
    static const char tag[] = AIC_PT_TAG;

    if (Table->Data == NULL || Table->Len < 16 + 24 ||
        aic_memcmp(Table->Data, tag, sizeof(tag) - 1) != 0) {
        return AIC_E_FORMAT;
    }
    It->Data = Table->Data;
    It->Len = Table->Len;
    It->Off = 16;
    return AIC_OK;
}

int
aic_fw_parse_table(const aic_file *Table, aic_patch_info *Info)
{
    pt_iter it;
    const u8 *name, *pairs;
    u32 type, count, i;
    int rc, first = 1;

    aic_memset(Info, 0, sizeof(*Info));
    Info->AddrAdid = AIC_D80_ADID_ADDR;
    Info->AddrPatch = AIC_D80_PATCH_ADDR;
    rc = pt_open(Table, &it);
    if (rc != AIC_OK) {
        return rc;
    }
    while ((rc = pt_next(&it, &name, &type, &count, &pairs)) == 1) {
        Info->Sections++;
        if (first) {
            /* aicbt_patch_info_unpack: the first section must be the INF
             * one. Its pairs fill adid_addrinf, addr_adid, patch_addrinf,
             * addr_patch, reset_addr, reset_val, adid_flag_addr, adid_flag
             * (4 pairs), then (ext_patch_nb_addr, ext_patch_nb), then
             * ext_patch_nb pairs of (id, address). */
            first = 0;
            if (type != AIC_PT_INF || count == 0) {
                return AIC_E_FORMAT;
            }
            Info->AddrAdid = aic_get32(pairs + 4);
            if (count >= 2) {
                Info->AddrPatch = aic_get32(pairs + 12);
            }
            if (count > 4) {
                u32 nb = aic_get32(pairs + 4 * 8 + 4);

                if (nb > AIC_MAX_EXT_PATCH || count < 5 + nb) {
                    return AIC_E_FORMAT;
                }
                Info->ExtCount = nb;
                for (i = 0; i < nb; i++) {
                    Info->ExtId[i] = aic_get32(pairs + (5 + i) * 8);
                    Info->ExtAddr[i] = aic_get32(pairs + (5 + i) * 8 + 4);
                }
            }
            Info->Writes += count < 4 ? count : 4;
            continue;
        }
        if (type == AIC_PT_VER) {
            u32 n = count * 8;

            if (n > sizeof(Info->Version) - 1) {
                n = sizeof(Info->Version) - 1;
            }
            for (i = 0; i < n && pairs[i]; i++) {
                Info->Version[i] = (char)((pairs[i] >= 0x20 && pairs[i] < 0x7F) ? pairs[i] : '?');
            }
            Info->Version[i] = 0;
            continue;
        }
        Info->Writes += count;
    }
    if (rc < 0) {
        return rc;
    }
    return first ? AIC_E_FORMAT : AIC_OK;
}

/* aicbt_patch_table_load(): write every pair; patch the BT mode section;
 * the INF section only up to its "base" length; skip the version text. */
static int
pt_load(aic_dev *Dev, const aic_file *Table)
{
    static const u32 btmode[AIC_BT_MODE_COUNT] = AIC_BT_MODE_VALUES;
    pt_iter it;
    const u8 *name, *pairs;
    u32 type, count, i;
    int rc, first = 1;

    rc = pt_open(Table, &it);
    if (rc != AIC_OK) {
        return rc;
    }
    while ((rc = pt_next(&it, &name, &type, &count, &pairs)) == 1) {
        if (first) {
            first = 0;
            if (count > 4) {
                count = 4;
            }
        }
        if (type == AIC_PT_VER) {
            continue;
        }
        for (i = 0; i < count; i++) {
            u32 addr = aic_get32(pairs + i * 8);
            u32 val = aic_get32(pairs + i * 8 + 4);

            if (type == AIC_PT_BTMODE && i < AIC_BT_MODE_COUNT) {
                val = btmode[i];
            }
            rc = aic_mem_write(Dev, addr, val);
            if (rc != AIC_OK) {
                aic_set_error(Dev, "patch table write at %x failed", addr);
                return rc;
            }
        }
        if (type == AIC_PT_PWRON) {
            Dev->Ops->delay_ms(Dev->Ctx, 100);
        }
    }
    return rc < 0 ? rc : AIC_OK;
}

static int
upload(aic_dev *Dev, u32 Addr, const aic_file *F, const char *What)
{
    u32 off = 0;
    int rc;

    if (F->Data == NULL || F->Len == 0) {
        aic_set_error(Dev, What, 0);
        return AIC_E_FORMAT;
    }
    while (off < F->Len) {
        u32 n = F->Len - off;

        if (n > AIC_DBG_BLOCK_MAX) {
            n = AIC_DBG_BLOCK_MAX;
        }
        rc = aic_mem_block_write(Dev, Addr + off, F->Data + off, n);
        if (rc != AIC_OK) {
            aic_set_error(Dev, "block write at %x failed", Addr + off);
            return rc;
        }
        off += n;
    }
    aic_log(Dev, "uploaded %x bytes at %x", F->Len, Addr);
    return AIC_OK;
}

/* aicwf_patch_config_8800d80() with NEW_PATCH_BUFFER_MAP and adap_test 0. */
static int
patch_config(aic_dev *Dev)
{
    static const u32 tbl[AIC_D80_PATCH_TBL_COUNT][2] = AIC_D80_PATCH_TBL;
    u32 config_base, str_base, ver, start = AIC_D80_PATCH_START, i;
    int rc;

    if ((rc = aic_mem_read(Dev, AIC_D80_CONFIG_BASE_PTR, &config_base)) != AIC_OK ||
        (rc = aic_mem_read(Dev, AIC_D80_PATCH_STR_PTR, &str_base)) != AIC_OK ||
        (rc = aic_mem_read(Dev, AIC_D80_FW_VERSION, &ver)) != AIC_OK) {
        return rc;
    }
    Dev->FwVersionWord = ver;
    if (ver > AIC_D80_NEW_PATCH_MAP_VER) {
        rc = aic_mem_read(Dev, AIC_D80_PATCH_BUF_PTR, &start);
        if (rc != AIC_OK) {
            return rc;
        }
    }
    aic_log(Dev, "patch config base %x, strings %x", config_base, str_base);
    if ((rc = aic_mem_write(Dev, str_base + AIC_PATCH_OFS_MAGIC, AIC_PATCH_MAGIC)) != AIC_OK ||
        (rc = aic_mem_write(Dev, str_base + AIC_PATCH_OFS_MAGIC_2, AIC_PATCH_MAGIC_2)) != AIC_OK ||
        (rc = aic_mem_write(Dev, str_base + AIC_PATCH_OFS_PAIR_START, start)) != AIC_OK ||
        (rc = aic_mem_write(Dev, str_base + AIC_PATCH_OFS_PAIR_COUNT, AIC_D80_PATCH_TBL_COUNT)) != AIC_OK) {
        return rc;
    }
    for (i = 0; i < AIC_D80_PATCH_TBL_COUNT; i++) {
        if ((rc = aic_mem_write(Dev, start + 8 * i, tbl[i][0] + config_base)) != AIC_OK ||
            (rc = aic_mem_write(Dev, start + 8 * i + 4, tbl[i][1])) != AIC_OK) {
            return rc;
        }
    }
    for (i = 0; i < 4; i++) {
        rc = aic_mem_write(Dev, str_base + AIC_PATCH_OFS_BLOCK_SIZE + 4 * i, 0);
        if (rc != AIC_OK) {
            return rc;
        }
    }
    return AIC_OK;
}

static int
start_app(aic_dev *Dev, u32 Addr, u32 Type)
{
    u8 req[AIC_DBG_START_APP_REQ_LEN], cfm[4];

    aic_put32(req, Addr);
    aic_put32(req + 4, Type);
    aic_memset(cfm, 0, sizeof(cfm));
    return aic_cmd(Dev, AIC_DBG_START_APP_REQ, AIC_TASK_DBG, req, sizeof(req),
                   AIC_DBG_START_APP_CFM, cfm, sizeof(cfm));
}

int
aic_fw_download(aic_dev *Dev, const aic_fw_files *Files, aic_patch_info *InfoOut)
{
    aic_patch_info info;
    u32 i;
    int rc;

    if (Dev->ChipRev == AIC_CHIP_REV_U01) {
        /* U01 needs other files and addresses (the vendor's U01 branch);
         * every other revision takes the U02 path, as in aic_load_fw. */
        aic_set_error(Dev, "unsupported chip revision %x (U01)", Dev->ChipRev);
        return AIC_E_NOCHIP;
    }
    rc = aic_fw_parse_table(&Files->PatchTable, &info);
    if (rc != AIC_OK) {
        aic_set_error(Dev, "patch table is malformed (%x)", (u32)-rc);
        return rc;
    }
    if (InfoOut) {
        *InfoOut = info;
    }
    if ((rc = upload(Dev, info.AddrAdid, &Files->Adid, "fw_adid missing")) != AIC_OK ||
        (rc = upload(Dev, info.AddrPatch, &Files->Patch, "fw_patch missing")) != AIC_OK) {
        return rc;
    }
    for (i = 0; i < info.ExtCount; i++) {
        if (info.ExtId[i] >= AIC_MAX_EXT_PATCH) {
            return AIC_E_FORMAT;
        }
        rc = upload(Dev, info.ExtAddr[i], &Files->Ext[info.ExtId[i]], "fw_patch ext file missing");
        if (rc != AIC_OK) {
            return rc;
        }
    }
    if ((rc = pt_load(Dev, &Files->PatchTable)) != AIC_OK) {
        return rc;
    }
    if ((rc = upload(Dev, AIC_D80_FMAC_ADDR, &Files->Fmac, "fmacfw missing")) != AIC_OK) {
        return rc;
    }
    if ((rc = patch_config(Dev)) != AIC_OK) {
        aic_set_error(Dev, "patch config failed (%x)", (u32)-rc);
        return rc;
    }
    rc = start_app(Dev, AIC_D80_FMAC_ADDR, AIC_HOST_START_APP_AUTO);
    if (rc == AIC_E_TIMEOUT) {
        /* The chip may leave the bus before its confirm gets out. The
         * re-enumeration as 8D81 is the real answer. */
        aic_log(Dev, "no START_APP confirm, waiting for re-enumeration", 0, 0);
        return AIC_OK;
    }
    return rc;
}

int
aic_fw_reboot(aic_dev *Dev)
{
    /* rwnx_send_reboot(): boot address field carries a 2000 ms delay. */
    int rc = start_app(Dev, 2000, AIC_HOST_START_APP_REBOOT);

    return rc == AIC_E_TIMEOUT ? AIC_OK : rc;
}
