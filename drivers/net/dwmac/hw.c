/*++

Module Name:

    hw.c

Abstract:

    Synopsys DWMAC-4.20a hardware engine for the RK3576 GMAC. Verified against
    the kernel stmmac driver (dwmac4_core / dwmac4_dma / dwmac4_descs) and the
    rockchip dwmac glue (SDGMAC_GRF clock select).

    Datapath: bounce buffers. One TX and one RX DMA channel, fixed descriptor
    rings, fixed per-descriptor DMA buffers. Frame data is copied to/from those
    buffers (no per-packet DMA mapping in v1).

Environment:

    Kernel mode.

--*/

#include "dwmac.h"

//
// Standard clause-22 MII registers (PHY-agnostic link resolution).
//
#define MII_BMCR        0x00
#define MII_BMSR        0x01
#define MII_PHYSID1     0x02
#define MII_PHYSID2     0x03
#define MII_ADVERTISE   0x04
#define MII_LPA         0x05
#define MII_CTRL1000    0x09
#define MII_STAT1000    0x0A

#define BMSR_LSTATUS    0x0004
#define BMSR_ANEGCOMPLETE 0x0020

#define LPA_10HALF      0x0020
#define LPA_10FULL      0x0040
#define LPA_100HALF     0x0080
#define LPA_100FULL     0x0100
#define STAT1000_1000HALF 0x0400
#define STAT1000_1000FULL 0x0800
#define CTRL1000_ADV_1000HALF 0x0100
#define CTRL1000_ADV_1000FULL 0x0200

//
// MDIO clock range field. MDC = CSR_clk / divisor; CR=4 (CSR/102) keeps MDC
// well under the 2.5 MHz limit for a ~150 MHz CSR. CSR-clock dependent.
//
#define DWMAC_MDIO_CR   4

static
NTSTATUS
DwmacPoll(
    _In_ PDWMAC_ADAPTER A,
    _In_ ULONG Off,
    _In_ ULONG Mask,
    _In_ BOOLEAN WaitSet,
    _In_ ULONG TimeoutUs
    )
{
    LARGE_INTEGER start, freq, now;
    start = KeQueryPerformanceCounter(&freq);
    for (;;) {
        ULONG v = DwmacRead(A->Mac, Off) & Mask;
        if (WaitSet ? (v != 0) : (v == 0)) {
            return STATUS_SUCCESS;
        }
        now = KeQueryPerformanceCounter(NULL);
        if (((now.QuadPart - start.QuadPart) * 1000000LL / freq.QuadPart) >
            (LONGLONG)TimeoutUs) {
            return STATUS_IO_TIMEOUT;
        }
        KeStallExecutionProcessor(2);
    }
}

NTSTATUS
DwmacSwReset(
    _In_ PDWMAC_ADAPTER A
    )
{
    DwmacWrite(A->Mac, DMA_BUS_MODE,
               DwmacRead(A->Mac, DMA_BUS_MODE) | DMA_BUS_MODE_SFT_RESET);

    //
    // SFT_RESET self-clears once the reset (incl. all clock domains) completes.
    //
    return DwmacPoll(A, DMA_BUS_MODE, DMA_BUS_MODE_SFT_RESET, FALSE, 200000);
}

VOID
DwmacSetMacAddress(
    _In_ PDWMAC_ADAPTER A
    )
{
    ULONG hi = GMAC_ADDR_HIGH_AE |
               ((ULONG)A->MacAddress[4]) |
               ((ULONG)A->MacAddress[5] << 8);
    ULONG lo = ((ULONG)A->MacAddress[0]) |
               ((ULONG)A->MacAddress[1] << 8) |
               ((ULONG)A->MacAddress[2] << 16) |
               ((ULONG)A->MacAddress[3] << 24);

    DwmacWrite(A->Mac, GMAC_ADDR_HIGH0, hi);
    DwmacWrite(A->Mac, GMAC_ADDR_LOW0, lo);
}

