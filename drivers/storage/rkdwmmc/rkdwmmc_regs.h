/*++

Module Name:

    rkdwmmc_regs.h

Abstract:

    Register layout for the Synopsys DesignWare Mobile Storage Host Controller
    (dw_mmc) as used for the RK3576 SD card slot (device-tree
    "rockchip,rk3576-dw-mshc" / "rockchip,rk3288-dw-mshc").

    This is NOT an SDHCI-standard controller — the register map and command/
    data model differ entirely. Verified against the kernel driver
    drivers/mmc/host/dw_mmc.{c,h}.

Environment:

    Kernel mode.

--*/

#pragma once

//
// Register offsets.
//
#define DWMMC_CTRL          0x000   // control
#define DWMMC_PWREN         0x004   // power enable (per-slot bit)
#define DWMMC_CLKDIV        0x008   // clock divider
#define DWMMC_CLKSRC        0x00C   // clock source mux
#define DWMMC_CLKENA        0x010   // clock enable
#define DWMMC_TMOUT         0x014   // data/response timeout
#define DWMMC_CTYPE         0x018   // card bus-width type
#define DWMMC_BLKSIZ        0x01C   // block size
#define DWMMC_BYTCNT        0x020   // byte count (total transfer)
#define DWMMC_INTMASK       0x024   // interrupt mask
#define DWMMC_CMDARG        0x028   // command argument
#define DWMMC_CMD           0x02C   // command
#define DWMMC_RESP0         0x030   // response 0..3
#define DWMMC_RESP1         0x034
#define DWMMC_RESP2         0x038
#define DWMMC_RESP3         0x03C
#define DWMMC_MINTSTS       0x040   // masked interrupt status
#define DWMMC_RINTSTS       0x044   // raw interrupt status (W1C)
#define DWMMC_STATUS        0x048   // status
#define DWMMC_FIFOTH        0x04C   // FIFO threshold / burst
#define DWMMC_CDETECT       0x050   // card detect (controller pin; unused here)
#define DWMMC_WRTPRT        0x054   // write protect
#define DWMMC_TCBCNT        0x05C   // transferred CIU byte count
#define DWMMC_TBBCNT        0x060   // transferred host byte count
#define DWMMC_DEBNCE        0x064   // debounce
#define DWMMC_USRID         0x068
#define DWMMC_VERID         0x06C   // version (selects FIFO data offset)
#define DWMMC_HCON          0x070   // hardware configuration
#define DWMMC_UHS_REG       0x074
#define DWMMC_RST_N         0x078   // card reset
#define DWMMC_BMOD          0x080   // bus mode (IDMAC)
#define DWMMC_PLDMND        0x084   // poll demand
#define DWMMC_DBADDR        0x088   // descriptor list base
#define DWMMC_IDSTS         0x08C   // IDMAC status
#define DWMMC_IDINTEN       0x090   // IDMAC interrupt enable
#define DWMMC_DSCADDR       0x094
#define DWMMC_BUFADDR       0x098

//
// FIFO data register. Legacy parts place it at 0x100, dw_mmc >= 0x240A at
// 0x200. Pick by VERID at init (see Rkdwmmc_FifoOffset).
//
#define DWMMC_DATA_OFFSET_LEGACY    0x100
#define DWMMC_DATA_OFFSET_V2        0x200
#define DWMMC_VERID_240A            0x5342240A  // boundary used by the kernel

//
// CTRL bits.
//
#define DWMMC_CTRL_RESET            (1u << 0)
#define DWMMC_CTRL_FIFO_RESET       (1u << 1)
#define DWMMC_CTRL_DMA_RESET        (1u << 2)
#define DWMMC_CTRL_INT_ENABLE       (1u << 4)
#define DWMMC_CTRL_DMA_ENABLE       (1u << 5)
#define DWMMC_CTRL_USE_IDMAC        (1u << 25)
#define DWMMC_CTRL_ALL_RESET        (DWMMC_CTRL_RESET | DWMMC_CTRL_FIFO_RESET | DWMMC_CTRL_DMA_RESET)

//
// CTYPE bus width.
//
#define DWMMC_CTYPE_1BIT            0
#define DWMMC_CTYPE_4BIT            (1u << 0)
#define DWMMC_CTYPE_8BIT           (1u << 16)

//
// CLKENA.
//
#define DWMMC_CLKENA_ENABLE        (1u << 0)
#define DWMMC_CLKENA_LOWPWR        (1u << 16)

//
// Interrupt / RINTSTS bits.
//
#define DWMMC_INT_CD               (1u << 0)   // card detect
#define DWMMC_INT_RESP_ERR         (1u << 1)
#define DWMMC_INT_CMD_DONE         (1u << 2)
#define DWMMC_INT_DATA_OVER        (1u << 3)
#define DWMMC_INT_TXDR             (1u << 4)
#define DWMMC_INT_RXDR             (1u << 5)
#define DWMMC_INT_RCRC             (1u << 6)
#define DWMMC_INT_DCRC             (1u << 7)
#define DWMMC_INT_RTO              (1u << 8)   // response timeout
#define DWMMC_INT_DRTO             (1u << 9)   // data read timeout
#define DWMMC_INT_HTO              (1u << 10)
#define DWMMC_INT_FRUN             (1u << 11)
#define DWMMC_INT_HLE              (1u << 12)
#define DWMMC_INT_SBE              (1u << 13)
#define DWMMC_INT_ACD              (1u << 14)  // auto command done
#define DWMMC_INT_EBE              (1u << 15)
#define DWMMC_INT_ERROR            0xBFC2u     // all error bits (matches kernel)

//
// CMD bits.
//
#define DWMMC_CMD_START            (1u << 31)
#define DWMMC_CMD_USE_HOLD_REG     (1u << 29)
#define DWMMC_CMD_UPD_CLK          (1u << 21)  // update clock registers only
#define DWMMC_CMD_INIT             (1u << 15)  // send 80-clock init sequence
#define DWMMC_CMD_STOP             (1u << 14)
#define DWMMC_CMD_PRV_DAT_WAIT     (1u << 13)
#define DWMMC_CMD_SEND_STOP        (1u << 12)  // auto STOP after data
#define DWMMC_CMD_DAT_WR           (1u << 10)  // write (else read)
#define DWMMC_CMD_DAT_EXP          (1u << 9)   // data transfer expected
#define DWMMC_CMD_RESP_CRC         (1u << 8)
#define DWMMC_CMD_RESP_LONG        (1u << 7)   // 136-bit response
#define DWMMC_CMD_RESP_EXP         (1u << 6)   // response expected
#define DWMMC_CMD_INDX(_n)         ((_n) & 0x1F)

//
// STATUS bits / helpers.
//
#define DWMMC_STATUS_DMA_REQ       (1u << 31)
#define DWMMC_STATUS_BUSY          (1u << 9)   // data busy (card)
#define DWMMC_STATUS_FIFO_COUNT(_x)  (((_x) >> 17) & 0x1FFF)

//
// FIFOTH: msize[30:28], rx watermark[27:16], tx watermark[11:0].
//
#define DWMMC_FIFOTH(_m, _rx, _tx) \
    ((((_m) & 0x7) << 28) | (((_rx) & 0xFFF) << 16) | ((_tx) & 0xFFF))

//
// MMIO accessors.
//

FORCEINLINE
ULONG
DwmmcRead(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

FORCEINLINE
VOID
DwmmcWrite(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Value);
}
