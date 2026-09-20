/*++

Module Name:

    rkemmc_regs.h

Abstract:

    Register map for the RK3576 eMMC host, a Synopsys DesignWare Cores Mobile
    Storage Host (DWCMSHC).

    Unlike the SD slot's dw_mmc, this controller *is* SDHCI register
    compatible: offsets 0x000-0x0FF are the standard SD Host Controller
    Specification v3.00 layout. What it adds is a block of Rockchip vendor
    registers from 0x500 upwards, and those are the reason this driver has to
    exist at all -- an SDHCI software reset clears three of them and no inbox
    driver knows to put them back.

    Sources: SD Host Controller Simplified Specification 3.00 for the standard
    block; drivers/mmc/host/sdhci-of-dwcmshc.c (mainline) for the vendor block,
    cross-checked against the EDK2 driver in this project's firmware
    (Silicon/Rockchip/Drivers/DwcSdhciDxe).

Environment:

    Kernel mode.

--*/

#pragma once

//
// ---------------------------------------------------------------------------
// Standard SDHCI block (SDHCI 3.00). Widths matter: several of these are 8- or
// 16-bit registers and a 32-bit access to the wrong one has side effects on
// its neighbour.
// ---------------------------------------------------------------------------
//

#define SDHCI_DMA_ADDRESS           0x00    // 32
#define SDHCI_BLOCK_SIZE            0x04    // 16
#define SDHCI_BLOCK_COUNT           0x06    // 16
#define SDHCI_ARGUMENT              0x08    // 32
#define SDHCI_TRANSFER_MODE         0x0C    // 16
#define SDHCI_COMMAND               0x0E    // 16
#define SDHCI_RESPONSE              0x10    // 4 x 32
#define SDHCI_BUFFER                0x20    // 32
#define SDHCI_PRESENT_STATE         0x24    // 32
#define SDHCI_HOST_CONTROL          0x28    // 8
#define SDHCI_POWER_CONTROL         0x29    // 8
#define SDHCI_BLOCK_GAP_CONTROL     0x2A    // 8
#define SDHCI_WAKE_UP_CONTROL       0x2B    // 8
#define SDHCI_CLOCK_CONTROL         0x2C    // 16
#define SDHCI_TIMEOUT_CONTROL       0x2E    // 8
#define SDHCI_SOFTWARE_RESET        0x2F    // 8
#define SDHCI_INT_STATUS            0x30    // 16 (normal)
#define SDHCI_ERR_INT_STATUS        0x32    // 16
#define SDHCI_INT_ENABLE            0x34    // 16 (status enable, normal)
#define SDHCI_ERR_INT_ENABLE        0x36    // 16
#define SDHCI_SIGNAL_ENABLE         0x38    // 16 (signal enable, normal)
#define SDHCI_ERR_SIGNAL_ENABLE     0x3A    // 16
#define SDHCI_AUTO_CMD_STATUS       0x3C    // 16
#define SDHCI_HOST_CONTROL2         0x3E    // 16
#define SDHCI_CAPABILITIES          0x40    // 32
#define SDHCI_CAPABILITIES_1        0x44    // 32
#define SDHCI_ADMA_ADDRESS          0x58    // 64
#define SDHCI_HOST_VERSION          0xFE    // 16

//
// TRANSFER_MODE (0x0C)
//
#define SDHCI_TRNS_DMA              0x0001
#define SDHCI_TRNS_BLK_CNT_EN       0x0002
#define SDHCI_TRNS_AUTO_CMD12       0x0004
#define SDHCI_TRNS_AUTO_CMD23       0x0008
#define SDHCI_TRNS_READ             0x0010
#define SDHCI_TRNS_MULTI            0x0020

//
// COMMAND (0x0E)
//
#define SDHCI_CMD_RESP_NONE         0x00
#define SDHCI_CMD_RESP_LONG         0x01
#define SDHCI_CMD_RESP_SHORT        0x02
#define SDHCI_CMD_RESP_SHORT_BUSY   0x03
#define SDHCI_CMD_CRC               0x08
#define SDHCI_CMD_INDEX             0x10
#define SDHCI_CMD_DATA              0x20
#define SDHCI_MAKE_CMD(_i, _f)      ((USHORT)(((_i) << 8) | (_f)))

