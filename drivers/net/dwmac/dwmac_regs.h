/*++

Module Name:

    dwmac_regs.h

Abstract:

    Register layout for the Synopsys DesignWare MAC v4.20a ("GMAC4" / stmmac)
    as used by the RK3576 GMAC0/GMAC1, plus the RK3576 SDGMAC_GRF glue.
    Verified against the kernel stmmac driver
    (drivers/net/ethernet/stmicro/stmmac/dwmac4*.{c,h}).

    Layout: MAC registers at base+0x0000, MTL at +0x0d00, DMA at +0x1000, and
    per-channel DMA at +0x1100 (channel 0 only used here).

Environment:

    Kernel mode.

--*/

#pragma once

#define BIT_(_n) (1u << (_n))

//
// MAC registers.
//
#define GMAC_CONFIG             0x0000
#define GMAC_EXT_CONFIG         0x0004
#define GMAC_PACKET_FILTER      0x0008
#define GMAC_RXQ_CTRL0          0x00A0
#define GMAC_INT_STATUS         0x00B0
#define GMAC_INT_EN             0x00B4
#define GMAC_HW_FEATURE0        0x011C
#define GMAC_MDIO_ADDR          0x0200
#define GMAC_MDIO_DATA          0x0204
#define GMAC_ADDR_HIGH0         0x0300
#define GMAC_ADDR_LOW0          0x0304

// GMAC_CONFIG bits
#define GMAC_CONFIG_RE          BIT_(0)    // receiver enable
#define GMAC_CONFIG_TE          BIT_(1)    // transmitter enable
#define GMAC_CONFIG_DM          BIT_(13)   // full duplex
#define GMAC_CONFIG_FES         BIT_(14)   // 1 = 100 Mbps (with PS=1)
#define GMAC_CONFIG_PS          BIT_(15)   // port select: 1 = 10/100 (MII)
#define GMAC_CONFIG_JD          BIT_(17)   // jabber disable
#define GMAC_CONFIG_CST         BIT_(21)   // CRC strip for type
#define GMAC_CONFIG_ACS         BIT_(20)   // auto pad/CRC strip

// GMAC_RXQ_CTRL0: RXQ0 enable field [1:0], 2 = enabled (DCB/generic)
#define GMAC_RXQ_CTRL0_EN_DCB   (2u << 0)

// GMAC_ADDR_HIGH: address enable
#define GMAC_ADDR_HIGH_AE       BIT_(31)

// MDIO_ADDR bits
#define GMAC_MDIO_PA_SHIFT      21         // PHY address
#define GMAC_MDIO_RDA_SHIFT     16         // register/device address
#define GMAC_MDIO_CR_SHIFT      8          // clock range
#define GMAC_MDIO_GOC_SHIFT     2          // operation: 1=write, 3=read
#define GMAC_MDIO_GOC_WRITE     1
#define GMAC_MDIO_GOC_READ      3
#define GMAC_MDIO_C45E          BIT_(1)    // clause 45 enable (0 for C22)
#define GMAC_MDIO_GB            BIT_(0)    // busy

//
// DMA (global) registers.
//
#define DMA_BUS_MODE            0x1000
#define DMA_SYS_BUS_MODE        0x1004
#define DMA_STATUS              0x1008

#define DMA_BUS_MODE_SFT_RESET  BIT_(0)
#define DMA_SYS_BUS_AAL         BIT_(12)   // address-aligned beats
#define DMA_SYS_BUS_MB          BIT_(14)   // mixed burst
#define DMA_SYS_BUS_BLEN16      BIT_(3)
#define DMA_SYS_BUS_BLEN8       BIT_(2)
#define DMA_SYS_BUS_BLEN4       BIT_(1)