NTSTATUS
DwmacMdioRead(
    _In_ PDWMAC_ADAPTER A,
    _In_ ULONG Reg,
    _Out_ PUSHORT Value
    )
{
    ULONG cmd;
    NTSTATUS status;

    *Value = 0;

    cmd = ((A->PhyAddr & 0x1F) << GMAC_MDIO_PA_SHIFT) |
          ((Reg & 0x1F) << GMAC_MDIO_RDA_SHIFT) |
          (DWMAC_MDIO_CR << GMAC_MDIO_CR_SHIFT) |
          (GMAC_MDIO_GOC_READ << GMAC_MDIO_GOC_SHIFT) |
          GMAC_MDIO_GB;

    DwmacWrite(A->Mac, GMAC_MDIO_ADDR, cmd);

    status = DwmacPoll(A, GMAC_MDIO_ADDR, GMAC_MDIO_GB, FALSE, DWMAC_MDIO_TIMEOUT_US);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *Value = (USHORT)(DwmacRead(A->Mac, GMAC_MDIO_DATA) & 0xFFFF);
    return STATUS_SUCCESS;
}

NTSTATUS
DwmacMdioWrite(
    _In_ PDWMAC_ADAPTER A,
    _In_ ULONG Reg,
    _In_ USHORT Value
    )
{
    ULONG cmd;

    DwmacWrite(A->Mac, GMAC_MDIO_DATA, Value);

    cmd = ((A->PhyAddr & 0x1F) << GMAC_MDIO_PA_SHIFT) |
          ((Reg & 0x1F) << GMAC_MDIO_RDA_SHIFT) |
          (DWMAC_MDIO_CR << GMAC_MDIO_CR_SHIFT) |
          (GMAC_MDIO_GOC_WRITE << GMAC_MDIO_GOC_SHIFT) |
          GMAC_MDIO_GB;

    DwmacWrite(A->Mac, GMAC_MDIO_ADDR, cmd);

    return DwmacPoll(A, GMAC_MDIO_ADDR, GMAC_MDIO_GB, FALSE, DWMAC_MDIO_TIMEOUT_US);
}

NTSTATUS
DwmacPhyDetect(
    _In_ PDWMAC_ADAPTER A
    )
{
    ULONG addr;

    for (addr = 0; addr < 32; addr += 1) {
        USHORT id1 = 0;
        A->PhyAddr = addr;
        if (NT_SUCCESS(DwmacMdioRead(A, MII_PHYSID1, &id1)) &&
            id1 != 0x0000 && id1 != 0xFFFF) {
            RkLog(RK_DBG_INFO, "PHY at addr %u (id1 0x%04x)\n", addr, id1);
            return STATUS_SUCCESS;
        }
    }

    A->PhyAddr = 0;
    RkLog(RK_DBG_ERROR, "no PHY found on MDIO\n");
    return STATUS_NO_SUCH_DEVICE;
}

//
// Program the SDGMAC_GRF clock selection for the negotiated speed (HIWORD-
// UPDATE: high 16 bits are the write mask).
//
static
VOID
DwmacSetGrfClock(
    _In_ PDWMAC_ADAPTER A,
    _In_ ULONG SpeedMbps
    )
{
    ULONG val;

    if (A->Grf == NULL) {
        return;
    }

    if (SpeedMbps >= 1000) {
        val = RK3576_GMAC_CLK_125M;
    } else if (SpeedMbps >= 100) {
        val = RK3576_GMAC_CLK_25M;
    } else {
        val = RK3576_GMAC_CLK_2_5M;
    }

    WRITE_REGISTER_ULONG((volatile ULONG *)(A->Grf + RK3576_GMAC0_CON0_OFFSET), val);
}

