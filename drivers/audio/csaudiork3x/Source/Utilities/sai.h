/*
 * Rockchip SAI (Serial Audio Interface), the RK3576 audio block.
 *
 * Offsets, bit positions and reset values from the RK3576 TRM V1.2 Part 1,
 * chapter 24 (SAI).  RK3576's SAI reports SAI_VERSION 0x23073576.
 *
 * Fields are N-1 encoded where the TRM says so (VDW, SBW, SNB, CSR, FW,
 * FPW, MDIV).  Only what csaudiork3x uses is defined.
 */
#ifndef _ROCKCHIP_SAI_REGS_H
#define _ROCKCHIP_SAI_REGS_H

#define SAI_TXCR        0x0000
#define SAI_FSCR        0x0004
#define SAI_RXCR        0x0008
#define SAI_MONO_CR     0x000c
#define SAI_XFER        0x0010
#define SAI_CLR         0x0014
#define SAI_CKR         0x0018
#define SAI_TXFIFOLR    0x001c
#define SAI_DMACR       0x0024
#define SAI_INTCR       0x0028
#define SAI_INTSR       0x002c
#define SAI_TXDR        0x0030
#define SAI_PATH_SEL    0x0038
#define SAI_TX_SHIFT    0x0064
#define SAI_STATUS      0x006c
#define SAI_VERSION     0x0070

/* TXCR: transmit control.  Writable only while XFER.CLK is 0. */
#define SAI_XCR_EDGE_SHIFT_1    (1u << 22)          /* delay data to the FS position */
#define SAI_XCR_CSR(lanes)      (((lanes) - 1u) << 20)
#define SAI_XCR_SJM_L           (1u << 19)
#define SAI_XCR_FBM_LSB         (1u << 18)
#define SAI_XCR_SNB(slots)      (((slots) - 1u) << 11)
#define SAI_XCR_VDJ_L           (1u << 10)
#define SAI_XCR_SBW(bits)       (((bits) - 1u) << 5)
#define SAI_XCR_VDW(bits)       (((bits) - 1u) << 0)

/* FSCR: frame sync. */
#define SAI_FSCR_EDGE_DUAL      (1u << 24)
#define SAI_FSCR_FPW(bits)      (((bits) - 1u) << 12)
#define SAI_FSCR_FW(bits)       (((bits) - 1u) << 0)

/* XFER: transfer start. */
#define SAI_XFER_TXS            (1u << 2)
#define SAI_XFER_FSS            (1u << 1)
#define SAI_XFER_CLK            (1u << 0)

/* CLR: self-clearing logic clear. */
#define SAI_CLR_RXC             (1u << 1)
#define SAI_CLR_TXC             (1u << 0)

/* CKR: clock generation.  MDIV, MSS writable only while XFER.CLK is 0. */
#define SAI_CKR_MDIV(div)       (((div) - 1u) << 3)  /* sclk = mclk / div */
#define SAI_CKR_MSS_SLAVE       (1u << 2)            /* 0 = master, sclk out */
#define SAI_CKR_CKP_INVERTED    (1u << 1)
#define SAI_CKR_FSP_HIGH        (1u << 0)            /* 0 = frame sync low active */

/* DMACR */
#define SAI_DMACR_TDE           (1u << 8)
#define SAI_DMACR_TDL(level)    ((level) << 0)       /* request at or below level */
#define SAI_DMACR_TDL_MASK      0x1fu

/* INTCR / INTSR */
#define SAI_INTCR_TXUIC         (1u << 2)            /* write 1 to clear */
#define SAI_INTCR_TXUIE         (1u << 1)
#define SAI_INTSR_TXUI_ACT      (1u << 1)

/* TX_SHIFT: data delay from the FS edge, in half sclk cycles. */
#define SAI_XSHIFT_RIGHT(half)  ((half) << 0)

/* STATUS (SAI_VERSION 0x2307xxxx): 1 = idle. */
#define SAI_STATUS_TX_IDLE      (1u << 2)
#define SAI_STATUS_FS_IDLE      (1u << 1)

#endif
