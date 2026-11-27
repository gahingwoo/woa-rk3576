/*
 * pool.c - the driver's own buffers: ExAllocatePool2 with a header in
 * front and a guard behind every buffer, checked when it is freed.
 *
 * Every buffer the driver writes into (the CMD53 bounce buffer, the
 * firmware files, the converted NVRAM, the registry text) comes from
 * here, not from WdfMemoryCreate. Two reasons: Driver Verifier's special
 * pool and pool tracking for bcmwlsdio.sys cover allocations that
 * bcmwlsdio.sys makes itself (WdfMemoryCreate allocates from
 * Wdf01000.sys), and a write past the end of a buffer is caught at the
 * next free instead of surfacing later in someone else's pool block.
 *
 * Layout: a 64-byte header, the buffer, a 64-byte guard. 64 keeps the
 * buffer on a cache line of its own at both ends (the bounce buffer is
 * the target of SD host DMA); the pool returns 16-byte aligned blocks,
 * page aligned ones from a page up.
 *
 * A broken guard is recorded under Device Parameters (PoolGuard*: the
 * buffer's name, its size and the first bad offset) and in the debugger
 * log. A broken header is not freed: the block is leaked, so the pool
 * does not bugcheck on it and the evidence stays.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"

#define BUF_MAGIC       0x46755742u     /* "BWuF" */
#define BUF_DEAD        0x44614542u     /* "BEaD", after the free */
#define BUF_HDR         64u
#define BUF_GUARD       64u
#define GUARD_BYTE      0xB5u

typedef struct _BWL_BUF {
    ULONG Magic;
    ULONG Size;
    ULONG Tag;
    ULONG Reserved;
    PCSTR Name;
} BWL_BUF;

C_ASSERT(sizeof(BWL_BUF) <= BUF_HDR);

PVOID
BwlBufAlloc(_In_ POOL_FLAGS Flags, _In_ SIZE_T Size, _In_ PCSTR Name)
{
    BWL_BUF *h;
    PUCHAR p;

    if (Size == 0 || Size > MAXULONG - BUF_HDR - BUF_GUARD) {
        return NULL;
    }
    p = (PUCHAR)ExAllocatePool2(Flags, BUF_HDR + Size + BUF_GUARD, BWL_POOL_TAG);
    if (p == NULL) {
        return NULL;
    }
    h = (BWL_BUF *)p;
    h->Magic = BUF_MAGIC;
    h->Size = (ULONG)Size;
    h->Tag = BWL_POOL_TAG;
    h->Name = Name;
    RtlFillMemory(p + BUF_HDR + Size, BUF_GUARD, GUARD_BYTE);
    return p + BUF_HDR;
}

/* 0 if the header and the guard are intact, else 1 + the first bad offset past the buffer (header: ~0). */
ULONG
BwlBufCheck(_In_ PVOID Buf)
{
    BWL_BUF *h = (BWL_BUF *)((PUCHAR)Buf - BUF_HDR);
    PUCHAR g;
    ULONG i;

    if (h->Magic != BUF_MAGIC || h->Tag != BWL_POOL_TAG) {
        return MAXULONG;
    }
    g = (PUCHAR)Buf + h->Size;
    for (i = 0; i < BUF_GUARD; i++) {
        if (g[i] != GUARD_BYTE) {
            return i + 1;
        }
    }
    return 0;
}

/* Record a broken buffer; PASSIVE_LEVEL (registry). */
static VOID
Report(_In_opt_ PBWL_DEVICE_CONTEXT Ctx, _In_ PVOID Buf, _In_ ULONG Bad, _In_ PCSTR Where)
{
    BWL_BUF *h = (BWL_BUF *)((PUCHAR)Buf - BUF_HDR);
    PCSTR name = Bad == MAXULONG ? "?" : h->Name;
    ULONG size = Bad == MAXULONG ? 0 : h->Size;

    BWL_LOG("POOL GUARD BROKEN (%s): buffer %s at %p, size %lu, first bad byte %s+%lu",
            Where, name, Buf, size, Bad == MAXULONG ? "header" : "end", Bad == MAXULONG ? 0 : Bad - 1);
    if (Ctx == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    Ctx->GuardErrors++;
    BwlPutULong(Ctx, L"PoolGuardErrors", Ctx->GuardErrors);
    BwlPutString(Ctx, L"PoolGuardBuffer", name);
    BwlPutString(Ctx, L"PoolGuardWhere", Where);
    BwlPutULong(Ctx, L"PoolGuardSize", size);
    BwlPutULong(Ctx, L"PoolGuardOffset", Bad == MAXULONG ? MAXULONG : Bad - 1);
}

BOOLEAN
BwlBufVerify(_In_opt_ PBWL_DEVICE_CONTEXT Ctx, _In_ PVOID Buf, _In_ PCSTR Where)
{
    ULONG bad = BwlBufCheck(Buf);

    if (bad != 0) {
        Report(Ctx, Buf, bad, Where);
        return FALSE;
    }
    return TRUE;
}

VOID
BwlBufFree(_In_opt_ PBWL_DEVICE_CONTEXT Ctx, _In_opt_ PVOID Buf, _In_ PCSTR Where)
{
    BWL_BUF *h;
    ULONG bad;

    if (Buf == NULL) {
        return;
    }
    h = (BWL_BUF *)((PUCHAR)Buf - BUF_HDR);
    bad = BwlBufCheck(Buf);
    if (bad != 0) {
        Report(Ctx, Buf, bad, Where);
        if (bad == MAXULONG) {
            return;     /* not ours, or overwritten from below: leak it */
        }
    }
    h->Magic = BUF_DEAD;
    ExFreePoolWithTag(h, BWL_POOL_TAG);
}
