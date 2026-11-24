/*
 * bwl_regs.h - SDIO, backplane and firmware protocol constants for
 * Broadcom FullMAC chips, as used by bcmwlsdio.
 *
 * The values are those of OpenBSD's bwfm (sys/dev/ic/bwfmreg.h and
 * sys/dev/sdmmc/if_bwfm_sdio.h), whose notice follows, cross-checked
 * against Linux brcmfmac (ISC, Broadcom). Names were shortened.
 *
 * Copyright (c) 2010-2016 Broadcom Corporation
 * Copyright (c) 2016,2017 Patrick Wildt <patrick@blueri.se>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Changes for bcmwlsdio: Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>,
 * distributed under GPL-2.0-only with the rest of this driver.
 */

#pragma once

/* ------------------------------------------------------------ SDIO CCCR (function 0) */
#define CCCR_REV                0x00    /* [3:0] CCCR, [7:4] SDIO spec */
#define CCCR_SDREV              0x01
#define CCCR_IOEX               0x02    /* I/O enable, bit n = function n */
#define CCCR_IORX               0x03    /* I/O ready */
#define CCCR_IENX               0x04    /* interrupt enable, bit 0 = master */
#define CCCR_INTX               0x05    /* interrupt pending */
#define CCCR_ABORT              0x06    /* [2:0] function to abort */
#define CCCR_BUSIF              0x07
#define CCCR_CAPS               0x08
#define CCCR_CIS0               0x09    /* common CIS pointer, 3 bytes */
#define CCCR_BLKSIZE0           0x10    /* function 0 block size */
#define CCCR_SPEED              0x13

/* Function basic registers: function n at n * 0x100. */
#define FBR_BASE(fn)            ((u32)(fn) * 0x100u)
#define FBR_CIS0                0x09
#define FBR_BLKSIZE0            0x10
#define FBR_BLKSIZE1            0x11

/* Broadcom vendor CCCR bytes */
#define CCCR_BRCM_CARDCAP               0xF0
#define  CARDCAP_CMD14_SUPPORT          0x02
#define  CARDCAP_CMD14_EXT              0x04
#define  CARDCAP_CMD_NODEC              0x08
#define CCCR_BRCM_CARDCTRL              0xF1
#define  CARDCTRL_WLANRESET             0x02
#define CCCR_BRCM_SEPINT                0xF2
#define  SEPINT_MASK                    0x01
#define  SEPINT_OE                      0x02
#define  SEPINT_ACT_HI                  0x04

/* ------------------------------------------------------------ function 1 registers */
#define F1_WATERMARK            0x10008
#define F1_DEVICE_CTL           0x10009
#define  DEVCTL_CA_INT_ONLY             0x04
#define F1_SBADDRLOW            0x1000A
#define F1_SBADDRMID            0x1000B
#define F1_SBADDRHIGH           0x1000C
#define F1_FRAMECTRL            0x1000D
#define  FRAMECTRL_RF_TERM              0x01    /* read frame terminate */
#define  FRAMECTRL_WF_TERM              0x02
#define F1_CHIPCLKCSR           0x1000E
#define  CLKCSR_FORCE_ALP               0x01
#define  CLKCSR_FORCE_HT                0x02
#define  CLKCSR_FORCE_ILP               0x04
#define  CLKCSR_ALP_AVAIL_REQ           0x08
#define  CLKCSR_HT_AVAIL_REQ            0x10
#define  CLKCSR_FORCE_HW_CLKREQ_OFF     0x20
#define  CLKCSR_ALP_AVAIL               0x40
#define  CLKCSR_HT_AVAIL                0x80
#define F1_SDIOPULLUP           0x1000F
#define F1_RFRAMEBCLO           0x10019
#define F1_RFRAMEBCHI           0x1001A
#define F1_WAKEUPCTRL           0x1001E
#define  WAKEUPCTRL_HTWAIT              0x02
#define F1_SLEEPCSR             0x1001F
#define  SLEEPCSR_KSO                   0x01
#define  SLEEPCSR_DEVON                 0x02

#define CLK_ALPAV(v)            (((v) & (CLKCSR_ALP_AVAIL | CLKCSR_HT_AVAIL)) != 0)
#define CLK_HTAV(v)             (((v) & (CLKCSR_ALP_AVAIL | CLKCSR_HT_AVAIL)) == (CLKCSR_ALP_AVAIL | CLKCSR_HT_AVAIL))
#define CLK_AV(v, alponly)      (CLK_ALPAV(v) && ((alponly) || CLK_HTAV(v)))

