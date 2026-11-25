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

/* IOMMU (rockchip-iommu.c). Two instances, +0x800 and +0x900, one table. */
#define RK_MMU_DTE_ADDR         0x00
#define RK_MMU_STATUS           0x04
#define RK_MMU_COMMAND          0x08
#define RK_MMU_PAGE_FAULT_ADDR  0x0C
#define RK_MMU_INT_RAWSTAT      0x14
#define RK_MMU_INT_CLEAR        0x18
#define RK_MMU_INT_MASK         0x1C
#define RK_MMU_INT_STATUS       0x20
#define RK_MMU_AUTO_GATING      0x24
#define RK_MMU_STATUS_PAGING    0x01
#define RK_MMU_STATUS_FAULT     0x02
#define RK_MMU_STATUS_STALL     0x04
#define RK_MMU_STATUS_FAULT_IS_WRITE 0x20
#define RK_MMU_CMD_ENABLE_PAGING  0
#define RK_MMU_CMD_DISABLE_PAGING 1
#define RK_MMU_CMD_ENABLE_STALL   2
#define RK_MMU_CMD_DISABLE_STALL  3
#define RK_MMU_CMD_ZAP_CACHE      4
#define RK_MMU_CMD_PAGE_FAULT_DONE 5
#define RK_MMU_IRQ_PAGE_FAULT   0x01
#define RK_MMU_IRQ_BUS_ERROR    0x02
#define RK_MMU_IRQ_MASK         (RK_MMU_IRQ_PAGE_FAULT | RK_MMU_IRQ_BUS_ERROR)
#define RK_MMU_AUTO_GATING_DISABLE_FETCH_DTE_TIME_LIMIT 0x80000000u

/*
 * IO address space of the decoder with paging on: 32 bits, a directory of
 * 1024 entries, each a page table of 1024 4 KiB pages (4 MiB). Addresses
 * below V383_IOVA_FIRST are never handed out, so 0 stays "no buffer" in
 * the address check and a small garbage address faults.
 */
#define V383_IOVA_PAGES         0x100000u           /* 4 GiB / 4 KiB */
#define V383_IOVA_FIRST         0x01000000u         /* 16 MiB */
#define V383_PT_COUNT           1024u
/* more page faults than this in one decode: the IOMMU interrupt is masked
 * until the decode has been stopped, so a runaway core cannot storm */
#define V383_MMU_FAULTS_PER_RUN 32

typedef struct _V383_ALLOC {
    LIST_ENTRY      Link;
    ULONG           Handle;
    WDFFILEOBJECT   Owner;
    PEPROCESS       Process;
    BOOLEAN         Iova;       /* pages from MmAllocatePagesForMdlEx, mapped at Dma */
    PVOID           Kva;        /* contiguous path only */
    PHYSICAL_ADDRESS Pa;        /* contiguous path only */
    ULONG           Dma;        /* what the decoder is given: IO address or physical */
    SIZE_T          Size;
    PMDL            Mdl;
    PVOID           UserVa;
} V383_ALLOC, *PV383_ALLOC;

/* Contiguous memory one open handle may hold (V383Alloc). */
#define V383_MAX_PER_FILE   (384u * 1024 * 1024)

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

    /* breadcrumbs (Device Parameters), settings */
    volatile LONG   StepSeq;
    ULONG           WarmupAtD0;     /* Device Parameters WarmupAtD0, default 1: at D0 entry and after an IP reset */
    ULONG           WarmRuns;       /* warm-ups the driver ran by itself */
    ULONG           UserAccess;     /* Device Parameters UserAccess, default 0 (EvtDeviceAdd) */

    /* warm-up */
    PVOID           WarmVa;
    PHYSICAL_ADDRESS WarmPa;
    VDPU383_WARMUP_RESULT Warm;

    /* IOMMU. IommuSetting: Device Parameters "Iommu" (default 1; 0 = the
     * old physically contiguous buffers with paging off). IommuOn: the
     * tables exist and paging was turned on at D0 entry, so buffers are
     * mapped. MmuLock serialises the tables, the IO address bitmap and the
     * IOMMU commands. */
    ULONG           IommuSetting;
    BOOLEAN         IommuOn;
    BOOLEAN         MmuLive;        /* in D0 with paging on: TLB zaps go to the hardware */
    BOOLEAN         SramMapped;
    ULONG           WarmMappedIova; /* the warm-up buffer's identity mapping, or 0 */
    FAST_MUTEX      MmuLock;
    PULONG          Dt;
    PHYSICAL_ADDRESS DtPa;
    PULONG          PtVa[V383_PT_COUNT];
    ULONG64         PtPa[V383_PT_COUNT];
    RTL_BITMAP      IovaMap;
    PULONG          IovaBits;
    ULONG           IovaHint;
    ULONG           IovaPagesMapped;
    ULONG           MmuEnables;
    volatile LONG   RunMmuFaults;   /* fault interrupts during the current decode */
    ULONG           RunFaultAddr;
    ULONG           RunFaultStatus;
    volatile LONG   MmuFaults;      /* all of them */
    ULONG           AllocFailures;

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

/* driver.c */
VOID V383Crumb(_In_ PDEVICE_CONTEXT Ctx, _In_ PCWSTR Step);
VOID V383Fact(_In_ PDEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value);

/* warmup.c */
NTSTATUS V383WarmupInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     V383WarmupFree(_In_ PDEVICE_CONTEXT Ctx);
VOID     V383WarmupRun(_In_ PDEVICE_CONTEXT Ctx, _Out_ VDPU383_WARMUP_RESULT *Result);

#endif