VOID
DwmacUpdateLink(
    _In_ PDWMAC_ADAPTER A
    )
{
    USHORT bmsr = 0, lpa = 0, stat1000 = 0;
    ULONG speed = 10;
    BOOLEAN full = FALSE;
    ULONG cfg;

    //
    // BMSR latches link-low; read twice for the current state.
    //
    (VOID)DwmacMdioRead(A, MII_BMSR, &bmsr);
    (VOID)DwmacMdioRead(A, MII_BMSR, &bmsr);

    if (!(bmsr & BMSR_LSTATUS)) {
        A->LinkUp = FALSE;
        RkLog(RK_DBG_INFO, "link down\n");
        return;
    }

    //
    // Resolve the negotiated speed/duplex from the standard clause-22 registers
    // (PHY-agnostic): 1000 status first, then the 10/100 link-partner ability.
    //
    (VOID)DwmacMdioRead(A, MII_STAT1000, &stat1000);
    (VOID)DwmacMdioRead(A, MII_LPA, &lpa);

    if (stat1000 & STAT1000_1000FULL) {
        speed = 1000; full = TRUE;
    } else if (stat1000 & STAT1000_1000HALF) {
        speed = 1000; full = FALSE;
    } else if (lpa & LPA_100FULL) {
        speed = 100; full = TRUE;
    } else if (lpa & LPA_100HALF) {
        speed = 100; full = FALSE;
    } else if (lpa & LPA_10FULL) {
        speed = 10; full = TRUE;
    } else {
        speed = 10; full = FALSE;
    }

    A->LinkUp = TRUE;
    A->LinkSpeedMbps = speed;
    A->FullDuplex = full;

    DwmacSetGrfClock(A, speed);

    //
    // MAC speed/duplex: PS=1 selects 10/100 (MII), FES=1 selects 100 within that;
    // gigabit uses PS=0/FES=0 (GMII).
    //
    cfg = DwmacRead(A->Mac, GMAC_CONFIG);
    cfg &= ~(GMAC_CONFIG_PS | GMAC_CONFIG_FES | GMAC_CONFIG_DM);
    if (speed == 100) {
        cfg |= GMAC_CONFIG_PS | GMAC_CONFIG_FES;
    } else if (speed == 10) {
        cfg |= GMAC_CONFIG_PS;
    }
    if (full) {
        cfg |= GMAC_CONFIG_DM;
    }
    DwmacWrite(A->Mac, GMAC_CONFIG, cfg);

    RkLog(RK_DBG_INFO, "link up %u Mbps %s-duplex\n", speed, full ? "full" : "half");
}

VOID
DwmacInitMacDma(
    _In_ PDWMAC_ADAPTER A
    )
{
    //
    // DMA system bus: mixed burst, address-aligned beats, a spread of burst
    // lengths.
    //
    DwmacWrite(A->Mac, DMA_SYS_BUS_MODE,
               DMA_SYS_BUS_MB | DMA_SYS_BUS_AAL |
               DMA_SYS_BUS_BLEN16 | DMA_SYS_BUS_BLEN8 | DMA_SYS_BUS_BLEN4);

    //
    // Base MAC config (speed/duplex filled in by DwmacUpdateLink). Strip CRC/pad
    // on receive of typed frames.
    //
    DwmacWrite(A->Mac, GMAC_CONFIG, GMAC_CONFIG_ACS | GMAC_CONFIG_CST);

    //
    // Accept broadcast + our unicast; enable RX queue 0 in DCB mode. (Multicast/
    // promiscuous handling is set later from the packet filter callback.)
    //
    DwmacWrite(A->Mac, GMAC_PACKET_FILTER, 0);
    DwmacWrite(A->Mac, GMAC_RXQ_CTRL0, GMAC_RXQ_CTRL0_EN_DCB);
}

