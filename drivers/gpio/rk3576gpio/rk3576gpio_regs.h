/*++

Module Name:

    rk3576gpio_regs.h

Abstract:

    Register layout and low-level accessors for the Rockchip "GPIO v2"
    controller as found on RK3576 (also RK3562/RK3568/RK3588 share this IP).

    Register model (per 32-pin bank)
    --------------------------------
    Every logical 32-bit register is split into two 16-bit halves at
    consecutive 32-bit offsets:

        offset + 0x0   ->  pins  0..15  (low half)
        offset + 0x4   ->  pins 16..31  (high half)

    Writes use a "write-enable" scheme: the upper 16 bits of each half are a
    per-bit mask; a bit only takes effect if its mask bit (bit + 16) is set.
    This lets a pin be updated without a read-modify-write and without
    disturbing neighbouring pins.

        write half:   value = (data & 0xFFFF) | 0xFFFF0000
        write 1 bit:  value = (1 << (bit%16+16)) | (set ? (1 << (bit%16)) : 0)
        read  half:   data  = readl(half) & 0xFFFF

    Verified against mainline/vendor drivers/gpio/gpio-rockchip.c
    (gpio_regs_v2, gpio_writel_v2 / gpio_readl_v2).

Environment:

    Kernel mode.

--*/

#pragma once

//
// Logical register offsets (low half). Add 0x4 for the high half.
//
#define RK_GPIO_SWPORT_DR               0x0000  // data (output level)
#define RK_GPIO_SWPORT_DDR              0x0008  // direction (1 = output)
#define RK_GPIO_INT_EN                  0x0010  // interrupt enable (1 = enabled)
#define RK_GPIO_INT_MASK                0x0018  // interrupt mask (1 = masked)
#define RK_GPIO_INT_TYPE                0x0020  // 1 = edge, 0 = level
#define RK_GPIO_INT_POLARITY            0x0028  // 1 = high/rising, 0 = low/falling
#define RK_GPIO_INT_BOTHEDGE            0x0030  // 1 = both edges (edge type only)
#define RK_GPIO_DEBOUNCE                0x0038  // 1 = debounce enabled (per pin)
#define RK_GPIO_DBCLK_DIV_EN            0x0040  // per-pin debounce-clock divider en
#define RK_GPIO_DBCLK_DIV_CON           0x0048  // divider count (plain value)

//
// Default debounce-clock divider. The debounce clock is ~32 kHz; this gives a
// coarse (~ms-scale) filter, fine for mechanical buttons. Exact period is not
// critical and is not mapped from the requested timeout in v1.
//
#define RK_GPIO_DBCLK_DIV_DEFAULT      0x00000100
#define RK_GPIO_INT_STATUS              0x0050  // masked interrupt status
#define RK_GPIO_INT_RAWSTATUS           0x0058  // raw interrupt status
#define RK_GPIO_PORT_EOI                0x0060  // write 1 to clear an edge interrupt
#define RK_GPIO_EXT_PORT                0x0070  // input level (read-only)
#define RK_GPIO_VERSION_ID              0x0078  // controller version id

//
// VERSION_ID values that identify the v2 IP (informational only; we trust the
// ACPI _HID rather than probing this).
//
#define RK_GPIO_VER_V2                  0x01000C2B
#define RK_GPIO_VER_V2_1                0x0101157C
#define RK_GPIO_VER_V2_2                0x010219C8

//
// MMIO accessors. Base is the mapped controller window; Off is one of the
// RK_GPIO_* offsets above.
//

FORCEINLINE
VOID
RkGpioWrite32(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG Value
    )
{
    //
    // Split the 32-bit logical value across the two halves, setting the
    // write-enable mask for every bit in each half.
    //
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off),
                         (Value & 0xFFFF) | 0xFFFF0000);
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off + 4),
                         (Value >> 16) | 0xFFFF0000);
}

FORCEINLINE
ULONG
RkGpioRead32(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off
    )
{
    ULONG Lo = READ_REGISTER_ULONG((volatile ULONG *)(Base + Off)) & 0xFFFF;
    ULONG Hi = READ_REGISTER_ULONG((volatile ULONG *)(Base + Off + 4)) & 0xFFFF;
    return (Hi << 16) | Lo;
}

FORCEINLINE
VOID
RkGpioWriteBit(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG Bit,            // 0..31
    _In_ BOOLEAN Set
    )
{
    ULONG Shift = Bit & 0xF;                       // bit within the half (0..15)
    ULONG Data  = 1UL << (Shift + 16);             // write-enable mask
    if (Set) {
        Data |= 1UL << Shift;
    }
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off + ((Bit >= 16) ? 4 : 0)),
                         Data);
}

//
// Apply a set/clear pair to a split register using read-modify-write. Callers
// are serialized by GpioClx per controller, so RMW is safe here.
//
FORCEINLINE
VOID
RkGpioApplyMask32(
    _In_ volatile UCHAR *Base,
    _In_ ULONG Off,
    _In_ ULONG SetMask,
    _In_ ULONG ClearMask
    )
{
    ULONG Value = RkGpioRead32(Base, Off);
    Value = (Value & ~ClearMask) | SetMask;
    RkGpioWrite32(Base, Off, Value);
}
