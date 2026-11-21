/*
 * vdpu383.h - private definitions of vdpu383.sys
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_DRV_H
#define VDPU383_DRV_H

#include <ntifs.h>     /* KeStackAttachProcess; includes ntddk.h */
#include <wdf.h>
#include <intrin.h>

#include "vdpu383_ioctl.h"

#define V383_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "vdpu383: " fmt "\n", __VA_ARGS__)

#define V383_POOL_TAG   '383V'

/* IOMMU (rockchip-iommu.c) */
#define RK_MMU_DTE_ADDR         0x00
#define RK_MMU_STATUS           0x04
#define RK_MMU_COMMAND          0x08
#define RK_MMU_PAGE_FAULT_ADDR  0x0C
#define RK_MMU_INT_RAWSTAT      0x14
#define RK_MMU_INT_CLEAR        0x18
#define RK_MMU_INT_MASK         0x1C
#define RK_MMU_INT_STATUS       0x20
#define RK_MMU_STATUS_PAGING    0x01
#define RK_MMU_STATUS_STALL     0x04
#define RK_MMU_CMD_DISABLE_PAGING 1
#define RK_MMU_CMD_ENABLE_STALL   2
#define RK_MMU_CMD_DISABLE_STALL  3

typedef struct _V383_ALLOC {
    LIST_ENTRY      Link;
    ULONG           Handle;
    WDFFILEOBJECT   Owner;
    PEPROCESS       Process;
    PVOID           Kva;
    PHYSICAL_ADDRESS Pa;
    SIZE_T          Size;
    PMDL            Mdl;
    PVOID           UserVa;
} V383_ALLOC, *PV383_ALLOC;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE       Device;
    PUCHAR          Regs;
    SIZE_T          RegsLen;
    PHYSICAL_ADDRESS RegsPa;
    BOOLEAN         SramPresent;
    PHYSICAL_ADDRESS SramPa;
    ULONG           SramLen;

    WDFINTERRUPT    DecInterrupt;
    WDFINTERRUPT    MmuInterrupt;
    BOOLEAN         InterruptsConnected;

    KEVENT          RunDone;
    volatile LONG   RunPending;
    ULONG           RunIrq;
    ULONG           RunStatus;

    /* statistics, GET_INFO */
    volatile LONG   IrqCount;
    volatile LONG   MmuIrqCount;
    ULONG           LastIrq, LastStatus;
    ULONG           MmuLastStatus[2], MmuLastFault[2];
    ULONG           Runs, Timeouts;

    /* warm-up */
    PVOID           WarmVa;
    PHYSICAL_ADDRESS WarmPa;
    VDPU383_WARMUP_RESULT Warm;

    /* DMA buffers handed to user mode */
    WDFWAITLOCK     AllocLock;
    LIST_ENTRY      Allocs;
    ULONG           NextHandle;

    WDFQUEUE        Queue;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

static __forceinline ULONG V383Rd(PDEVICE_CONTEXT c, ULONG off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(c->Regs + off));
}

static __forceinline VOID V383Wr(PDEVICE_CONTEXT c, ULONG off, ULONG v)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(c->Regs + off), v);
}

/* warmup.c */
NTSTATUS V383WarmupInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     V383WarmupFree(_In_ PDEVICE_CONTEXT Ctx);
VOID     V383WarmupRun(_In_ PDEVICE_CONTEXT Ctx, _Out_ VDPU383_WARMUP_RESULT *Result);

#endif
