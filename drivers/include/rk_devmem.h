/*
 * rk_devmem.h - touch uncached DMA memory with naturally aligned accesses only.
 *
 * Windows 10 (19041 to 19045) maps PAGE_NOCACHE / MmNonCached RAM, and so the
 * common buffer of every non-coherent device (_CCA 0, all of RK3576; the HAL
 * asks for PAGE_READWRITE | PAGE_NOCACHE on both), with MAIR index 1, which
 * behaves as Device memory. Windows 11 moved MM_NOCACHE to index 2
 * (MmProtectToPteMask of the two kernels), which takes misaligned accesses
 * and DC ZVA, so code that has always worked there faults on Windows 10:
 *   - any misaligned access raises STATUS_DATATYPE_MISALIGNMENT (bugcheck 0x7E
 *     in dwcsdhc and stornvme, measured over KD on 19045);
 *   - RtlZeroMemory / memset clears large blocks with DC ZVA, which faults on
 *     Device memory at any alignment;
 *   - RtlCopyMemory / memcpy covers a head and a tail with overlapping,
 *     unaligned LDP/STP.
 * PAGE_WRITECOMBINE / MmWriteCombined memory is Normal non-cacheable on both
 * and takes any access, so a driver that may choose should allocate that. A
 * buffer it cannot choose (a WDF or HAL common buffer) goes through these.
 *
 * The device side gets naturally aligned 8-byte stores or loads and single
 * bytes at the ends. The __iso_volatile intrinsics are one LDR/STR each: the
 * compiler can neither pair them, vectorise them, nor turn the loop back into
 * a memset or memcpy call. The other side of RkDevCopy may be misaligned
 * only when it is ordinary cached memory.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <intrin.h>

static __forceinline void
RkDevZero(
    _Out_writes_bytes_all_(Len) void *Dst,
    _In_ SIZE_T Len
    )
{
    unsigned char *d = (unsigned char *)Dst;

    for (; Len && ((ULONG_PTR)d & 7); Len--, d++)
        __iso_volatile_store8((volatile char *)d, 0);
    for (; Len >= 8; Len -= 8, d += 8)
        __iso_volatile_store64((volatile __int64 *)d, 0);
    for (; Len; Len--, d++)
        __iso_volatile_store8((volatile char *)d, 0);
}

/* DevIsSrc: which side is the uncached memory, so which one the loop aligns. */
static __forceinline void
RkDevCopy(
    _Out_writes_bytes_all_(Len) void *Dst,
    _In_reads_bytes_(Len) const void *Src,
    _In_ SIZE_T Len,
    _In_ BOOLEAN DevIsSrc
    )
{
    unsigned char *d = (unsigned char *)Dst;
    const unsigned char *s = (const unsigned char *)Src;
    ULONG_PTR dev = DevIsSrc ? (ULONG_PTR)s : (ULONG_PTR)d;

    for (; Len && (dev & 7); Len--, d++, s++, dev++) {
        if (DevIsSrc)
            *d = (unsigned char)__iso_volatile_load8((const volatile char *)s);
        else
            __iso_volatile_store8((volatile char *)d, (char)*s);
    }
    for (; Len >= 8; Len -= 8, d += 8, s += 8) {
        if (DevIsSrc) {
            __int64 v = __iso_volatile_load64((const volatile __int64 *)s);
            RtlCopyMemory(d, &v, 8);   /* d may be misaligned: cached memory */
        } else {
            __int64 v;
            RtlCopyMemory(&v, s, 8);
            __iso_volatile_store64((volatile __int64 *)d, v);
        }
    }
    for (; Len; Len--, d++, s++) {
        if (DevIsSrc)
            *d = (unsigned char)__iso_volatile_load8((const volatile char *)s);
        else
            __iso_volatile_store8((volatile char *)d, (char)*s);
    }
}

/* One 32-bit or 64-bit access at a naturally aligned uncached address. */
static __forceinline void RkDevWrite32(volatile void *p, ULONG v)
{
    __iso_volatile_store32((volatile __int32 *)p, (__int32)v);
}

static __forceinline ULONG RkDevRead32(const volatile void *p)
{
    return (ULONG)__iso_volatile_load32((const volatile __int32 *)p);
}