//
// Per-channel DMA registers (channel 0). base + 0x1100.
//
#define DMA_CHAN_BASE           0x1100
#define DMA_CHAN_CONTROL        (DMA_CHAN_BASE + 0x00)
#define DMA_CHAN_TX_CONTROL     (DMA_CHAN_BASE + 0x04)
#define DMA_CHAN_RX_CONTROL     (DMA_CHAN_BASE + 0x08)
#define DMA_CHAN_TX_BASE_HI     (DMA_CHAN_BASE + 0x10)
#define DMA_CHAN_TX_BASE        (DMA_CHAN_BASE + 0x14)
#define DMA_CHAN_RX_BASE_HI     (DMA_CHAN_BASE + 0x18)
#define DMA_CHAN_RX_BASE        (DMA_CHAN_BASE + 0x1C)
#define DMA_CHAN_TX_END         (DMA_CHAN_BASE + 0x20)
#define DMA_CHAN_RX_END         (DMA_CHAN_BASE + 0x28)
#define DMA_CHAN_TX_RING_LEN    (DMA_CHAN_BASE + 0x2C)
#define DMA_CHAN_RX_RING_LEN    (DMA_CHAN_BASE + 0x30)
#define DMA_CHAN_INTR_ENA       (DMA_CHAN_BASE + 0x34)
#define DMA_CHAN_CUR_TX_DESC    (DMA_CHAN_BASE + 0x44)
#define DMA_CHAN_CUR_RX_DESC    (DMA_CHAN_BASE + 0x4C)
#define DMA_CHAN_STATUS         (DMA_CHAN_BASE + 0x60)

#define DMA_CHAN_TXCTRL_ST      BIT_(0)    // start transmit
#define DMA_CHAN_RXCTRL_SR      BIT_(0)    // start receive
#define DMA_CHAN_RXCTRL_RBSZ_SHIFT  1      // receive buffer size [14:1]

#define DMA_CHAN_INTR_NIE       BIT_(15)   // normal interrupt summary enable
#define DMA_CHAN_INTR_AIE       BIT_(14)   // abnormal interrupt summary enable
#define DMA_CHAN_INTR_RBUE      BIT_(7)    // rx buffer unavailable
#define DMA_CHAN_INTR_RIE       BIT_(6)    // rx interrupt
#define DMA_CHAN_INTR_TIE       BIT_(0)    // tx interrupt

#define DMA_CHAN_STATUS_NIS     BIT_(15)
#define DMA_CHAN_STATUS_AIS     BIT_(14)
#define DMA_CHAN_STATUS_RBU     BIT_(7)
#define DMA_CHAN_STATUS_RI      BIT_(6)
#define DMA_CHAN_STATUS_TI      BIT_(0)

//
// dwmac4 descriptor (4 x 32-bit). Same layout for TX and RX; bit meaning
// differs by direction and read/write-back phase.
//
typedef struct _DWMAC_DESC {
    ULONG Des0;     // buffer address low
    ULONG Des1;     // buffer address high
    ULONG Des2;     // length / control
    ULONG Des3;     // OWN + flags
} DWMAC_DESC, *PDWMAC_DESC;

// TX (read/setup) bits
#define TDES2_IOC               BIT_(31)   // interrupt on completion
#define TDES2_B1L_MASK          0x3FFF     // buffer 1 length [13:0]
#define TDES3_OWN               BIT_(31)
#define TDES3_FD                BIT_(29)   // first descriptor
#define TDES3_LD                BIT_(28)   // last descriptor
#define TDES3_FL_MASK           0x7FFF     // frame length [14:0] (in FD)

// RX (setup) bits
#define RDES3_OWN               BIT_(31)
#define RDES3_IOC               BIT_(30)   // interrupt on completion
#define RDES3_BUF1V             BIT_(24)   // buffer 1 address valid
// RX (write-back) bits
#define RDES3_PL_MASK           0x7FFF     // packet length [14:0]
#define RDES3_ES                BIT_(15)   // error summary
#define RDES3_LD                BIT_(28)   // last descriptor
#define RDES3_FD                BIT_(29)   // first descriptor
#define RDES3_CTXT              BIT_(30)   // context descriptor

//
// RK3576 SDGMAC_GRF — GMAC clock-speed select (HIWORD-UPDATE: high 16 = mask).
// GMAC0 CON0 @ 0x26038020. Values from the EDK2 Gmac0.asl _DSM.
//
#define RK3576_SDGMAC_GRF_BASE      0x26038000
#define RK3576_SDGMAC_GRF_SIZE      0x100
#define RK3576_GMAC0_CON0_OFFSET    0x20
#define RK3576_GMAC_CLK_125M        0x00600000   // 1000 Mbps
#define RK3576_GMAC_CLK_25M         0x00600060   // 100 Mbps
#define RK3576_GMAC_CLK_2_5M        0x00600040   // 10 Mbps

//
// MMIO accessors.
//

FORCEINLINE
ULONG
DwmacRead(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

FORCEINLINE
VOID
DwmacWrite(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Value);
}