VOID
DwmacInitRings(
    _In_ PDWMAC_ADAPTER A
    )
{
    ULONG i;

    //
    // TX descriptors: host-owned, empty.
    //
    for (i = 0; i < DWMAC_TX_RING; i += 1) {
        A->TxDesc[i].Des0 = 0;
        A->TxDesc[i].Des1 = 0;
        A->TxDesc[i].Des2 = 0;
        A->TxDesc[i].Des3 = 0;
    }
    A->TxHead = 0;
    A->TxTail = 0;

    //
    // RX descriptors: each points at its bounce buffer and is owned by the DMA.
    //
    for (i = 0; i < DWMAC_RX_RING; i += 1) {
        ULONGLONG pa = (ULONGLONG)A->RxBufPa.QuadPart + (ULONGLONG)i * DWMAC_BUF_SIZE;
        A->RxDesc[i].Des0 = (ULONG)(pa & 0xFFFFFFFF);
        A->RxDesc[i].Des1 = (ULONG)(pa >> 32);
        A->RxDesc[i].Des2 = 0;
        A->RxDesc[i].Des3 = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;
    }
    A->RxIndex = 0;

    //
    // Program the ring base/length/tail pointers (channel 0).
    //
    DwmacWrite(A->Mac, DMA_CHAN_TX_BASE_HI, (ULONG)(A->TxDescPa.QuadPart >> 32));
    DwmacWrite(A->Mac, DMA_CHAN_TX_BASE, (ULONG)(A->TxDescPa.QuadPart & 0xFFFFFFFF));
    DwmacWrite(A->Mac, DMA_CHAN_RX_BASE_HI, (ULONG)(A->RxDescPa.QuadPart >> 32));
    DwmacWrite(A->Mac, DMA_CHAN_RX_BASE, (ULONG)(A->RxDescPa.QuadPart & 0xFFFFFFFF));

    DwmacWrite(A->Mac, DMA_CHAN_TX_RING_LEN, DWMAC_TX_RING - 1);
    DwmacWrite(A->Mac, DMA_CHAN_RX_RING_LEN, DWMAC_RX_RING - 1);

    //
    // RX tail = last descriptor (all are armed); TX tail = base (ring empty).
    //
    DwmacWrite(A->Mac, DMA_CHAN_RX_END,
               (ULONG)((A->RxDescPa.QuadPart + (ULONGLONG)DWMAC_RX_RING * sizeof(DWMAC_DESC)) & 0xFFFFFFFF));
    DwmacWrite(A->Mac, DMA_CHAN_TX_END, (ULONG)(A->TxDescPa.QuadPart & 0xFFFFFFFF));

    //
    // RX buffer size in RX_CONTROL[14:1].
    //
    DwmacWrite(A->Mac, DMA_CHAN_RX_CONTROL,
               DwmacRead(A->Mac, DMA_CHAN_RX_CONTROL) |
               (DWMAC_BUF_SIZE << DMA_CHAN_RXCTRL_RBSZ_SHIFT));
}

VOID
DwmacStart(
    _In_ PDWMAC_ADAPTER A
    )
{
    //
    // Enable channel interrupts (normal summary + TX + RX + RX-buffer-unavail).
    //
    DwmacWrite(A->Mac, DMA_CHAN_INTR_ENA,
               DMA_CHAN_INTR_NIE | DMA_CHAN_INTR_AIE |
               DMA_CHAN_INTR_TIE | DMA_CHAN_INTR_RIE | DMA_CHAN_INTR_RBUE);

    //
    // Start the DMA channels, then the MAC TX/RX.
    //
    DwmacWrite(A->Mac, DMA_CHAN_TX_CONTROL,
               DwmacRead(A->Mac, DMA_CHAN_TX_CONTROL) | DMA_CHAN_TXCTRL_ST);
    DwmacWrite(A->Mac, DMA_CHAN_RX_CONTROL,
               DwmacRead(A->Mac, DMA_CHAN_RX_CONTROL) | DMA_CHAN_RXCTRL_SR);

    DwmacWrite(A->Mac, GMAC_CONFIG,
               DwmacRead(A->Mac, GMAC_CONFIG) | GMAC_CONFIG_TE | GMAC_CONFIG_RE);
}

VOID
DwmacStop(
    _In_ PDWMAC_ADAPTER A
    )
{
    DwmacWrite(A->Mac, GMAC_CONFIG,
               DwmacRead(A->Mac, GMAC_CONFIG) & ~(GMAC_CONFIG_TE | GMAC_CONFIG_RE));
    DwmacWrite(A->Mac, DMA_CHAN_TX_CONTROL,
               DwmacRead(A->Mac, DMA_CHAN_TX_CONTROL) & ~DMA_CHAN_TXCTRL_ST);
    DwmacWrite(A->Mac, DMA_CHAN_RX_CONTROL,
               DwmacRead(A->Mac, DMA_CHAN_RX_CONTROL) & ~DMA_CHAN_RXCTRL_SR);
    DwmacWrite(A->Mac, DMA_CHAN_INTR_ENA, 0);
}

ULONG
DwmacReadAndClearIrq(
    _In_ PDWMAC_ADAPTER A
    )
{
    ULONG sts = DwmacRead(A->Mac, DMA_CHAN_STATUS);

    //
    // The status bits are write-1-to-clear. Ack the ones we act on plus the
    // normal/abnormal summary bits so the line de-asserts.
    //
    DwmacWrite(A->Mac, DMA_CHAN_STATUS,
               sts & (DMA_CHAN_STATUS_TI | DMA_CHAN_STATUS_RI | DMA_CHAN_STATUS_RBU |
                      DMA_CHAN_STATUS_NIS | DMA_CHAN_STATUS_AIS));
    return sts;
}