/* Backplane access through function 1: a 32 KB window. */
#define SB_WINDOW_SIZE          0x08000u
#define SB_WINDOW_MASK          0x07FFFu
#define SB_ACCESS_4B            0x08000u

/* ------------------------------------------------------------ backplane (chipcommon) */
#define CHIP_BASE               0x18000000u
#define CC_CHIPID               0x000
#define  CHIPID_ID(x)                   ((x) & 0xFFFFu)
#define  CHIPID_REV(x)                  (((x) >> 16) & 0xFu)
#define  CHIPID_TYPE(x)                 (((x) >> 28) & 0xFu)
#define  CHIPID_TYPE_SOCI_SB            0
#define  CHIPID_TYPE_SOCI_AI            1
#define CC_CAPS                 0x004
#define  CC_CAPS_PMU                    0x10000000u
#define CC_CAPS_EXT             0x0AC
#define  CC_CAPS_EXT_AOB                0x00000040u
#define CC_EROMPTR              0x0FC
#define CC_PMUCONTROL           0x600
#define  PMUCONTROL_RES_SHIFT           13
#define  PMUCONTROL_RES_RELOAD          0x2u
#define CC_PMUCAPS              0x604
#define  PMUCAPS_REV_MASK               0xFFu
#define CC_PMUCAPS_EXT          0x64C
#define  PMUCAPS_EXT_SR_SUPP            0x02u
#define CC_CHIPCTL_ADDR         0x650
#define CC_CHIPCTL_DATA         0x654
#define CC_RETENTION_CTL        0x670
#define  RETENTION_MACPHY_DIS           (1u << 26)
#define  RETENTION_LOGIC_DIS            (1u << 27)

/* Chip IDs this driver names firmware for */
#define CHIP_4345               0x4345

/* Core IDs (EROM part numbers) */
#define CORE_CHIPCOMMON         0x800
#define CORE_INTERNAL_MEM       0x80E
#define CORE_80211              0x812
#define CORE_PMU                0x827
#define CORE_SDIO_DEV           0x829
#define CORE_ARM_CM3            0x82A
#define CORE_ARM_CR4            0x83E
#define CORE_GCI                0x840
#define CORE_ARM_CA7            0x847
#define CORE_SYS_MEM            0x849

/* AI wrapper ("agent") registers, relative to a core's wrapper base */
#define AGENT_IOCTL             0x0408
#define  IOCTL_CLK                      0x0001
#define  IOCTL_FGC                      0x0002
#define  IOCTL_ARMCR4_CPUHALT           0x0020
#define  IOCTL_D11_PHYCLOCKEN           0x0004
#define  IOCTL_D11_PHYRESET             0x0008
#define AGENT_RESET_CTL         0x0800
#define  RESET_CTL_RESET                0x0001

/* ARM CR4 core registers */
#define ARMCR4_CAP              0x0004
#define  ARMCR4_CAP_TCBANB(x)           ((x) & 0xFu)
#define  ARMCR4_CAP_TCBBNB(x)           (((x) >> 4) & 0xFu)
#define ARMCR4_BANKIDX          0x0040
#define ARMCR4_BANKINFO         0x0044
#define  ARMCR4_BANKINFO_BSZ(x)         ((x) & 0x7Fu)
#define  ARMCR4_BANKINFO_BLK_1K         0x200u

/* EROM descriptors */
#define DMP_DESC_MASK           0x0000000Fu
#define DMP_DESC_COMPONENT      0x00000001u
#define DMP_DESC_MASTER_PORT    0x00000003u
#define DMP_DESC_ADDRESS        0x00000005u
#define DMP_DESC_ADDRSIZE_GT32  0x00000008u
#define DMP_DESC_EOT            0x0000000Fu
#define DMP_COMP_PARTNUM(x)     (((x) & 0x000FFF00u) >> 8)
#define DMP_COMP_REVISION(x)    (((x) & 0xFF000000u) >> 24)
#define DMP_COMP_NUM_SWRAP(x)   (((x) & 0x00F80000u) >> 19)
#define DMP_COMP_NUM_MWRAP(x)   (((x) & 0x0007C000u) >> 14)
#define DMP_SLAVE_ADDR_BASE     0xFFFFF000u
#define DMP_SLAVE_TYPE(x)       (((x) & 0x000000C0u) >> 6)
#define  DMP_SLAVE_TYPE_SLAVE           0
#define  DMP_SLAVE_TYPE_SWRAP           2
#define  DMP_SLAVE_TYPE_MWRAP           3
#define DMP_SLAVE_SIZE_TYPE(x)  (((x) & 0x00000030u) >> 4)
#define  DMP_SLAVE_SIZE_4K              0
#define  DMP_SLAVE_SIZE_8K              1
#define  DMP_SLAVE_SIZE_DESC            3

