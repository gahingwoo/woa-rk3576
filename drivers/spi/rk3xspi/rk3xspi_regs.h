/*++

Module Name:

    rk3xspi_regs.h

Abstract:

    Register layout for the Rockchip SPI controller (device-tree
    "rockchip,rk3576-spi" / "rockchip,rk3066-spi"). This is Rockchip's own SPI
    master (loosely DW-SSI-derived but with its own register map). Verified
    against the kernel driver drivers/spi/spi-rockchip.c.

Environment:

    Kernel mode.

--*/

#pragma once

//
// Register offsets.
//
#define RKSPI_CTRLR0        0x0000  // control 0 (frame format, mode, xfer mode)
#define RKSPI_CTRLR1        0x0004  // control 1 (number of data frames - 1)
#define RKSPI_SSIENR        0x0008  // enable
#define RKSPI_SER           0x000C  // slave (chip) select enable
#define RKSPI_BAUDR         0x0010  // baud rate divider (even)
#define RKSPI_TXFTLR        0x0014  // TX FIFO threshold
#define RKSPI_RXFTLR        0x0018  // RX FIFO threshold
#define RKSPI_TXFLR         0x001C  // TX FIFO level
#define RKSPI_RXFLR         0x0020  // RX FIFO level
#define RKSPI_SR            0x0024  // status
#define RKSPI_IPR           0x0028
#define RKSPI_IMR           0x002C  // interrupt mask
#define RKSPI_ISR           0x0030
#define RKSPI_RISR          0x0034
#define RKSPI_ICR           0x0038  // interrupt clear
#define RKSPI_DMACR         0x003C
#define RKSPI_DMATDLR       0x0040
#define RKSPI_DMARDLR       0x0044
#define RKSPI_VERSION       0x0048
#define RKSPI_TXDR          0x0400  // TX data
#define RKSPI_RXDR          0x0800  // RX data

//
// CTRLR0 fields (bit offsets).
//
#define RKSPI_CR0_DFS_8BIT      (0x1u << 0)   // data frame size = 8 bit
#define RKSPI_CR0_SCPH          (1u << 6)     // clock phase (CPHA)
#define RKSPI_CR0_SCPOL         (1u << 7)     // clock polarity (CPOL)
#define RKSPI_CR0_CSM_KEEP      (0u << 8)     // keep CS asserted between frames
#define RKSPI_CR0_FBM_MSB       (0u << 12)    // MSB first
#define RKSPI_CR0_FRF_SPI       (0u << 16)    // Motorola SPI frame format
#define RKSPI_CR0_XFM_TR        (0u << 18)    // transmit & receive (full duplex)
#define RKSPI_CR0_XFM_TO        (1u << 18)    // transmit only
#define RKSPI_CR0_XFM_RO        (2u << 18)    // receive only
#define RKSPI_CR0_OPM_MASTER    (0u << 20)    // master mode

//
// SR (status) bits.
//
#define RKSPI_SR_BUSY           (1u << 0)
#define RKSPI_SR_TF_FULL        (1u << 1)
#define RKSPI_SR_TF_EMPTY       (1u << 2)
#define RKSPI_SR_RF_EMPTY       (1u << 3)
#define RKSPI_SR_RF_FULL        (1u << 4)

//
// SSIENR / SER.
//
#define RKSPI_SSIENR_ENABLE     (1u << 0)
#define RKSPI_SER_CS(_n)        (1u << (_n))

//
// BAUDR limits (even divider): sclk = spi_input_clk / BAUDR.
//
#define RKSPI_BAUDR_MIN         2
#define RKSPI_BAUDR_MAX         65534

//
// FIFO depth (frames). RK3576 SPI v2 has a 64-entry FIFO; 32 is the safe
// minimum across variants and is what we pump against.
//
#define RKSPI_FIFO_DEPTH        32

#define RKSPI_MAX_FRAMES        0xFFFF   // CTRLR1 is 16-bit

//
// MMIO accessors.
//

FORCEINLINE
ULONG
RkSpiRead(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

FORCEINLINE
VOID
RkSpiWrite(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Value);
}
