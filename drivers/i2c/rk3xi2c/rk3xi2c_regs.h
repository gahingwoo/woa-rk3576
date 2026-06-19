/*++

Module Name:

    rk3xi2c_regs.h

Abstract:

    Register layout for the Rockchip "rk3x" I2C controller (compatible
    "rockchip,rk3576-i2c" / "rockchip,rk3399-i2c"). This is Rockchip's own I2C
    IP, NOT the Synopsys DesignWare I2C — the register map and transfer model
    are entirely different.

    Verified against the mainline/vendor kernel driver
    drivers/i2c/busses/i2c-rk3x.c.

    Transfer model
    --------------
    The controller is a master engine with four modes selected in REG_CON:
        MOD_TX (0)          transmit bytes from TXDATA (first byte = addr<<1|W)
        MOD_REGISTER_TX (1) send register addr then restart
        MOD_RX (2)          receive bytes into RXDATA; addr taken from MRXADDR
        MOD_REGISTER_RX (3) (errata, unused)
    Software sets START to begin (a repeated start if no STOP was issued since
    the last transfer), writes MTXCNT/MRXCNT to move a <=32-byte chunk, waits
    for the MBTF (tx) / MBRF (rx) interrupt-pending bit, then issues STOP.

Environment:

    Kernel mode.

--*/

#pragma once

//
// Register offsets.
//
#define RK3X_REG_CON            0x0000  // control
#define RK3X_REG_CLKDIV         0x0004  // SCL clock divider (low | high<<16)
#define RK3X_REG_MRXADDR        0x0008  // slave address (for RX / register modes)
#define RK3X_REG_MRXRADDR       0x000C  // slave register address
#define RK3X_REG_MTXCNT         0x0010  // bytes to transmit (write triggers TX)
#define RK3X_REG_MRXCNT         0x0014  // bytes to receive  (write triggers RX)
#define RK3X_REG_IEN            0x0018  // interrupt enable
#define RK3X_REG_IPD            0x001C  // interrupt pending / disable (W1C)
#define RK3X_REG_FCNT           0x0020  // finished byte count
#define RK3X_REG_TXDATA_BASE    0x0100  // 8 x 32-bit TX FIFO words (32 bytes)
#define RK3X_REG_RXDATA_BASE    0x0200  // 8 x 32-bit RX FIFO words (32 bytes)

#define RK3X_FIFO_MAX           32      // bytes per chunk

//
// REG_CON bits.
//
#define RK3X_CON_EN             (1u << 0)
#define RK3X_CON_MOD_SHIFT      1
#define RK3X_CON_MOD_MASK       (3u << 1)
#define RK3X_CON_MOD(_m)        (((_m) << RK3X_CON_MOD_SHIFT) & RK3X_CON_MOD_MASK)
#define RK3X_CON_START          (1u << 3)
#define RK3X_CON_STOP           (1u << 4)
#define RK3X_CON_LASTACK        (1u << 5)  // 1: NACK after last received byte
#define RK3X_CON_ACTACK         (1u << 6)  // 1: abort transfer if NACK received

#define RK3X_MOD_TX             0
#define RK3X_MOD_REGISTER_TX    1
#define RK3X_MOD_RX             2
#define RK3X_MOD_REGISTER_RX    3

//
// REG_MRXADDR: low byte is the slave address; VALID(n) marks byte n present.
//
#define RK3X_MRXADDR_VALID(_x)  (1u << (24 + (_x)))

//
// REG_IEN / REG_IPD bits.
//
#define RK3X_INT_BTF            (1u << 0)  // byte transfer finished
#define RK3X_INT_BRF            (1u << 1)  // byte receive finished
#define RK3X_INT_MBTF           (1u << 2)  // master data transmit finished
#define RK3X_INT_MBRF           (1u << 3)  // master data receive finished
#define RK3X_INT_START          (1u << 4)  // START generated
#define RK3X_INT_STOP           (1u << 5)  // STOP generated
#define RK3X_INT_NAKRCV         (1u << 6)  // NACK received
#define RK3X_INT_ALL            0x7Fu

//
// MMIO accessors (plain 32-bit; no GPIO-style write-enable mask here).
//

FORCEINLINE
ULONG
Rk3xRead(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off
    )
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

FORCEINLINE
VOID
Rk3xWrite(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Value);
}