BOOLEAN
DwmacTransmitFrame(
    _In_ PDWMAC_ADAPTER A,
    _In_reads_bytes_(Length) PUCHAR Frame,
    _In_ ULONG Length
    )
{
    ULONG idx = A->TxHead;
    ULONG next = (idx + 1) % DWMAC_TX_RING;
    PDWMAC_DESC d = &A->TxDesc[idx];
    ULONGLONG bufPa;

    if (Length == 0 || Length > DWMAC_BUF_SIZE) {
        return FALSE;
    }
    //
    // Ring full if the descriptor is still owned by the DMA or we would collide
    // with the reclaim tail.
    //
    if ((d->Des3 & TDES3_OWN) || next == A->TxTail) {
        return FALSE;
    }

    RtlCopyMemory(A->TxBuf + (SIZE_T)idx * DWMAC_BUF_SIZE, Frame, Length);
    bufPa = (ULONGLONG)A->TxBufPa.QuadPart + (ULONGLONG)idx * DWMAC_BUF_SIZE;

    d->Des0 = (ULONG)(bufPa & 0xFFFFFFFF);
    d->Des1 = (ULONG)(bufPa >> 32);
    d->Des2 = (Length & TDES2_B1L_MASK) | TDES2_IOC;
    //
    // OWN must be set last; a single-buffer frame is FD + LD with the frame
    // length in Des3.
    //
    d->Des3 = TDES3_OWN | TDES3_FD | TDES3_LD | (Length & TDES3_FL_MASK);

    A->TxHead = next;

    //
    // Poke the tail pointer to tell the DMA there is new work.
    //
    DwmacWrite(A->Mac, DMA_CHAN_TX_END,
               (ULONG)((A->TxDescPa.QuadPart + (ULONGLONG)A->TxHead * sizeof(DWMAC_DESC)) & 0xFFFFFFFF));
    return TRUE;
}

ULONG
DwmacReclaimTx(
    _In_ PDWMAC_ADAPTER A
    )
{
    ULONG count = 0;

    while (A->TxTail != A->TxHead) {
        PDWMAC_DESC d = &A->TxDesc[A->TxTail];
        if (d->Des3 & TDES3_OWN) {
            break;     // still owned by DMA
        }
        d->Des3 = 0;
        A->TxTail = (A->TxTail + 1) % DWMAC_TX_RING;
        count += 1;
    }
    return count;
}

BOOLEAN
DwmacReceiveFrame(
    _In_ PDWMAC_ADAPTER A,
    _Out_writes_bytes_(DstCap) PUCHAR Dst,
    _In_ ULONG DstCap,
    _Out_ PULONG Length
    )
{
    ULONG idx = A->RxIndex;
    PDWMAC_DESC d = &A->RxDesc[idx];
    ULONG des3 = d->Des3;
    ULONG len;
    ULONGLONG bufPa;

    *Length = 0;

    if (des3 & RDES3_OWN) {
        return FALSE;            // still owned by DMA — nothing ready
    }

    //
    // Completed descriptor. A good frame is FD+LD with no error summary.
    //
    len = des3 & RDES3_PL_MASK;
    if (!(des3 & RDES3_ES) && (des3 & RDES3_LD) && len > 0 && len <= DstCap) {
        RtlCopyMemory(Dst, A->RxBuf + (SIZE_T)idx * DWMAC_BUF_SIZE, len);
        *Length = len;
    } else {
        len = 0;                 // error / oversized — recycle silently
    }

    //
    // Re-arm the descriptor and advance.
    //
    bufPa = (ULONGLONG)A->RxBufPa.QuadPart + (ULONGLONG)idx * DWMAC_BUF_SIZE;
    d->Des0 = (ULONG)(bufPa & 0xFFFFFFFF);
    d->Des1 = (ULONG)(bufPa >> 32);
    d->Des2 = 0;
    d->Des3 = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;

    //
    // Hand the descriptor back to the DMA via the tail pointer.
    //
    DwmacWrite(A->Mac, DMA_CHAN_RX_END,
               (ULONG)((A->RxDescPa.QuadPart + (ULONGLONG)idx * sizeof(DWMAC_DESC)) & 0xFFFFFFFF));

    A->RxIndex = (idx + 1) % DWMAC_RX_RING;
    return (*Length != 0);
}