//
// PRESENT_STATE (0x24)
//
#define SDHCI_CMD_INHIBIT           0x00000001
#define SDHCI_DATA_INHIBIT          0x00000002
#define SDHCI_DOING_WRITE           0x00000100
#define SDHCI_DOING_READ            0x00000200
#define SDHCI_SPACE_AVAILABLE       0x00000400
#define SDHCI_DATA_AVAILABLE        0x00000800
#define SDHCI_CARD_PRESENT          0x00010000
#define SDHCI_WRITE_PROTECT         0x00080000

//
// HOST_CONTROL (0x28)
//
#define SDHCI_CTRL_LED              0x01
#define SDHCI_CTRL_4BITBUS          0x02
#define SDHCI_CTRL_HISPD            0x04
#define SDHCI_CTRL_DMA_MASK         0x18
#define SDHCI_CTRL_SDMA             0x00
#define SDHCI_CTRL_ADMA32           0x10
#define SDHCI_CTRL_8BITBUS          0x20

//
// POWER_CONTROL (0x29)
//
#define SDHCI_POWER_ON              0x01
#define SDHCI_POWER_180             0x0A
#define SDHCI_POWER_300             0x0C
#define SDHCI_POWER_330             0x0E

//
// CLOCK_CONTROL (0x2C)
//
#define SDHCI_CLOCK_INT_EN          0x0001
#define SDHCI_CLOCK_INT_STABLE      0x0002
#define SDHCI_CLOCK_CARD_EN         0x0004

//
// SOFTWARE_RESET (0x2F). BIT0 is the one that costs us the vendor registers.
//
#define SDHCI_RESET_ALL             0x01
#define SDHCI_RESET_CMD             0x02
#define SDHCI_RESET_DATA            0x04

//
// Normal interrupt status (0x30) and error interrupt status (0x32).
//
#define SDHCI_INT_RESPONSE          0x0001
#define SDHCI_INT_DATA_END          0x0002
#define SDHCI_INT_BLK_GAP           0x0004
#define SDHCI_INT_DMA_END           0x0008
#define SDHCI_INT_SPACE_AVAIL       0x0010
#define SDHCI_INT_DATA_AVAIL        0x0020
#define SDHCI_INT_CARD_INSERT       0x0040
#define SDHCI_INT_CARD_REMOVE       0x0080
#define SDHCI_INT_CARD_INT          0x0100
#define SDHCI_INT_RETUNE            0x1000
#define SDHCI_INT_ERROR             0x8000

#define SDHCI_ERR_CMD_TIMEOUT       0x0001
#define SDHCI_ERR_CMD_CRC           0x0002
#define SDHCI_ERR_CMD_END_BIT       0x0004
#define SDHCI_ERR_CMD_INDEX         0x0008
#define SDHCI_ERR_DATA_TIMEOUT      0x0010
#define SDHCI_ERR_DATA_CRC          0x0020
#define SDHCI_ERR_DATA_END_BIT      0x0040
#define SDHCI_ERR_CURRENT_LIMIT     0x0080
#define SDHCI_ERR_AUTO_CMD          0x0100
#define SDHCI_ERR_ADMA              0x0200

#define SDHCI_ERR_CMD_MASK          (SDHCI_ERR_CMD_TIMEOUT | SDHCI_ERR_CMD_CRC |   \
                                     SDHCI_ERR_CMD_END_BIT | SDHCI_ERR_CMD_INDEX)
#define SDHCI_ERR_DATA_MASK         (SDHCI_ERR_DATA_TIMEOUT | SDHCI_ERR_DATA_CRC | \
                                     SDHCI_ERR_DATA_END_BIT)

//
// HOST_CONTROL2 (0x3E) -- UHS mode select in bits [2:0].
//
#define SDHCI_CTRL_UHS_MASK         0x0007
#define SDHCI_CTRL_UHS_SDR12        0x0000
#define SDHCI_CTRL_UHS_SDR25        0x0001
#define SDHCI_CTRL_UHS_SDR50        0x0002
#define SDHCI_CTRL_UHS_SDR104       0x0003
#define SDHCI_CTRL_UHS_DDR50        0x0004
#define SDHCI_CTRL_HS400            0x0007    // Rockchip's non-standard encoding
#define SDHCI_CTRL_VDD_180          0x0008

//
// ---------------------------------------------------------------------------
// Rockchip vendor block.
//
// The three bits flagged below are what an SDHCI_RESET_ALL clears and what
// this driver has to restore.  Without them the controller is deaf: no
// internal clock, and the eMMC held in hardware reset.
// ---------------------------------------------------------------------------
//