/* ------------------------------------------------------------ SDIO device core */
#define SDPCM_PROT_VERSION              4
#define SDPCM_PROT_VERSION_SHIFT        16

#define SDCORE_INTSTATUS        0x020
#define  INTSTATUS_HMB_FC_STATE         (1u << 4)
#define  INTSTATUS_HMB_FC_CHANGE        (1u << 5)
#define  INTSTATUS_HMB_FRAME_IND        (1u << 6)
#define  INTSTATUS_HMB_HOST_INT         (1u << 7)
#define  INTSTATUS_HMB_SW_MASK          0x000000F0u
#define  INTSTATUS_CHIPACTIVE           (1u << 29)
#define SDCORE_HOSTINTMASK      0x024
#define SDCORE_TOSBMAILBOX      0x040
#define  TOSBMAILBOX_INT_ACK            (1u << 1)
#define SDCORE_TOSBMAILBOXDATA  0x048
#define SDCORE_TOHOSTMAILBOXDATA 0x04C
#define  TOHOST_NAKHANDLED              (1u << 0)
#define  TOHOST_DEVREADY                (1u << 1)
#define  TOHOST_FC                      (1u << 2)
#define  TOHOST_FWREADY                 (1u << 3)
#define  TOHOST_FWHALT                  (1u << 4)

#define F2_WATERMARK_DEFAULT    8

/* sdpcm_shared, at the address stored in the last word of RAM */
#define SHARED_FLAGS            0x00
#define  SHARED_VERSION_MASK            0x00FFu
#define  SHARED_ASSERT                  0x0200u
#define  SHARED_TRAP                    0x0400u
#define SHARED_TRAP_ADDR        0x04
#define SHARED_CONSOLE_ADDR     0x14
#define SHARED_SIZE             0x40

/* rte_console: the log buffer descriptor is at offset 8 */
#define CONSOLE_LOG_BUF         0x08
#define CONSOLE_LOG_BUFSZ       0x0C
#define CONSOLE_LOG_IDX         0x10

/* ------------------------------------------------------------ SDPCM framing */
#define SDPCM_CH_CONTROL        0x00
#define SDPCM_CH_EVENT          0x01
#define SDPCM_CH_DATA           0x02
#define SDPCM_CH_GLOM           0x03
#define SDPCM_CH_MASK           0x0F

/* ------------------------------------------------------------ BCDC */
#define BCDC_DCMD_ERROR         (1u << 0)
#define BCDC_DCMD_SET           (1u << 1)
#define BCDC_DCMD_IF(x)         (((u32)(x) & 0xFu) << 12)
#define BCDC_DCMD_ID(x)         (((u32)(x) & 0xFFFFu) << 16)
#define BCDC_DCMD_ID_GET(x)     (((x) >> 16) & 0xFFFFu)
#define BCDC_DCMD_MAXLEN        8192u

#define BCDC_HDR_LEN            4u
#define BCDC_PROTO_VER          2
#define BCDC_FLAG_VER(x)        (((x) & 0xF) << 4)
#define BCDC_FLAG2_IF_MASK      0xF

/* Firmware commands */
#define C_GET_VERSION           1
#define C_UP                    2
#define C_DOWN                  3
#define C_GET_VAR               262
#define C_SET_VAR               263

/* Firmware events */
#define ETHERTYPE_BRCM          0x886C
#define BRCM_SUBTYPE_EVENT      1
#define E_SET_SSID              0
#define E_LINK                  16
#define E_IF                    54
#define E_ESCAN_RESULT          69

/* Downloads ("clmload") */
#define DLOAD_FLAG_BEGIN        (1u << 1)
#define DLOAD_FLAG_END          (1u << 2)
#define DLOAD_FLAG_HANDLER_VER  (1u << 12)
#define DLOAD_TYPE_CLM          2
#define DLOAD_HDR_LEN           12u
#define BWL_DLOAD_MAX           1400u