#define DWCMSHC_HOST_CTRL3          0x508
#define DWCMSHC_EMMC_CTRL           0x52C
#define   EMMC_CTRL_CARD_IS_EMMC    0x0001   // bit 0  <-- cleared by RESET_ALL
#define   EMMC_CTRL_RST_N           0x0004   // bit 2  <-- cleared by RESET_ALL
#define DWCMSHC_AT_CTRL             0x540

#define DWCMSHC_EMMC_DLL_CTRL       0x800
#define   DLL_CTRL_START            0x00000001
#define   DLL_CTRL_SRST             0x00000002
#define   DLL_CTRL_START_POINT_DEF  (5 << 16)
#define   DLL_CTRL_INCREMENT_DEF    (2 << 8)
#define   DLL_CTRL_BYPASS           0x01000000   // bit 24, for <= 52 MHz
#define DWCMSHC_EMMC_DLL_RXCLK      0x804
#define   DLL_RXCLK_ORI_GATE        0x80000000   // bit 31
#define DWCMSHC_EMMC_DLL_TXCLK      0x808
#define DWCMSHC_EMMC_DLL_STRBIN     0x80C
#define   DLL_STRBIN_TAPNUM_DEFAULT 0x00000003
#define   DLL_STRBIN_DELAY_NUM_SEL  0x04000000   // bit 26
#define   DLL_STRBIN_DELAY_NUM_OFS  16
#define DWCMSHC_EMMC_DLL_CMDOUT     0x810
#define   DLL_CMDOUT_SRC_CLK_NEG    0x10000000   // bit 28
#define   DLL_CMDOUT_EN_SRC_CLK_NEG 0x20000000   // bit 29
#define   DLL_CMDOUT_TAPNUM_90_DEG  0x00000008

#define DWCMSHC_EMMC_MISC_CON       0x81C
#define   MISC_CON_INTCLK_EN        0x00000002   // bit 1  <-- cleared by RESET_ALL

#define DWCMSHC_EMMC_DLL_STATUS0    0x840
#define   DLL_STATUS0_DLL_LOCK      0x00000100
#define   DLL_STATUS0_DLL_TIMEOUT   0x00000200
#define DWCMSHC_EMMC_DLL_STATUS1    0x844

#define DLL_DLYENA                  0x08000000   // bit 27, shared by RX/TX/STRBIN
#define DLL_TAPNUM_FROM_SW          0x01000000   // bit 24
#define DLL_NO_INVERTER             0x20000000   // bit 29
#define DLL_TXCLK_TAPNUM_DEFAULT    0x00000010
#define DLL_TXCLK_TAPNUM_90_DEG     0x00000009

//
// RK3576: the STRBIN delay for the non-DLL (<= 52 MHz) path.  Per the vendor
// kernel's rk3576_drvdata.ddr50_strbin_delay_num; RK3588 uses 0x10.
//
#define RK3576_NONDLL_STRBIN_DELAY  0x0A

//
// ---------------------------------------------------------------------------
// CRU: CCLK_SRC_EMMC, outside this device's own register window.
//
// The card clock is not set through SDHCI's divider at all.  Mainline says so
// outright in sdhci-of-dwcmshc.c: "The sdclk frequency select bits in
// SDHCI_CLOCK_CONTROL are not functional on Rockchip's SDHCI implementation.
// Instead, the clock frequency is fully controlled via external clk provider."
// So the divider stays 0 (pass-through) and the rate comes from here.
//
//   bits [15:14]  mux   00 = gpll_400m, 01 = cpll_400m, 10 = xin_24m
//   bits [13:8]   divider - 1
//   bits [31:16]  write mask
//
// This mirrors the _DSM in the firmware's Emmc.asl exactly -- same register,
// same table.  The method is not reachable from an sdport miniport, which gets
// a mapped register window and no device handle to evaluate ACPI against, so
// the one register is mapped directly instead.
// ---------------------------------------------------------------------------
//

#define RK3576_CRU_CLKSEL_CON89     0x27200464
#define   CRU_EMMC_WRITE_MASK       0xFF000000   // enables bits [15:8]
#define   CRU_EMMC_MUX_GPLL_400M    (0u << 14)
#define   CRU_EMMC_MUX_CPLL_400M    (1u << 14)
#define   CRU_EMMC_MUX_XIN_24M      (2u << 14)
#define   CRU_EMMC_DIV(_n)          (((_n) - 1) << 8)

#define CRU_EMMC_PARENT_400M        400000000UL
#define CRU_EMMC_PARENT_24M         24000000UL
