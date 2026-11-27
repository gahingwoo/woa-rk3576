/*
 * driver.c - RK3576 video decoder (RKVDEC, VDPU383), bring-up driver
 *
 * A KMDF function driver for ACPI\RKCP0383, which edk2-rk3576 publishes in
 * an SSDT (Vdec.asl) only when "Video Decoder (VDPU383)" is enabled in
 * setup. The firmware has powered PD_VDEC and set the decoder's clocks; this
 * driver touches neither, and has no power management of its own.
 *
 * It does not decode. It gives a test program (tools/vdpu383test) what it
 * needs to decode through the hardware: DMA buffers mapped into the
 * program, register reads and writes, and "write this register file, start
 * the decoder, wait for the interrupt". The register file comes from
 * drivers/video/vdpu383/lib, built in the program. The planned decoder
 * lives in the display driver (wddm-mali-g52) as a VIDEO_DECODE node; this
 * driver is the prototype that finds out what the hardware needs first.
 *
 * Behaviour follows mainline Linux (drivers/media/platform/rockchip/rkvdec,
 * rkvdec.c and rkvdec-vdpu383-h264.c): how a decode is started, how the
 * interrupt is acknowledged. The warm-up decode is the vendor kernel's
 * (warmup.c): at every D0 entry and after every IP reset, unless the
 * Device Parameters value WarmupAtD0 is 0. Without it, H.264 decodes
 * after a power-on came out wrong, differently on every boot, until one
 * warm-up had run (board, 2026-11-25); `vdpu383test warmup` runs it on
 * demand.
 *
 * The decoder's own IOMMU is run with paging on and this driver's page
 * tables (Device Parameters Iommu, default 1): a buffer is any pages of
 * DRAM, mapped at a free run of 32-bit IO addresses, and the address the
 * caller is given (VDPU383_ALLOC_OUT.dma) is that IO address. Physically
 * contiguous buffers of several MiB could not be had on a running system.
 * The SRAM and the warm-up buffer are mapped at their physical addresses.
 * An IOMMU page fault fails the decode (VDPU383_RUN_MMU_FAULT) and resets
 * the core. Iommu 0, or paging that does not come on, gives the old path:
 * paging off, physical addresses, contiguous buffers below 4 GiB.
 *
 * Zero copy (interface version 3, IOCTL_VDPU383_IMPORT): the pages of a
 * display driver allocation (wddm-mali-g52's D3D11 decoder output texture)
 * at IO addresses of the decoder, from a token the display driver gave the
 * calling process; the pages come from the display driver itself, kernel to
 * kernel (vdpu383_import.h), and it takes them back through V383ImportRevoke
 * before they stop being the allocation's.
 *
 * Only administrators and SYSTEM can open the device, and one handle at a
 * time. Every address register of a decode is checked against the
 * caller's buffers before the decoder is started, unless the caller asks
 * not to.
 *
 * Sessions (interface version 4, vdpu383_check.h): a file object opened
 * from kernel mode, by the display driver for one of its processes, so
 * that a sandboxed process (a browser's GPU process) decodes with no
 * handle of its own. A session may allocate, free, import, run and ask for
 * information, nothing else; every decode is address-checked, and runs
 * with an IOMMU directory that has only the session's buffers in it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "vdpu383.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD V383EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE V383EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE V383EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY V383EvtD0Entry;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED V383EvtD0EntryPostInterrupts;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED V383EvtD0ExitPreInterrupts;
EVT_WDF_INTERRUPT_ISR V383EvtDecIsr;
EVT_WDF_INTERRUPT_DPC V383EvtDecDpc;
EVT_WDF_INTERRUPT_ISR V383EvtMmuIsr;
EVT_WDF_INTERRUPT_DPC V383EvtMmuDpc;
EVT_WDF_OBJECT_CONTEXT_CLEANUP V383EvtDeviceCleanup;
EVT_WDF_IO_IN_CALLER_CONTEXT V383EvtIoInCallerContext;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL V383EvtIoDeviceControl;
EVT_WDF_FILE_CLEANUP V383EvtFileCleanup;
EVT_WDF_DEVICE_FILE_CREATE V383EvtFileCreate;
EVT_WDF_FILE_CLOSE V383EvtFileClose;

#define LINK_IRQ_CLEAR      0x00030000      /* FIELD_PREP_WM16(irq | line irq, 0) */
#define LINK_STATUS_CLEAR   0x03FF0000      /* FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0) */

/* ------------------------------------------------------- breadcrumbs */

/*
 * Breadcrumbs: the step the driver is in, and what it read, in the device's
 * hardware key (Device Parameters), flushed at once, so a hang or a bugcheck
 * at the first register read leaves the last step readable after a reboot
 * with the decoder off in setup:
 *
 *   reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"
 *
 * LastStep, PrevStep, StepSeq; and DWORDs named after what they hold
 * (Version, Mmu0Status, ...). PASSIVE_LEVEL only; at any other level the
 * call does nothing.
 */
static VOID
V383PutULong(_In_ WDFKEY Key, _In_ PCWSTR Name, _In_ ULONG Value)
{
    UNICODE_STRING n;

    RtlInitUnicodeString(&n, Name);
    (VOID)WdfRegistryAssignULong(Key, &n, Value);
}

static BOOLEAN
V383OpenParams(_In_ PDEVICE_CONTEXT Ctx, _In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return FALSE;
    return NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, Access,
                                               WDF_NO_OBJECT_ATTRIBUTES, Key));
}

VOID
V383Crumb(_In_ PDEVICE_CONTEXT Ctx, _In_ PCWSTR Step)
{
    WDFKEY key;
    UNICODE_STRING n, v;
    DECLARE_UNICODE_STRING_SIZE(prev, 64);

    V383_LOG("step %ws", Step);
    if (!V383OpenParams(Ctx, KEY_READ | KEY_WRITE, &key))
        return;
    RtlInitUnicodeString(&n, L"LastStep");
    if (NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &n, NULL, &prev))) {
        RtlInitUnicodeString(&v, L"PrevStep");
        (VOID)WdfRegistryAssignUnicodeString(key, &v, &prev);
    }
    RtlInitUnicodeString(&v, Step);
    (VOID)WdfRegistryAssignUnicodeString(key, &n, &v);
    V383PutULong(key, L"StepSeq", (ULONG)InterlockedIncrement(&Ctx->StepSeq));
    (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    WdfRegistryClose(key);
}

VOID
V383Fact(_In_ PDEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY key;

    if (!V383OpenParams(Ctx, KEY_WRITE, &key))
        return;
    V383PutULong(key, Name, Value);
    (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    WdfRegistryClose(key);
}

static ULONG
V383Setting(_In_ PDEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY key;
    UNICODE_STRING n;
    ULONG v = Default;

    if (V383OpenParams(Ctx, KEY_READ, &key)) {
        RtlInitUnicodeString(&n, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v)))
            v = Default;
        WdfRegistryClose(key);
    }
    return v;
}

/* ------------------------------------------------------------ IOMMU */

/*
 * The decoder's IOMMU: two instances (+0x800, +0x900) that are given the
 * same directory, as mainline gives them one domain. RK3576 has the v2
 * format ("rockchip,rk3576-iommu", "rockchip,rk3568-iommu": 40-bit page
 * addresses in 32-bit entries). Sequences follow rockchip-iommu.c
 * (rk_iommu_enable(), rk_iommu_disable(), rk_iommu_irq()); there is no
 * FORCE_RESET, because the node has rockchip,disable-mmu-reset. The tables
 * are this driver's own, not the OS's: Windows does not know the IOMMU.
 */
static const ULONG V383MmuBase[2] = { V383_MMU0_OFFSET, V383_MMU1_OFFSET };

#define V383_DTE_VALID      0x1
#define V383_PTE_RW_VALID   0x7     /* writable, readable, valid */

/* rk_mk_dte_v2() / rk_mk_pte_v2(): address bits 31:12 in place, 35:32 in
 * 11:8, 39:36 in 7:4 */
static __forceinline ULONG
V383MmuEntry(_In_ ULONG64 Pa, _In_ ULONG Flags)
{
    return (ULONG)(Pa & 0xFFFFF000u) | (((ULONG)(Pa >> 32) & 0xF) << 8) |
           (((ULONG)(Pa >> 36) & 0xF) << 4) | Flags;
}

static BOOLEAN
V383MmuWaitBase(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Base, _In_ ULONG Mask, _In_ ULONG Want)
{
    ULONG i;

    for (i = 0; i < 1000; i++) {
        if ((V383Rd(Ctx, Base + RK_MMU_STATUS) & Mask) == Want)
            return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

/* rk_iommu_disable() for one instance, if it has paging on. Used with the
 * IOMMU setting 0 at D0 entry, and at D0 exit with paging on. */
static VOID
V383MmuPagingOff(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Base
    )
{
    ULONG st = V383Rd(Ctx, Base + RK_MMU_STATUS);

    if (!(st & RK_MMU_STATUS_PAGING))
        return;

    V383_LOG("IOMMU at +0x%x has paging on (status 0x%08x); turning it off", Base, st);
    if (st & RK_MMU_STATUS_FAULT)
        V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_PAGE_FAULT_DONE);
    V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_ENABLE_STALL);
    (VOID)V383MmuWaitBase(Ctx, Base, RK_MMU_STATUS_STALL, RK_MMU_STATUS_STALL);
    V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_DISABLE_PAGING);
    (VOID)V383MmuWaitBase(Ctx, Base, RK_MMU_STATUS_PAGING, 0);
    V383Wr(Ctx, Base + RK_MMU_INT_MASK, 0);
    V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_DISABLE_STALL);
    V383_LOG("IOMMU at +0x%x: status now 0x%08x", Base, V383Rd(Ctx, Base + RK_MMU_STATUS));
}

/*
 * rk_iommu_enable(): stall (only possible with paging on), directory,
 * TLB zap, fault interrupts on, the "iommu blocked" workaround bit, paging
 * on, unstall. At D0 entry, after an IP reset (which resets the IOMMU with
 * the core; mainline's rkvdec_iommu_restore()), before a decode if
 * paging is found off, and when a decode needs another directory than the
 * one the IOMMU has (a session's, V383UseDomain). The directory is
 * Ctx->ActiveDom's. TRUE if both instances have paging on. MmuLock held.
 */
static BOOLEAN
V383MmuEnable(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG dte = V383MmuEntry((ULONG64)Ctx->ActiveDom->DtPa.QuadPart, V383_DTE_VALID);
    BOOLEAN ok = TRUE;
    ULONG i, st;

    KeMemoryBarrier();
    __dsb(_ARM64_BARRIER_SY);          /* table writes (non-cached) before the walk */
    for (i = 0; i < 2; i++) {
        ULONG b = V383MmuBase[i];

        st = V383Rd(Ctx, b + RK_MMU_STATUS);
        if (st & RK_MMU_STATUS_PAGING) {
            if (st & RK_MMU_STATUS_FAULT)
                V383Wr(Ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_PAGE_FAULT_DONE);
            V383Wr(Ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_ENABLE_STALL);
            if (!V383MmuWaitBase(Ctx, b, RK_MMU_STATUS_STALL, RK_MMU_STATUS_STALL))
                V383_LOG("IOMMU +0x%x: stall timed out, status 0x%08x", b, V383Rd(Ctx, b + RK_MMU_STATUS));
        }
        V383Wr(Ctx, b + RK_MMU_DTE_ADDR, dte);
        V383Wr(Ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
        V383Wr(Ctx, b + RK_MMU_INT_CLEAR, RK_MMU_IRQ_MASK);
        V383Wr(Ctx, b + RK_MMU_INT_MASK, RK_MMU_IRQ_MASK);
        V383Wr(Ctx, b + RK_MMU_AUTO_GATING,
               V383Rd(Ctx, b + RK_MMU_AUTO_GATING) | RK_MMU_AUTO_GATING_DISABLE_FETCH_DTE_TIME_LIMIT);
    }
    for (i = 0; i < 2; i++) {
        ULONG b = V383MmuBase[i];

        if (!(V383Rd(Ctx, b + RK_MMU_STATUS) & RK_MMU_STATUS_PAGING))
            V383Wr(Ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_ENABLE_PAGING);
        if (!V383MmuWaitBase(Ctx, b, RK_MMU_STATUS_PAGING, RK_MMU_STATUS_PAGING)) {
            V383_LOG("IOMMU +0x%x: paging did not come on, status 0x%08x dte 0x%08x (wrote 0x%08x)", b,
                     V383Rd(Ctx, b + RK_MMU_STATUS), V383Rd(Ctx, b + RK_MMU_DTE_ADDR), dte);
            ok = FALSE;
        }
    }
    for (i = 0; i < 2; i++) {
        ULONG b = V383MmuBase[i];

        if (V383Rd(Ctx, b + RK_MMU_STATUS) & RK_MMU_STATUS_STALL) {
            V383Wr(Ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_DISABLE_STALL);
            (VOID)V383MmuWaitBase(Ctx, b, RK_MMU_STATUS_STALL, 0);
        }
    }
    Ctx->MmuEnables++;
    return ok;
}

/* TLB shoot-down after the tables changed. MmuLock held. */
static VOID
V383MmuZap(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    KeMemoryBarrier();
    __dsb(_ARM64_BARRIER_SY);
    if (!Ctx->MmuLive)
        return;
    V383Wr(Ctx, V383_MMU0_OFFSET + RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
    V383Wr(Ctx, V383_MMU1_OFFSET + RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
}

/* One page, non-cached, below 4 GiB (v2 could take 40 bits; the VOP2's
 * tables, the same IOMMU, are below 4 GiB and work). */
static PVOID
V383TablePage(
    _Out_ PHYSICAL_ADDRESS *Pa
    )
{
    PHYSICAL_ADDRESS low, high, skip;
    PVOID va;

    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    va = MmAllocateContiguousNodeMemory(PAGE_SIZE, low, high, skip, PAGE_READWRITE | PAGE_NOCACHE,
                                        MM_ANY_NODE_OK);
    if (va) {
        RtlZeroMemory(va, PAGE_SIZE);
        *Pa = MmGetPhysicalAddress(va);
    } else {
        Pa->QuadPart = 0;
    }
    return va;
}

/* The page table of a directory for an IO address, made and entered in
 * the directory on first use. MmuLock held. */
static PULONG
V383MmuPt(
    _In_ PV383_DOMAIN Dom,
    _In_ ULONG Iova
    )
{
    ULONG d = Iova >> 22;
    PHYSICAL_ADDRESS pa;

    if (!Dom->PtVa[d]) {
        Dom->PtVa[d] = (PULONG)V383TablePage(&pa);
        if (!Dom->PtVa[d])
            return NULL;
        Dom->PtPa[d] = (ULONG64)pa.QuadPart;
        Dom->Dt[d] = V383MmuEntry(Dom->PtPa[d], V383_DTE_VALID);
    }
    return Dom->PtVa[d];
}

/* Entries for Pages pages from Iova on cleared in Dom. MmuLock held. */
static VOID
V383MmuClear(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PV383_DOMAIN Dom,
    _In_ ULONG Iova,
    _In_ ULONG Pages
    )
{
    ULONG i;

    for (i = 0; i < Pages; i++) {
        ULONG va = Iova + (i << PAGE_SHIFT);
        PULONG pt = Dom->PtVa[va >> 22];

        if (pt && (pt[(va >> PAGE_SHIFT) & 0x3FF] & 1)) {
            pt[(va >> PAGE_SHIFT) & 0x3FF] = 0;
            Ctx->IovaPagesMapped--;
        }
    }
}

/* Pages pages entered in Dom from Iova on: page i at physical Pfns[i], or
 * at Pa + i pages when Pfns is NULL. All or nothing. MmuLock held. */
static NTSTATUS
V383MmuEnter(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PV383_DOMAIN Dom,
    _In_ ULONG Iova,
    _In_opt_ PPFN_NUMBER Pfns,
    _In_ ULONG64 Pa,
    _In_ ULONG Pages
    )
{
    ULONG i;

    for (i = 0; i < Pages; i++) {
        ULONG va = Iova + (i << PAGE_SHIFT);
        ULONG64 pa = Pfns ? ((ULONG64)Pfns[i] << PAGE_SHIFT) : Pa + ((ULONG64)i << PAGE_SHIFT);
        PULONG pt = V383MmuPt(Dom, va);

        if (!pt || pa >= (1ull << 40)) {
            V383MmuClear(Ctx, Dom, Iova, i);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pt[(va >> PAGE_SHIFT) & 0x3FF] = V383MmuEntry(pa, V383_PTE_RW_VALID);
        Ctx->IovaPagesMapped++;
    }
    return STATUS_SUCCESS;
}

/* A free run of IO addresses; 0 if there is none. Allocation goes on from
 * where the last one ended, so a freed range is not handed out again at
 * once (a stale address faults instead of hitting the next buffer).
 * MmuLock held. */
static ULONG
V383IovaAlloc(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Pages
    )
{
    ULONG idx = RtlFindClearBitsAndSet(&Ctx->IovaMap, Pages, Ctx->IovaHint);

    if (idx == 0xFFFFFFFF)
        return 0;
    Ctx->IovaHint = (idx + Pages) % V383_IOVA_PAGES;
    return idx << PAGE_SHIFT;
}

/* Pa..Pa+Size at the same IO address (the SRAM, the warm-up buffer): the
 * register values the decoder is given for them stay what they were. */
static NTSTATUS
V383MmuIdentityMap(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG64 Pa,
    _In_ SIZE_T Size
    )
{
    ULONG first, pages;
    NTSTATUS status;

    if ((Pa & (PAGE_SIZE - 1)) || Size == 0 || Pa + Size > 0x100000000ull || Pa < V383_IOVA_FIRST)
        return STATUS_INVALID_PARAMETER;
    first = (ULONG)(Pa >> PAGE_SHIFT);
    pages = (ULONG)((Size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    ExAcquireFastMutex(&Ctx->MmuLock);
    if (!RtlAreBitsClear(&Ctx->IovaMap, first, pages)) {
        ExReleaseFastMutex(&Ctx->MmuLock);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    RtlSetBits(&Ctx->IovaMap, first, pages);
    status = V383MmuEnter(Ctx, &Ctx->Dom0, (ULONG)Pa, NULL, Pa, pages);
    if (!NT_SUCCESS(status))
        RtlClearBits(&Ctx->IovaMap, first, pages);
    V383MmuZap(Ctx);
    ExReleaseFastMutex(&Ctx->MmuLock);
    return status;
}

static VOID
V383MmuUnmapRange(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PV383_DOMAIN Dom,
    _In_ ULONG Iova,
    _In_ SIZE_T Size
    )
{
    ULONG pages = (ULONG)((Size + PAGE_SIZE - 1) >> PAGE_SHIFT);

    ExAcquireFastMutex(&Ctx->MmuLock);
    V383MmuClear(Ctx, Dom, Iova, pages);
    V383MmuZap(Ctx);
    RtlClearBits(&Ctx->IovaMap, Iova >> PAGE_SHIFT, pages);
    ExReleaseFastMutex(&Ctx->MmuLock);
}

/* The directory and the IO address bitmap; page tables come on demand.
 * Kept until the device object goes (V383EvtDeviceCleanup), so buffers
 * that outlive a ReleaseHardware are still unmapped safely. */
static NTSTATUS
V383MmuTablesInit(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    if (Ctx->Dom0.Dt)
        return STATUS_SUCCESS;
    Ctx->IovaBits = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, V383_IOVA_PAGES / 8, V383_POOL_TAG);
    if (!Ctx->IovaBits)
        return STATUS_INSUFFICIENT_RESOURCES;
    Ctx->Dom0.Dt = (PULONG)V383TablePage(&Ctx->Dom0.DtPa);
    if (!Ctx->Dom0.Dt) {
        ExFreePoolWithTag(Ctx->IovaBits, V383_POOL_TAG);
        Ctx->IovaBits = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlInitializeBitMap(&Ctx->IovaMap, Ctx->IovaBits, V383_IOVA_PAGES);
    RtlClearAllBits(&Ctx->IovaMap);
    RtlSetBits(&Ctx->IovaMap, 0, V383_IOVA_FIRST >> PAGE_SHIFT);
    RtlSetBits(&Ctx->IovaMap, V383_IOVA_PAGES - 1, 1);     /* no buffer ends at 2^32 */
    Ctx->IovaHint = V383_IOVA_FIRST >> PAGE_SHIFT;
    return STATUS_SUCCESS;
}

/* A directory's tables, freed. */
static VOID
V383DomainTablesFree(
    _In_ PV383_DOMAIN Dom
    )
{
    ULONG i;

    for (i = 0; i < V383_PT_COUNT; i++)
        if (Dom->PtVa[i]) {
            MmFreeContiguousMemory(Dom->PtVa[i]);
            Dom->PtVa[i] = NULL;
        }
    if (Dom->Dt) {
        MmFreeContiguousMemory(Dom->Dt);
        Dom->Dt = NULL;
    }
}

static VOID
V383MmuTablesFree(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    V383DomainTablesFree(&Ctx->Dom0);
    if (Ctx->IovaBits) {
        ExFreePoolWithTag(Ctx->IovaBits, V383_POOL_TAG);
        Ctx->IovaBits = NULL;
    }
}

/*
 * A session's directory (vdpu383_check.h): empty, so a decode of the
 * session reaches its own buffers and imports and nothing else, not even
 * the SRAM. NULL without memory.
 */
static PV383_DOMAIN
V383DomainCreate(VOID)
{
    PV383_DOMAIN d = (PV383_DOMAIN)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*d), V383_POOL_TAG);

    if (!d)
        return NULL;
    d->Dt = (PULONG)V383TablePage(&d->DtPa);
    if (!d->Dt) {
        ExFreePoolWithTag(d, V383_POOL_TAG);
        return NULL;
    }
    return d;
}

/*
 * The IOMMU walks Dom's directory from now on: stall, the new directory,
 * TLB zap, paging on (V383MmuEnable, the sequence every IP reset already
 * goes through). Under the decode gate (no decode runs), PASSIVE_LEVEL;
 * takes MmuLock. Without paging (D0 exit, IOMMU off) only the choice is
 * kept, for D0 entry.
 */
static VOID
V383UseDomain(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PV383_DOMAIN Dom
    )
{
    if (!Ctx->IommuOn || Ctx->ActiveDom == Dom)
        return;
    ExAcquireFastMutex(&Ctx->MmuLock);
    Ctx->ActiveDom = Dom;
    Ctx->DomainSwitches++;
    if (Ctx->MmuLive && !V383MmuEnable(Ctx))
        V383_LOG("IOMMU: paging did not come back on with another directory");
    ExReleaseFastMutex(&Ctx->MmuLock);
}

/* A session's directory goes, after every buffer of the session has been
 * unmapped (file close). Under the decode gate. */
static VOID
V383DomainDestroy(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PV383_DOMAIN Dom
    )
{
    V383UseDomain(Ctx, &Ctx->Dom0);
    if (Ctx->ActiveDom == Dom)          /* IOMMU off: never walked */
        Ctx->ActiveDom = &Ctx->Dom0;
    V383DomainTablesFree(Dom);
    ExFreePoolWithTag(Dom, V383_POOL_TAG);
}

/* ------------------------------------------------------------ cache */

/* rkvdec_vdpu383_config_cache() in mainline: cacheable, read-allocate,
 * 64-byte lines, and the vendor driver's read limit. The registers lose
 * their configuration when the decoder powers down; mainline programs them
 * at every runtime resume. Here: at D0 entry, and after every IP reset,
 * because whether the reset clears them is not known. */
static VOID
V383CacheConfig(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    V383Wr(Ctx, V383_CACHE_OFFSET + V383_CACHE_CACHE0_CFG,
           V383_CACHE_CFG_CACHEABLE | V383_CACHE_CFG_READ_ALLOCATE | V383_CACHE_CFG_LINE_64B);
    V383Wr(Ctx, V383_CACHE_OFFSET + V383_CACHE_MAX_READS, V383_CACHE_MAX_READS_DEFAULT);
}

/* rkvdec_vdpu383_clear_cache(): reference buffers are rewritten between
 * frames, so the cache is cleared before every frame. */
static VOID
V383CacheClear(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    V383Wr(Ctx, V383_CACHE_OFFSET + V383_CACHE_CLR_CACHE0, 1);
}

/* ---------------------------------------------------------- warm-up */

/*
 * rk3576_workaround_run(), which the vendor kernel runs at probe and at
 * every resume: one canned H.264 picture through the link registers.
 * Without it, every H.264 decode after a power-on came out wrong (chroma,
 * and luma too with the IOMMU off), the same within one boot and
 * different from boot to boot, and HEVC decodes did not cure it; after
 * one warm-up, H.264 stayed right (board, 2026-11-25). Whatever it sets
 * up is inside the decoder: the warm-up shares no buffer with a decode.
 * PASSIVE_LEVEL; the decoder interrupt is masked while it polls, and the
 * interrupt lock is held around it once interrupts are connected.
 */
static VOID
V383WarmupNow(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PCWSTR Why
    )
{
    if (Ctx->WarmupAtD0 != 1 || !Ctx->WarmVa)
        return;
    if (Ctx->IommuOn && !Ctx->WarmMappedIova) {
        V383_LOG("warm-up (%ws): skipped, its buffer is not mapped in the IOMMU", Why);
        return;
    }
    /* the warm-up buffer is in the device's directory only */
    V383UseDomain(Ctx, &Ctx->Dom0);
    V383Crumb(Ctx, Why);
    if (Ctx->InterruptsConnected)
        WdfInterruptAcquireLock(Ctx->DecInterrupt);
    V383WarmupRun(Ctx, &Ctx->Warm);
    if (Ctx->InterruptsConnected)
        WdfInterruptReleaseLock(Ctx->DecInterrupt);
    Ctx->WarmRuns++;
    V383Fact(Ctx, L"WarmupIrq", Ctx->Warm.irq);
    V383Fact(Ctx, L"WarmupStatus", Ctx->Warm.status);
    V383Fact(Ctx, L"WarmupTimedOut", Ctx->Warm.timed_out);
    V383Fact(Ctx, L"WarmupRuns", Ctx->WarmRuns);
}

/* ------------------------------------------------------------ reset */

/* vdpu383_stop() in mainline ("Stop the VDPU383 core on timeout") and
 * rkvdec_vdpu383_reset() in the vendor kernel: IP reset through the link
 * block, which resets the core and the IOMMU. Returns link status. */
static ULONG
V383IpReset(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG st = 0, i;

    V383Wr(Ctx, V383_LINK_IRQ, LINK_IRQ_CLEAR);             /* irq, line irq off (mainline) */
    V383Wr(Ctx, V383_LINK_IP_ENABLE, 0x00008000);          /* irq_dis (vendor) */
    V383Wr(Ctx, V383_LINK_IP_RESET, 1);
    for (i = 0; i < 200; i++) {
        st = V383Rd(Ctx, V383_LINK_STATUS);
        if (st & V383_STA_IP_RST_RDY)
            break;
        KeStallExecutionProcessor(1);
    }
    V383Wr(Ctx, V383_LINK_STATUS, 0x08000000);             /* clear ip_rst_rdy */
    V383Wr(Ctx, V383_LINK_IRQ, 0xFFFF0000);
    V383Wr(Ctx, V383_LINK_STATUS, 0xFFFF0000);
    V383Wr(Ctx, V383_LINK_IP_ENABLE, 0);
    V383_LOG("IP reset: status 0x%08x after %u polls%s", st, i,
             (st & V383_STA_IP_RST_RDY) ? "" : " (not ready)");

    /* An IP reset resets the IOMMU too. Mainline reprograms its page
     * table afterwards (rkvdec_iommu_restore()); the callers do that here
     * (V383MmuRestore), outside the interrupt lock. */
    V383CacheConfig(Ctx);
    return st;
}

/* After an IP reset or a decode error: paging back on with the tables
 * (with the IOMMU in use), PASSIVE_LEVEL. */
static VOID
V383MmuRestore(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    if (!Ctx->IommuOn || !Ctx->MmuLive)
        return;
    ExAcquireFastMutex(&Ctx->MmuLock);
    if (!V383MmuEnable(Ctx))
        V383_LOG("IOMMU: paging could not be restored");
    ExReleaseFastMutex(&Ctx->MmuLock);
}

/* -------------------------------------------------------- interrupts */

BOOLEAN
V383EvtDecIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));
    ULONG irq, status;

    UNREFERENCED_PARAMETER(MessageID);

    irq = V383Rd(ctx, V383_LINK_IRQ);
    if (!(irq & 3))
        return FALSE;
    status = V383Rd(ctx, V383_LINK_STATUS);

    /* rkvdec.c vdpu383_irq_handler(): clear the status, and the interrupt,
     * which "must be disabled" on this core */
    V383Wr(ctx, V383_LINK_STATUS, LINK_STATUS_CLEAR);
    V383Wr(ctx, V383_LINK_IRQ, LINK_IRQ_CLEAR);

    ctx->LastIrq = irq;
    ctx->LastStatus = status;
    InterlockedIncrement(&ctx->IrqCount);
    if (ctx->RunPending) {
        ctx->RunIrq = irq;
        ctx->RunStatus = status;
    }
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
V383EvtDecDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedObject);

    if (InterlockedExchange(&ctx->RunPending, 0))
        KeSetEvent(&ctx->RunDone, IO_NO_INCREMENT, FALSE);
}

/*
 * rk_iommu_irq(): a page fault is logged, the TLB zapped and the fault
 * acknowledged, so the core is not left stalled. With a decode pending the
 * fault also ends the wait (the DPC), and the decode is failed and the
 * core reset (V383Run): the IO address was not one of the caller's
 * buffers, so the picture is wrong whatever else happens. A core that
 * keeps faulting has the IOMMU interrupt masked after
 * V383_MMU_FAULTS_PER_RUN; V383MmuEnable unmasks it.
 */
BOOLEAN
V383EvtMmuIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));
    BOOLEAN ours = FALSE;
    ULONG i;

    UNREFERENCED_PARAMETER(MessageID);

    for (i = 0; i < 2; i++) {
        ULONG b = V383MmuBase[i];
        ULONG st = V383Rd(ctx, b + RK_MMU_INT_STATUS);
        ULONG mst, addr;

        if (!st)
            continue;
        mst = V383Rd(ctx, b + RK_MMU_STATUS);
        addr = V383Rd(ctx, b + RK_MMU_PAGE_FAULT_ADDR);
        ctx->MmuLastStatus[i] = mst;
        ctx->MmuLastFault[i] = addr;
        if (st & RK_MMU_IRQ_PAGE_FAULT) {
            V383Wr(ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_ZAP_CACHE);
            V383Wr(ctx, b + RK_MMU_COMMAND, RK_MMU_CMD_PAGE_FAULT_DONE);
        }
        V383Wr(ctx, b + RK_MMU_INT_CLEAR, st);
        InterlockedIncrement(&ctx->MmuFaults);
        if (ctx->RunPending) {
            if (InterlockedIncrement(&ctx->RunMmuFaults) == 1) {
                ctx->RunFaultAddr = addr;
                ctx->RunFaultStatus = mst;
            }
            if (ctx->RunMmuFaults > V383_MMU_FAULTS_PER_RUN) {
                V383Wr(ctx, V383_MMU0_OFFSET + RK_MMU_INT_MASK, 0);
                V383Wr(ctx, V383_MMU1_OFFSET + RK_MMU_INT_MASK, 0);
            }
        }
        ours = TRUE;
    }
    if (ours) {
        InterlockedIncrement(&ctx->MmuIrqCount);
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
    return ours;
}

VOID
V383EvtMmuDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedObject);

    if (ctx->RunMmuFaults)
        V383_LOG("IOMMU fault at IO address 0x%08x (status 0x%08x), %d in this decode",
                 ctx->RunFaultAddr, ctx->RunFaultStatus, ctx->RunMmuFaults);
    if (ctx->RunMmuFaults && InterlockedExchange(&ctx->RunPending, 0))
        KeSetEvent(&ctx->RunDone, IO_NO_INCREMENT, FALSE);
}

/* ---------------------------------------------------------- hardware */

NTSTATUS
V383EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    ULONG i, nmem = 0;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    V383Crumb(ctx, L"PrepareHardware: start");
    ctx->WarmupAtD0 = V383Setting(ctx, L"WarmupAtD0", 1);
    ctx->IommuSetting = V383Setting(ctx, L"Iommu", 1);

    for (i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (d->Type != CmResourceTypeMemory)
            continue;
        if (nmem == 0) {
            ctx->RegsPa = d->u.Memory.Start;
            ctx->RegsLen = d->u.Memory.Length;
        } else if (nmem == 1) {
            ctx->SramPresent = TRUE;
            ctx->SramPa = d->u.Memory.Start;
            ctx->SramLen = d->u.Memory.Length;
        }
        nmem++;
    }
    if (nmem == 0 || ctx->RegsLen < V383_REG_BLOCK_SIZE) {
        V383_LOG("no register resource of 4 KiB (found %u memory resources)", nmem);
        V383Crumb(ctx, L"PrepareHardware: no register resource");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ctx->Regs = (PUCHAR)MmMapIoSpaceEx(ctx->RegsPa, V383_REG_BLOCK_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!ctx->Regs)
        return STATUS_INSUFFICIENT_RESOURCES;

    V383_LOG("registers at 0x%llx, SRAM %s 0x%llx+0x%x", ctx->RegsPa.QuadPart,
             ctx->SramPresent ? "at" : "absent", ctx->SramPa.QuadPart, ctx->SramLen);

    V383Fact(ctx, L"RegsPa", (ULONG)ctx->RegsPa.QuadPart);
    V383Fact(ctx, L"SramPa", ctx->SramPresent ? (ULONG)ctx->SramPa.QuadPart : 0);

    /* The IOMMU's tables (the hardware is not touched until D0 entry).
     * Without them, the contiguous physical path as with Iommu 0. */
    if (ctx->IommuSetting != 0) {
        NTSTATUS st = V383MmuTablesInit(ctx);
        if (!NT_SUCCESS(st)) {
            V383_LOG("IOMMU tables: 0x%08x; buffers will be physically contiguous", st);
            ctx->IommuSetting = 0;
        }
    }
    V383Fact(ctx, L"IommuSetting", ctx->IommuSetting);

    /* not fatal: the warm-up is then skipped */
    (VOID)V383WarmupInit(ctx);

    /* The SRAM and the warm-up buffer at their physical addresses, so the
     * addresses given to the decoder for them are the same either way
     * (mainline maps the SRAM into the decoder's domain the same way,
     * rkvdec-rcb.c). */
    if (ctx->IommuSetting != 0) {
        if (ctx->SramPresent && !ctx->SramMapped) {
            NTSTATUS st = V383MmuIdentityMap(ctx, (ULONG64)ctx->SramPa.QuadPart, ctx->SramLen);
            ctx->SramMapped = NT_SUCCESS(st);
            if (!ctx->SramMapped)
                V383_LOG("SRAM 0x%llx: not mapped (0x%08x); a decode using it will fault",
                         ctx->SramPa.QuadPart, st);
        }
        if (ctx->WarmVa && NT_SUCCESS(V383MmuIdentityMap(ctx, (ULONG64)ctx->WarmPa.QuadPart, 2 * PAGE_SIZE)))
            ctx->WarmMappedIova = (ULONG)ctx->WarmPa.QuadPart;
    }
    V383Crumb(ctx, L"PrepareHardware: done (no register touched yet)");
    return STATUS_SUCCESS;
}

NTSTATUS
V383EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->WarmMappedIova) {
        V383MmuUnmapRange(ctx, &ctx->Dom0, ctx->WarmMappedIova, 2 * PAGE_SIZE);
        ctx->WarmMappedIova = 0;
    }
    V383WarmupFree(ctx);
    if (ctx->Regs) {
        MmUnmapIoSpace(ctx->Regs, V383_REG_BLOCK_SIZE);
        ctx->Regs = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
V383EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    ULONG ver, mmu0, mmu1;

    UNREFERENCED_PARAMETER(PreviousState);

    /*
     * The first register read of the session. If PD_VDEC or the decoder's
     * clocks are off, this is where the bus errors out (an SError, so a
     * bugcheck), and LastStep is left saying so.
     */
    V383Crumb(ctx, L"D0Entry: first register read");
    ver = V383Rd(ctx, V383_FUNC_OFFSET);
    mmu0 = V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_STATUS);
    mmu1 = V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_STATUS);
    V383Fact(ctx, L"Version", ver);
    V383Fact(ctx, L"Mmu0Status", mmu0);
    V383Fact(ctx, L"Mmu1Status", mmu1);
    V383Fact(ctx, L"LinkIrqAtD0", V383Rd(ctx, V383_LINK_IRQ));
    V383Fact(ctx, L"LinkStatusAtD0", V383Rd(ctx, V383_LINK_STATUS));
    V383Crumb(ctx, L"D0Entry: registers read");

    V383_LOG("D0 entry: version 0x%08x, link irq 0x%08x status 0x%08x, mmu 0x%08x/0x%08x",
             V383Rd(ctx, V383_FUNC_OFFSET), V383Rd(ctx, V383_LINK_IRQ), V383Rd(ctx, V383_LINK_STATUS),
             V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_STATUS), V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_STATUS));

    /*
     * IOMMU: paging on with this driver's tables (Iommu 1, the default), or
     * off as the firmware should have left it (Iommu 0). If paging does not
     * come on, the driver falls back to contiguous physical buffers for
     * every buffer allocated from now on.
     */
    if (ctx->IommuSetting != 0 && ctx->Dom0.Dt) {
        BOOLEAN ok;

        ExAcquireFastMutex(&ctx->MmuLock);
        ok = V383MmuEnable(ctx);
        ExReleaseFastMutex(&ctx->MmuLock);
        V383Fact(ctx, L"MmuDte0", V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_DTE_ADDR));
        V383Fact(ctx, L"MmuEnableStatus0", V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_STATUS));
        V383Fact(ctx, L"MmuEnableStatus1", V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_STATUS));
        if (!ok) {
            V383_LOG("IOMMU: paging did not come on; falling back to physical addresses");
            V383MmuPagingOff(ctx, V383_MMU0_OFFSET);
            V383MmuPagingOff(ctx, V383_MMU1_OFFSET);
        }
        ctx->IommuOn = ok;
        ctx->MmuLive = ok;
    } else {
        V383MmuPagingOff(ctx, V383_MMU0_OFFSET);
        V383MmuPagingOff(ctx, V383_MMU1_OFFSET);
        ctx->IommuOn = FALSE;
    }
    V383Fact(ctx, L"IommuOn", ctx->IommuOn);

    /* stale interrupt state from the firmware or a previous OS */
    V383Wr(ctx, V383_LINK_IRQ, 0xFFFF0000);
    V383Wr(ctx, V383_LINK_STATUS, 0xFFFF0000);

    V383_LOG("D0 entry: cache cfg 0x%08x max reads 0x%08x (power-on values)",
             V383Rd(ctx, V383_CACHE_OFFSET + V383_CACHE_CACHE0_CFG),
             V383Rd(ctx, V383_CACHE_OFFSET + V383_CACHE_MAX_READS));
    V383Fact(ctx, L"CacheCfgPowerOn", V383Rd(ctx, V383_CACHE_OFFSET + V383_CACHE_CACHE0_CFG));
    V383Fact(ctx, L"CacheMaxReadsPowerOn", V383Rd(ctx, V383_CACHE_OFFSET + V383_CACHE_MAX_READS));
    V383CacheConfig(ctx);
    V383Crumb(ctx, L"D0Entry: IOMMU and cache set");

    /* rk3576_workaround_run(): probe and resume; interrupts are not yet
     * connected here. On unless WarmupAtD0 = 0. */
    RtlZeroMemory(&ctx->Warm, sizeof(ctx->Warm));
    V383WarmupNow(ctx, L"D0Entry: warm-up decode");
    V383Crumb(ctx, L"D0Entry: done");
    return STATUS_SUCCESS;
}

NTSTATUS
V383EvtD0EntryPostInterrupts(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    UNREFERENCED_PARAMETER(PreviousState);
    GetContext(Device)->InterruptsConnected = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
V383EvtD0ExitPreInterrupts(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);
    ctx->InterruptsConnected = FALSE;
    /* rk_iommu_disable(); the tables and the mappings stay, and D0 entry
     * programs them again. IommuOn stays: buffers keep their IO addresses. */
    if (ctx->MmuLive) {
        ExAcquireFastMutex(&ctx->MmuLock);
        ctx->MmuLive = FALSE;
        V383MmuPagingOff(ctx, V383_MMU0_OFFSET);
        V383MmuPagingOff(ctx, V383_MMU1_OFFSET);
        ExReleaseFastMutex(&ctx->MmuLock);
    }
    V383Crumb(ctx, L"D0Exit");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------- decode gate */

static VOID
V383GateEnter(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    (VOID)KeWaitForSingleObject(&Ctx->RunGate, Executive, KernelMode, FALSE, NULL);
}

static VOID
V383GateLeave(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    KeSetEvent(&Ctx->RunGate, IO_NO_INCREMENT, FALSE);
}

/* One call to the exporter (vdpu383_import.h). Status stays
 * STATUS_NOT_FOUND when no display driver has registered; FALSE when there
 * is not even the callback object (maliwddm never started its exporter). */
static BOOLEAN
V383ExporterCall(
    _In_ ULONG Op,
    _Inout_ PVDPU383_IMPORT_REQUEST Req
    )
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    PCALLBACK_OBJECT cb;

    Req->Version = VDPU383_IMPORT_VERSION;
    Req->Size = sizeof(*Req);
    Req->Status = STATUS_NOT_FOUND;
    RtlInitUnicodeString(&name, VDPU383_IMPORT_CALLBACK_NAME);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ExCreateCallback(&cb, &oa, FALSE, FALSE)))
        return FALSE;
    ExNotifyCallback(cb, Req, (PVOID)(ULONG_PTR)Op);
    ObDereferenceObject(cb);
    return TRUE;
}

/* The exporter forgets the import: no revoke for it after this returns.
 * Called holding none of this driver's locks (the exporter may be inside
 * a revoke that waits for the decode gate). */
static VOID
V383ImportRelease(
    _In_ ULONG64 Token,
    _In_ ULONG Handle
    )
{
    VDPU383_IMPORT_REQUEST r;

    RtlZeroMemory(&r, sizeof(r));
    r.Token = Token;
    r.ProcessId = PsGetCurrentProcessId();
    r.Cookie = Handle;
    (VOID)V383ExporterCall(VDPU383_IMPORT_OP_RELEASE, &r);
}

/* ---------------------------------------------------- DMA buffers */

static VOID
V383FreeAlloc(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PV383_ALLOC A
    )
{
    KAPC_STATE apc;
    BOOLEAN attach = (A->Process && A->Process != PsGetCurrentProcess());

    /* An import: out of the list already (under the decode gate), so no
     * revoke reaches it; the exporter is told before the IO addresses go
     * back to the pool. */
    if (A->Import && A->Token)
        V383ImportRelease(A->Token, A->Handle);
    /* the decoder loses the pages before anything else does */
    if (A->Iova && A->Dma)
        V383MmuUnmapRange(Ctx, A->Dom, A->Dma, A->Size);
    if (A->UserVa && A->Mdl) {
        if (attach)
            KeStackAttachProcess(A->Process, &apc);
        MmUnmapLockedPages(A->UserVa, A->Mdl);
        if (attach)
            KeUnstackDetachProcess(&apc);
    }
    if (A->Mdl) {
        if (A->Iova) {
            MmFreePagesFromMdl(A->Mdl);
            ExFreePool(A->Mdl);
        } else {
            IoFreeMdl(A->Mdl);
        }
    }
    if (A->Kva)
        MmFreeContiguousMemory(A->Kva);
    if (A->Process)
        ObDereferenceObject(A->Process);
    ExFreePoolWithTag(A, V383_POOL_TAG);
}

/*
 * The pages of a buffer. With the IOMMU on: any pages of DRAM (the v2
 * entries take 40-bit addresses), entered in the decoder's tables at a
 * free run of IO addresses, so a multi-megabyte buffer does not need
 * physically contiguous memory, which a running system often does not
 * have (ERROR_NO_SYSTEM_RESOURCES, 1450, on the board for 4 MiB surfaces).
 * Without it: physically contiguous below 4 GiB, as before.
 *
 * Write-combined is Normal non-cacheable on ARM64: no stale cache lines
 * for a device that is not coherent (_CCA 0), and unaligned CPU access
 * still works.
 */
static NTSTATUS
V383AllocPages(
    _In_ PDEVICE_CONTEXT Ctx,
    _Inout_ PV383_ALLOC A
    )
{
    PHYSICAL_ADDRESS low, high, skip;
    NTSTATUS status;
    ULONG iova;

    low.QuadPart = 0;
    skip.QuadPart = 0;
    if (!Ctx->IommuOn) {
        high.QuadPart = 0xFFFFFFFF;
        A->Kva = MmAllocateContiguousNodeMemory(A->Size, low, high, skip,
                                                PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
        if (!A->Kva)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(A->Kva, A->Size);
        A->Pa = MmGetPhysicalAddress(A->Kva);
        A->Dma = (ULONG)A->Pa.QuadPart;
        A->Mdl = IoAllocateMdl(A->Kva, (ULONG)A->Size, FALSE, FALSE, NULL);
        if (!A->Mdl)
            return STATUS_INSUFFICIENT_RESOURCES;
        MmBuildMdlForNonPagedPool(A->Mdl);
        return STATUS_SUCCESS;
    }

    high.QuadPart = (1ll << 40) - 1;
    /* zeroed by Mm (no MM_DONT_ZERO_ALLOCATION) */
    A->Mdl = MmAllocatePagesForMdlEx(low, high, skip, A->Size, MmWriteCombined, MM_ALLOCATE_FULLY_REQUIRED);
    if (!A->Mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
    A->Iova = TRUE;
    if (MmGetMdlByteCount(A->Mdl) != A->Size)
        return STATUS_INSUFFICIENT_RESOURCES;

    ExAcquireFastMutex(&Ctx->MmuLock);
    iova = V383IovaAlloc(Ctx, (ULONG)(A->Size >> PAGE_SHIFT));
    if (!iova) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        status = V383MmuEnter(Ctx, A->Dom, iova, MmGetMdlPfnArray(A->Mdl), 0, (ULONG)(A->Size >> PAGE_SHIFT));
        if (NT_SUCCESS(status))
            A->Dma = iova;
        else
            RtlClearBits(&Ctx->IovaMap, iova >> PAGE_SHIFT, (ULONG)(A->Size >> PAGE_SHIFT));
        /* a previous mapping of these addresses may still be in the TLB */
        V383MmuZap(Ctx);
    }
    ExReleaseFastMutex(&Ctx->MmuLock);
    return status;
}

/* In the caller's context: the mapping goes into its address space. */
static NTSTATUS
V383Alloc(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    VDPU383_ALLOC_IN *in;
    VDPU383_ALLOC_OUT *out;
    PV383_ALLOC a;
    PV383_FILE f = GetFileContext(WdfRequestGetFileObject(Request));
    SIZE_T size;
    NTSTATUS status;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status))
        return status;
    size = ((SIZE_T)in->size + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    if (size == 0 || size > 256u * 1024 * 1024)
        return STATUS_INVALID_PARAMETER;

    /* At most V383_MAX_PER_FILE of decoder buffers per open handle: a
     * 1080p H.264 decoder with 32 surfaces needs about 160 MiB. */
    {
        PLIST_ENTRY e;
        SIZE_T held = 0;
        WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);

        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        for (e = Ctx->Allocs.Flink; e != &Ctx->Allocs; e = e->Flink) {
            PV383_ALLOC h = CONTAINING_RECORD(e, V383_ALLOC, Link);
            if (h->Owner == fo && !h->Import)
                held += h->Size;
        }
        WdfWaitLockRelease(Ctx->AllocLock);
        if (held + size > V383_MAX_PER_FILE)
            return STATUS_QUOTA_EXCEEDED;
    }

    /* a session's buffers are mapped into the process it is for, and are
     * entered in its own directory */
    if (f->Session && PsGetCurrentProcessId() != f->Pid)
        return STATUS_ACCESS_DENIED;
    a = (PV383_ALLOC)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), V383_POOL_TAG);
    if (!a)
        return STATUS_INSUFFICIENT_RESOURCES;
    a->Size = size;
    a->Dom = f->Session ? f->Dom : &Ctx->Dom0;
    status = V383AllocPages(Ctx, a);
    if (!NT_SUCCESS(status)) {
        Ctx->AllocFailures++;
        V383_LOG("alloc of %Iu bytes (%s) failed: 0x%08x", size, Ctx->IommuOn ? "IOMMU" : "contiguous", status);
        V383FreeAlloc(Ctx, a);
        return status;
    }

    __try {
        a->UserVa = MmMapLockedPagesSpecifyCache(a->Mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                                 NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a->UserVa = NULL;
    }
    if (!a->UserVa) {
        V383FreeAlloc(Ctx, a);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    a->Process = PsGetCurrentProcess();
    ObReferenceObject(a->Process);
    a->Owner = WdfRequestGetFileObject(Request);

    WdfWaitLockAcquire(Ctx->AllocLock, NULL);
    a->Handle = ++Ctx->NextHandle;
    InsertTailList(&Ctx->Allocs, &a->Link);
    WdfWaitLockRelease(Ctx->AllocLock);

    out->handle = a->Handle;
    out->dma = a->Dma;
    out->size = (ULONG)size;
    out->flags = a->Iova ? VDPU383_ALLOC_IOVA : 0;
    out->user_va = (ULONGLONG)(ULONG_PTR)a->UserVa;
    *Info = sizeof(*out);
    return STATUS_SUCCESS;
}

static NTSTATUS
V383Free(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request
    )
{
    VDPU383_FREE_IN *in;
    PLIST_ENTRY e;
    PV383_ALLOC found = NULL;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;

    /* under the gate: a decode using the buffer finishes first, and a
     * revoke never meets an import being freed */
    V383GateEnter(Ctx);
    WdfWaitLockAcquire(Ctx->AllocLock, NULL);
    for (e = Ctx->Allocs.Flink; e != &Ctx->Allocs; e = e->Flink) {
        PV383_ALLOC a = CONTAINING_RECORD(e, V383_ALLOC, Link);
        if (a->Handle == in->handle && a->Owner == fo) {
            RemoveEntryList(&a->Link);
            found = a;
            break;
        }
    }
    WdfWaitLockRelease(Ctx->AllocLock);
    V383GateLeave(Ctx);
    if (!found)
        return STATUS_INVALID_HANDLE;
    V383FreeAlloc(Ctx, found);
    return STATUS_SUCCESS;
}

VOID
V383EvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfFileObjectGetDevice(FileObject));
    LIST_ENTRY mine;
    PLIST_ENTRY e, next;

    InitializeListHead(&mine);
    V383GateEnter(ctx);
    WdfWaitLockAcquire(ctx->AllocLock, NULL);
    for (e = ctx->Allocs.Flink; e != &ctx->Allocs; e = next) {
        PV383_ALLOC a = CONTAINING_RECORD(e, V383_ALLOC, Link);
        next = e->Flink;
        if (a->Owner == FileObject) {
            RemoveEntryList(&a->Link);
            InsertTailList(&mine, &a->Link);
        }
    }
    WdfWaitLockRelease(ctx->AllocLock);
    V383GateLeave(ctx);
    while (!IsListEmpty(&mine))
        V383FreeAlloc(ctx, CONTAINING_RECORD(RemoveHeadList(&mine), V383_ALLOC, Link));
    {
        PV383_FILE f = GetFileContext(FileObject);
        WdfWaitLockAcquire(ctx->AllocLock, NULL);
        if (f->CountedUser) {
            f->CountedUser = FALSE;
            ctx->UserOpens--;
        }
        WdfWaitLockRelease(ctx->AllocLock);
    }
}

/*
 * A new file object. From kernel mode: a session (vdpu383_check.h) for
 * the process the opener runs in (maliwddm opens it in its escape, in the
 * caller's context), with a directory of its own when the IOMMU is on; at
 * most V383_MAX_SESSIONS at once. From user mode with UserAccess 0: one
 * handle at a time, as the exclusive device was before sessions.
 */
VOID
V383EvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    PV383_FILE f = GetFileContext(FileObject);
    PV383_DOMAIN dom = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    RtlZeroMemory(f, sizeof(*f));
    if (WdfRequestGetRequestorMode(Request) == KernelMode) {
        if (ctx->IommuOn) {
            dom = V383DomainCreate();
            if (!dom)
                status = STATUS_INSUFFICIENT_RESOURCES;
        }
        WdfWaitLockAcquire(ctx->AllocLock, NULL);
        if (NT_SUCCESS(status) && ctx->Sessions >= V383_MAX_SESSIONS)
            status = STATUS_INSUFFICIENT_RESOURCES;
        if (NT_SUCCESS(status)) {
            ctx->Sessions++;
            ctx->SessionsMade++;
            f->Session = TRUE;
            f->Pid = PsGetCurrentProcessId();
            f->Dom = dom;
            dom = NULL;
        } else {
            ctx->SessionRefused++;
        }
        WdfWaitLockRelease(ctx->AllocLock);
        if (dom) {
            V383DomainTablesFree(dom);
            ExFreePoolWithTag(dom, V383_POOL_TAG);
        }
    } else if (ctx->UserAccess == 0) {
        WdfWaitLockAcquire(ctx->AllocLock, NULL);
        if (ctx->UserOpens)
            status = STATUS_SHARING_VIOLATION;
        else {
            ctx->UserOpens++;
            f->CountedUser = TRUE;
        }
        WdfWaitLockRelease(ctx->AllocLock);
    }
    WdfRequestComplete(Request, status);
}

/* After the cleanup (every buffer of the file is gone): a session's
 * directory. */
VOID
V383EvtFileClose(
    _In_ WDFFILEOBJECT FileObject
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfFileObjectGetDevice(FileObject));
    PV383_FILE f = GetFileContext(FileObject);

    if (!f->Session)
        return;
    WdfWaitLockAcquire(ctx->AllocLock, NULL);
    ctx->Sessions--;
    WdfWaitLockRelease(ctx->AllocLock);
    if (f->Dom) {
        V383GateEnter(ctx);
        V383DomainDestroy(ctx, f->Dom);
        V383GateLeave(ctx);
        f->Dom = NULL;
    }
    f->Session = FALSE;
}

/* Is [addr] inside a buffer this file allocated or imported, or (not for a
 * session, whose directory does not have it) the SRAM? V383_ADDR_DEAD:
 * inside an import its exporter took back. AllocLock held. */
typedef struct _V383_LOOKUP_CTX {
    PDEVICE_CONTEXT Ctx;
    WDFFILEOBJECT   Fo;
    BOOLEAN         Session;
} V383_LOOKUP_CTX;

static unsigned
V383AddrCheck(
    _In_ void *C,
    _In_ unsigned Addr
    )
{
    V383_LOOKUP_CTX *l = (V383_LOOKUP_CTX *)C;
    PDEVICE_CONTEXT ctx = l->Ctx;
    PLIST_ENTRY e;

    if (Addr == 0)
        return V383_ADDR_OK;
    if (!l->Session && ctx->SramPresent && Addr >= ctx->SramPa.QuadPart &&
        Addr < ctx->SramPa.QuadPart + ctx->SramLen)
        return V383_ADDR_OK;
    for (e = ctx->Allocs.Flink; e != &ctx->Allocs; e = e->Flink) {
        PV383_ALLOC a = CONTAINING_RECORD(e, V383_ALLOC, Link);
        if (a->Owner == l->Fo && Addr >= a->Dma && (ULONG64)Addr < (ULONG64)a->Dma + a->Size)
            return a->Dead ? V383_ADDR_DEAD : V383_ADDR_OK;
    }
    return V383_ADDR_BAD;
}

/* --------------------------------------------------------- decode */

static NTSTATUS
V383RunGated(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT *out;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    PV383_FILE f = GetFileContext(fo);
    LARGE_INTEGER timeout, t0, t1, freq;
    NTSTATUS status, wait;
    BOOLEAN timedOut, faulted, didReset = FALSE;
    ULONG r;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status))
        return status;
    /* METHOD_BUFFERED: in and out are the same system buffer */
    {
        static VDPU383_RUN_IN copy;     /* the queue is sequential */
        RtlCopyMemory(&copy, in, sizeof(copy));
        in = &copy;
    }
    if (f->Session)
        v383_session_sanitize_run(in);
    RtlZeroMemory(out, sizeof(*out));
    *Info = sizeof(*out);

    if (!Ctx->InterruptsConnected) {
        out->result = VDPU383_RUN_BUSY;
        return STATUS_SUCCESS;
    }

    /* With users let in, nobody may skip the check; a session never. */
    if (!(in->flags & VDPU383_RUN_NO_ADDR_CHECK) || Ctx->UserAccess || f->Session) {
        V383_LOOKUP_CTX l;
        unsigned bad = 0;

        l.Ctx = Ctx;
        l.Fo = fo;
        l.Session = f->Session;
        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        out->result = v383_check_addrs(in->regs, in->write_mask, V383AddrCheck, &l, &bad);
        WdfWaitLockRelease(Ctx->AllocLock);
        out->bad_reg = bad;
        if (out->result)
            return STATUS_SUCCESS;
    }

    /* The directory with this file's buffers: a session's own, or the
     * device's (V383UseDomain does nothing when it is already in use). */
    if (f->Session && !f->Dom) {
        out->result = VDPU383_RUN_BAD_ADDRESS;  /* IOMMU off: sessions never decode */
        return STATUS_SUCCESS;
    }
    V383UseDomain(Ctx, f->Session ? f->Dom : &Ctx->Dom0);
    if (f->Session)
        Ctx->SessionRuns++;

    /* Paging lost (an IP reset, or a core that reset itself after an
     * error): back on before the decoder is given IO addresses. */
    if (Ctx->IommuOn && Ctx->MmuLive &&
        (!(V383Rd(Ctx, V383_MMU0_OFFSET + RK_MMU_STATUS) & RK_MMU_STATUS_PAGING) ||
         !(V383Rd(Ctx, V383_MMU1_OFFSET + RK_MMU_STATUS) & RK_MMU_STATUS_PAGING))) {
        V383_LOG("IOMMU paging found off before a decode (0x%08x/0x%08x); restoring",
                 V383Rd(Ctx, V383_MMU0_OFFSET + RK_MMU_STATUS), V383Rd(Ctx, V383_MMU1_OFFSET + RK_MMU_STATUS));
        V383MmuRestore(Ctx);
    }

    Ctx->Runs++;
    if (Ctx->Runs == 1)
        V383Crumb(Ctx, L"Run: first decode of the session, starting");
    KeClearEvent(&Ctx->RunDone);
    Ctx->RunIrq = 0;
    Ctx->RunStatus = 0;
    InterlockedExchange(&Ctx->RunMmuFaults, 0);
    Ctx->RunFaultAddr = 0;
    Ctx->RunFaultStatus = 0;

    for (r = 0; r < V383_NUM_REGS; r++)
        if (in->write_mask[r / 32] & (1u << (r % 32)))
            V383Wr(Ctx, V383_FUNC_OFFSET + 4 * r, in->regs[r]);

    /* buffers written by user mode reach memory before the decoder reads */
    KeMemoryBarrier();
    __dsb(_ARM64_BARRIER_SY);

    V383CacheClear(Ctx);
    InterlockedExchange(&Ctx->RunPending, 1);
    t0 = KeQueryPerformanceCounter(&freq);
    /* rkvdec_h264_run(): timeout, IP enable, DEC_E */
    V383Wr(Ctx, V383_LINK_IP_TIMEOUT, in->link_ip_timeout);
    V383Wr(Ctx, V383_LINK_IP_ENABLE, in->link_ip_enable);
    V383Wr(Ctx, V383_LINK_DEC_E, 1);

    timeout.QuadPart = -10000LL * (in->timeout_ms ? in->timeout_ms : 1000);
    wait = KeWaitForSingleObject(&Ctx->RunDone, Executive, KernelMode, FALSE, &timeout);
    t1 = KeQueryPerformanceCounter(NULL);
    __dsb(_ARM64_BARRIER_SY);

    out->elapsed_us = (ULONG)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
    /* The DPC may have taken RunPending between the wait timing out and
     * here: then the decode did finish. */
    timedOut = (wait == STATUS_TIMEOUT && InterlockedExchange(&Ctx->RunPending, 0)) ? TRUE : FALSE;
    faulted = Ctx->RunMmuFaults != 0;
    if (faulted) {
        /* the IOMMU DPC ended the wait, or the fault came with the decoder's
         * interrupt or before the timeout: the core is stopped below */
        InterlockedExchange(&Ctx->RunPending, 0);
        if (timedOut)
            Ctx->Timeouts++;
        out->result = VDPU383_RUN_MMU_FAULT;
        out->bad_reg = Ctx->RunFaultAddr;
        out->irq = Ctx->RunIrq ? Ctx->RunIrq : V383Rd(Ctx, V383_LINK_IRQ);
        out->status = Ctx->RunStatus ? Ctx->RunStatus : V383Rd(Ctx, V383_LINK_STATUS);
    } else if (timedOut) {
        Ctx->Timeouts++;
        out->result = VDPU383_RUN_TIMEOUT;
        out->irq = V383Rd(Ctx, V383_LINK_IRQ);
        out->status = V383Rd(Ctx, V383_LINK_STATUS);
    } else {
        out->irq = Ctx->RunIrq;
        out->status = Ctx->RunStatus;
        out->result = (out->status & V383_STA_ERROR_MASK) || !(out->status & V383_STA_FRAME_RDY) ?
                      VDPU383_RUN_HW_ERROR : VDPU383_RUN_OK;
    }
    out->mmu_status[0] = V383Rd(Ctx, V383_MMU0_OFFSET + RK_MMU_STATUS);
    out->mmu_status[1] = V383Rd(Ctx, V383_MMU1_OFFSET + RK_MMU_STATUS);
    out->mmu_int_rawstat[0] = V383Rd(Ctx, V383_MMU0_OFFSET + RK_MMU_INT_RAWSTAT);
    out->mmu_int_rawstat[1] = V383Rd(Ctx, V383_MMU1_OFFSET + RK_MMU_INT_RAWSTAT);

    if (in->flags & VDPU383_RUN_READBACK)
        for (r = 0; r < V383_NUM_REGS; r++)
            out->regs[r] = V383Rd(Ctx, V383_FUNC_OFFSET + 4 * r);

    /*
     * A timed-out core is stopped whatever the caller asked: left running,
     * it keeps writing to buffers the caller may free next (mainline
     * "Stop the VDPU383 core on timeout"; before it, a timed-out core kept
     * running there too). An error status resets only when asked.
     */
    if (timedOut || faulted || (out->result != VDPU383_RUN_OK && (in->flags & VDPU383_RUN_RESET_ON_ERROR))) {
        WdfInterruptAcquireLock(Ctx->DecInterrupt);
        out->reset_status = V383IpReset(Ctx);
        WdfInterruptReleaseLock(Ctx->DecInterrupt);
        didReset = TRUE;
        /* the reset took the IOMMU with it; on a fault, also unmasks its
         * interrupt (rkvdec_iommu_restore() after every vdpu383 error) */
        V383MmuRestore(Ctx);
    } else if (out->result != VDPU383_RUN_OK) {
        V383MmuRestore(Ctx);
    }
    /*
     * An interrupt taken just before the reset may still have its DPC
     * queued; drained here, it cannot complete the next run (mainline
     * "Handle interrupts without a current context" closes the same race
     * between its watchdog and its interrupt thread).
     */
    if (timedOut || faulted)
        KeFlushQueuedDpcs();
    /* whether an IP reset undoes the warm-up is not known: run it again */
    if (didReset)
        V383WarmupNow(Ctx, L"Run: warm-up after the IP reset");
    if (faulted)
        V383Fact(Ctx, L"LastMmuFaultAddr", out->bad_reg);
    if (Ctx->Runs == 1) {
        V383Fact(Ctx, L"FirstRunResult", out->result);
        V383Fact(Ctx, L"FirstRunStatus", out->status);
        V383Crumb(Ctx, L"Run: first decode returned");
    }
    return STATUS_SUCCESS;
}

/* One decode under the gate: from the address check until the decoder is
 * done (or stopped), no import's pages can be taken back. */
static NTSTATUS
V383Run(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    NTSTATUS status;

    V383GateEnter(Ctx);
    status = V383RunGated(Ctx, Request, Info);
    V383GateLeave(Ctx);
    return status;
}

/* ------------------------------------------------------------ imports */

/*
 * The exporter takes an import's pages back (vdpu383_import.h): the decode
 * in progress, if any, ends first (the gate), then the entries are cleared
 * and the import is dead; its IO addresses stay reserved for a refresh.
 * PASSIVE_LEVEL, from the exporter, holding none of this driver's locks.
 */
static VOID
V383ImportRevoke(
    _In_ PVOID Context,
    _In_ ULONG64 Cookie
    )
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    PLIST_ENTRY e;
    PV383_ALLOC found = NULL;
    BOOLEAN live = FALSE;

    V383GateEnter(ctx);
    WdfWaitLockAcquire(ctx->AllocLock, NULL);
    for (e = ctx->Allocs.Flink; e != &ctx->Allocs; e = e->Flink) {
        PV383_ALLOC a = CONTAINING_RECORD(e, V383_ALLOC, Link);
        if (a->Import && a->Handle == (ULONG)Cookie) {
            found = a;
            break;
        }
    }
    if (found) {
        live = !found->Dead;
        found->Dead = TRUE;
    }
    WdfWaitLockRelease(ctx->AllocLock);
    if (live) {
        ExAcquireFastMutex(&ctx->MmuLock);
        V383MmuClear(ctx, found->Dom, found->Dma, (ULONG)(found->Size >> PAGE_SHIFT));
        V383MmuZap(ctx);
        ExReleaseFastMutex(&ctx->MmuLock);
        ctx->Revokes++;
    }
    V383GateLeave(ctx);
}

/*
 * The last import that failed, in Device Parameters, for a board with no
 * debugger (README.md): LastImportStage (V383_IMPORT_STAGE_*),
 * LastImportStatus, LastImportPages (asked for), LastImportExporterPages
 * and LastImportExporterStatus (the exporter's answer), LastImportHandle
 * (0: a new import), LastImportPid, ImportFailures, Imports. Written for
 * the first 16 failures and every 64th after; PASSIVE_LEVEL, no lock held.
 */
#define V383_IMPORT_STAGE_PARAMS        1   /* IOMMU off, token 0, pages 0 or over 256 MiB */
#define V383_IMPORT_STAGE_NOMEM         2   /* the PFN array or the record */
#define V383_IMPORT_STAGE_HANDLE        3   /* refresh: no such import, another size, another token */
#define V383_IMPORT_STAGE_IOVA          4   /* no free run of IO addresses */
#define V383_IMPORT_STAGE_NO_EXPORTER   5   /* no callback object: maliwddm's exporter is not running */
#define V383_IMPORT_STAGE_EXPORTER      6   /* the exporter refused (its status) */
#define V383_IMPORT_STAGE_PAGES         7   /* the exporter's page count is not the one asked for */
#define V383_IMPORT_STAGE_MMU           8   /* the IOMMU entries (a page table, an address over 40 bits) */

static VOID
V383ImportNote(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Stage,
    _In_ NTSTATUS Status,
    _In_ ULONG Pages,
    _In_ ULONG Handle,
    _In_ const VDPU383_IMPORT_REQUEST *R
    )
{
    WDFKEY key;
    ULONG n = ++Ctx->ImportFailures;

    V383_LOG("import of %u pages (handle %u) failed at stage %u: 0x%08x (exporter 0x%08x, %u pages)",
             Pages, Handle, Stage, Status, R->Status, R->Pages);
    if (n > 16 && n % 64)
        return;
    if (!V383OpenParams(Ctx, KEY_WRITE, &key))
        return;
    V383PutULong(key, L"LastImportStage", Stage);
    V383PutULong(key, L"LastImportStatus", (ULONG)Status);
    V383PutULong(key, L"LastImportPages", Pages);
    V383PutULong(key, L"LastImportExporterPages", R->Pages);
    V383PutULong(key, L"LastImportExporterStatus", (ULONG)R->Status);
    V383PutULong(key, L"LastImportHandle", Handle);
    V383PutULong(key, L"LastImportPid", (ULONG)(ULONG_PTR)PsGetCurrentProcessId());
    V383PutULong(key, L"ImportFailures", n);
    V383PutULong(key, L"Imports", Ctx->Imports);
    (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    WdfRegistryClose(key);
}

/*
 * IOCTL_VDPU383_IMPORT, in the caller's context (the exporter checks the
 * process its token was given to). A new import reserves its IO addresses
 * and goes into the list dead, so the address check refuses it until its
 * pages are entered; a refresh enters new pages at the same addresses.
 */
static NTSTATUS
V383Import(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    VDPU383_IMPORT_IN *in;
    VDPU383_IMPORT_OUT *out;
    VDPU383_IMPORT_REQUEST r;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    PV383_FILE f = GetFileContext(fo);
    PV383_ALLOC a = NULL;
    PPFN_NUMBER pfns;
    PLIST_ENTRY e;
    ULONG64 token;
    ULONG pages, handle, stage = V383_IMPORT_STAGE_PARAMS;
    BOOLEAN created = FALSE, imported = FALSE;
    NTSTATUS status;

    *Info = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status))
        return status;
    /* METHOD_BUFFERED: in and out are the same system buffer */
    token = in->token;
    pages = in->pages;
    handle = in->handle;
    RtlZeroMemory(&r, sizeof(r));
    if (!Ctx->IommuOn) {
        V383ImportNote(Ctx, stage, STATUS_NOT_SUPPORTED, pages, handle, &r);
        return STATUS_NOT_SUPPORTED;
    }
    if (token == 0 || pages == 0 || pages > ((256u * 1024 * 1024) >> PAGE_SHIFT)) {
        V383ImportNote(Ctx, stage, STATUS_INVALID_PARAMETER, pages, handle, &r);
        return STATUS_INVALID_PARAMETER;
    }
    /* a session imports only what its own process was given */
    if (f->Session && (!f->Dom || PsGetCurrentProcessId() != f->Pid)) {
        V383ImportNote(Ctx, stage, STATUS_ACCESS_DENIED, pages, handle, &r);
        return STATUS_ACCESS_DENIED;
    }
    pfns = (PPFN_NUMBER)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)pages * sizeof(PFN_NUMBER), V383_POOL_TAG);
    if (!pfns) {
        V383ImportNote(Ctx, V383_IMPORT_STAGE_NOMEM, STATUS_INSUFFICIENT_RESOURCES, pages, handle, &r);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    V383GateEnter(Ctx);
    if (handle) {
        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        for (e = Ctx->Allocs.Flink; e != &Ctx->Allocs; e = e->Flink) {
            PV383_ALLOC h = CONTAINING_RECORD(e, V383_ALLOC, Link);
            if (h->Import && h->Handle == handle && h->Owner == fo) {
                a = h;
                break;
            }
        }
        WdfWaitLockRelease(Ctx->AllocLock);
        stage = V383_IMPORT_STAGE_HANDLE;
        if (!a || a->Size != ((SIZE_T)pages << PAGE_SHIFT)) {
            status = STATUS_INVALID_HANDLE;
            goto fail;
        }
        if (!a->Dead) {
            /* still mapped: nothing to refresh */
            status = a->Token == token ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
            if (!NT_SUCCESS(status))
                goto fail;
            goto done;
        }
    } else {
        stage = V383_IMPORT_STAGE_NOMEM;
        a = (PV383_ALLOC)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), V383_POOL_TAG);
        if (!a) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }
        a->Import = TRUE;
        a->Dead = TRUE;
        a->Iova = TRUE;
        a->Size = (SIZE_T)pages << PAGE_SHIFT;
        a->Owner = fo;
        a->Dom = f->Session ? f->Dom : &Ctx->Dom0;
        stage = V383_IMPORT_STAGE_IOVA;
        ExAcquireFastMutex(&Ctx->MmuLock);
        a->Dma = V383IovaAlloc(Ctx, pages);
        ExReleaseFastMutex(&Ctx->MmuLock);
        if (!a->Dma) {
            ExFreePoolWithTag(a, V383_POOL_TAG);
            a = NULL;
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }
        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        a->Handle = ++Ctx->NextHandle;
        InsertTailList(&Ctx->Allocs, &a->Link);
        WdfWaitLockRelease(Ctx->AllocLock);
        created = TRUE;
    }

    r.Token = token;
    r.ProcessId = f->Session ? f->Pid : PsGetCurrentProcessId();
    r.Cookie = a->Handle;
    r.Revoke = V383ImportRevoke;
    r.RevokeContext = Ctx;
    r.Pfns = pfns;
    r.MaxPages = pages;
    stage = V383_IMPORT_STAGE_EXPORTER;
    if (!V383ExporterCall(VDPU383_IMPORT_OP_IMPORT, &r))
        stage = V383_IMPORT_STAGE_NO_EXPORTER;
    if (!NT_SUCCESS(r.Status)) {
        status = r.Status;
        goto fail;
    }
    imported = TRUE;
    if (r.Pages != pages) {
        stage = V383_IMPORT_STAGE_PAGES;
        status = STATUS_INVALID_PARAMETER;
        goto fail;
    }
    stage = V383_IMPORT_STAGE_MMU;
    ExAcquireFastMutex(&Ctx->MmuLock);
    status = V383MmuEnter(Ctx, a->Dom, a->Dma, pfns, 0, pages);
    /* a previous mapping of these addresses may still be in the TLB */
    V383MmuZap(Ctx);
    ExReleaseFastMutex(&Ctx->MmuLock);
    if (!NT_SUCCESS(status))
        goto fail;
    WdfWaitLockAcquire(Ctx->AllocLock, NULL);
    a->Token = token;
    a->Dead = FALSE;
    WdfWaitLockRelease(Ctx->AllocLock);
    Ctx->Imports++;

done:
    out->handle = a->Handle;
    out->dma = a->Dma;
    out->size = (ULONG)a->Size;
    out->flags = VDPU383_ALLOC_IOVA | VDPU383_ALLOC_IMPORTED;
    V383GateLeave(Ctx);
    ExFreePoolWithTag(pfns, V383_POOL_TAG);
    *Info = sizeof(*out);
    return STATUS_SUCCESS;

fail:
    if (created) {
        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        RemoveEntryList(&a->Link);
        WdfWaitLockRelease(Ctx->AllocLock);
    }
    V383GateLeave(Ctx);
    /* the exporter holds our revoke routine for this token: given back
     * with no lock of ours held */
    if (imported)
        V383ImportRelease(token, a->Handle);
    if (created) {
        V383MmuUnmapRange(Ctx, a->Dom, a->Dma, a->Size);
        ExFreePoolWithTag(a, V383_POOL_TAG);
    }
    ExFreePoolWithTag(pfns, V383_POOL_TAG);
    V383ImportNote(Ctx, stage, status, pages, handle, &r);
    return status;
}

/* ---------------------------------------------------------- dispatch */

static BOOLEAN
V383RegRangeOk(
    _In_ ULONG Offset,
    _In_ ULONG Count
    )
{
    return !(Offset & 3) && Count <= V383_REG_BLOCK_SIZE / 4 && Offset <= V383_REG_BLOCK_SIZE &&
           Offset + Count * 4 <= V383_REG_BLOCK_SIZE;
}

VOID
V383EvtIoInCallerContext(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    WDF_REQUEST_PARAMETERS params;
    ULONG_PTR info = 0;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (params.Type == WdfRequestTypeDeviceControl) {
        PV383_FILE f = GetFileContext(WdfRequestGetFileObject(Request));
        ULONG code = params.Parameters.DeviceIoControl.IoControlCode;

        /* A session: the calls of vdpu383_check.h only, and with the IOMMU
         * off (no directory of its own) only GET_INFO. */
        if (f->Session && (!v383_session_ioctl_ok(code) || (!f->Dom && code != IOCTL_VDPU383_GET_INFO))) {
            WdfWaitLockAcquire(ctx->AllocLock, NULL);
            ctx->SessionRefused++;
            WdfWaitLockRelease(ctx->AllocLock);
            WdfRequestComplete(Request, f->Dom ? STATUS_ACCESS_DENIED : STATUS_NOT_SUPPORTED);
            return;
        }
        switch (code) {
        case IOCTL_VDPU383_ALLOC:
            status = V383Alloc(ctx, Request, &info);
            WdfRequestCompleteWithInformation(Request, status, info);
            return;
        case IOCTL_VDPU383_FREE:
            status = V383Free(ctx, Request);
            WdfRequestComplete(Request, status);
            return;
        case IOCTL_VDPU383_IMPORT:
            status = V383Import(ctx, Request, &info);
            WdfRequestCompleteWithInformation(Request, status, info);
            return;
        default:
            break;
        }
    }
    status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(status))
        WdfRequestComplete(Request, status);
}

VOID
V383EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    ULONG i;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_VDPU383_GET_INFO: {
        VDPU383_INFO info2, *o = &info2;
        PVOID ob;
        size_t olen;
        /* a version 1 program passes the structure without the IOMMU fields */
        status = WdfRequestRetrieveOutputBuffer(Request, FIELD_OFFSET(VDPU383_INFO, iommu), &ob, &olen);
        if (!NT_SUCCESS(status))
            break;
        RtlZeroMemory(o, sizeof(*o));
        o->interface_version = VDPU383_INTERFACE_VERSION;
        o->reg_version = V383Rd(ctx, V383_FUNC_OFFSET);
        for (i = 0; i < 24; i++)
            o->link[i] = V383Rd(ctx, i * 4);
        o->mmu_status[0] = V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_STATUS);
        o->mmu_status[1] = V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_STATUS);
        o->mmu_dte[0] = V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_DTE_ADDR);
        o->mmu_dte[1] = V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_DTE_ADDR);
        o->mmu_int_rawstat[0] = V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_INT_RAWSTAT);
        o->mmu_int_rawstat[1] = V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_INT_RAWSTAT);
        o->sram_present = ctx->SramPresent;
        o->sram_dma = (ULONG)ctx->SramPa.QuadPart;
        o->sram_size = ctx->SramLen;
        o->irq_count = (ULONG)ctx->IrqCount;
        o->mmu_irq_count = (ULONG)ctx->MmuIrqCount;
        o->last_irq = ctx->LastIrq;
        o->last_status = ctx->LastStatus;
        o->mmu_last_status[0] = ctx->MmuLastStatus[0];
        o->mmu_last_status[1] = ctx->MmuLastStatus[1];
        o->mmu_last_fault_addr[0] = ctx->MmuLastFault[0];
        o->mmu_last_fault_addr[1] = ctx->MmuLastFault[1];
        o->runs = ctx->Runs;
        o->timeouts = ctx->Timeouts;
        o->warmup = ctx->Warm;
        o->iommu = ctx->IommuOn;
        o->iommu_setting = ctx->IommuSetting;
        o->iommu_dt_dma = (ULONG)ctx->Dom0.DtPa.QuadPart;
        o->iova_pages = ctx->IovaPagesMapped;
        o->mmu_faults = (ULONG)ctx->MmuFaults;
        o->mmu_enables = ctx->MmuEnables;
        o->alloc_failures = ctx->AllocFailures;
        o->imports = ctx->Imports;
        o->revokes = ctx->Revokes;
        o->import_failures = ctx->ImportFailures;
        o->sessions = ctx->Sessions;
        o->session_runs = ctx->SessionRuns;
        o->sessions_made = ctx->SessionsMade;
        o->session_refused = ctx->SessionRefused;
        o->domain_switches = ctx->DomainSwitches;
        o->user_access = ctx->UserAccess;
        if (GetFileContext(WdfRequestGetFileObject(Request))->Session)
            v383_session_scrub_info(o);
        if (olen > sizeof(*o))
            olen = sizeof(*o);
        RtlCopyMemory(ob, o, olen);
        info = olen;
        break;
    }
    case IOCTL_VDPU383_WARMUP: {
        VDPU383_WARMUP_RESULT *o;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*o), (PVOID *)&o, NULL);
        if (!NT_SUCCESS(status))
            break;
        V383UseDomain(ctx, &ctx->Dom0);
        V383Crumb(ctx, L"IOCTL warm-up: starting");
        /* the warm-up masks the decoder interrupt (link 0x58 bit 15) and
         * polls; hold the interrupt lock so the ISR cannot run in between */
        if (ctx->InterruptsConnected)
            WdfInterruptAcquireLock(ctx->DecInterrupt);
        V383WarmupRun(ctx, o);
        if (ctx->InterruptsConnected)
            WdfInterruptReleaseLock(ctx->DecInterrupt);
        V383Fact(ctx, L"WarmupIrq", o->irq);
        V383Fact(ctx, L"WarmupStatus", o->status);
        V383Fact(ctx, L"WarmupTimedOut", o->timed_out);
        V383Crumb(ctx, L"IOCTL warm-up: returned");
        info = sizeof(*o);
        break;
    }
    case IOCTL_VDPU383_RESET: {
        ULONG *o;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*o), (PVOID *)&o, NULL);
        if (!NT_SUCCESS(status))
            break;
        if (ctx->InterruptsConnected)
            WdfInterruptAcquireLock(ctx->DecInterrupt);
        *o = V383IpReset(ctx);
        if (ctx->InterruptsConnected)
            WdfInterruptReleaseLock(ctx->DecInterrupt);
        V383MmuRestore(ctx);
        V383WarmupNow(ctx, L"IOCTL reset: warm-up after the IP reset");
        info = sizeof(*o);
        break;
    }
    case IOCTL_VDPU383_READ_REGS: {
        VDPU383_READ_REGS_IN *in;
        ULONG *o, n;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (!NT_SUCCESS(status))
            break;
        n = in->count;
        if (!V383RegRangeOk(in->offset, n)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        {
            ULONG off = in->offset;     /* in and out share the buffer */
            status = WdfRequestRetrieveOutputBuffer(Request, (size_t)n * 4, (PVOID *)&o, NULL);
            if (!NT_SUCCESS(status))
                break;
            for (i = 0; i < n; i++)
                o[i] = V383Rd(ctx, off + i * 4);
        }
        info = (ULONG_PTR)n * 4;
        break;
    }
    case IOCTL_VDPU383_WRITE_REGS: {
        VDPU383_WRITE_REGS_IN *in;
        size_t len;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &len);
        if (!NT_SUCCESS(status))
            break;
        if (in->count == 0 || in->count > VDPU383_MAX_REG_OPS ||
            len < sizeof(*in) + (in->count - 1) * sizeof(VDPU383_REG_OP)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        for (i = 0; i < in->count; i++)
            if (!V383RegRangeOk(in->ops[i].offset, 1)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
        if (!NT_SUCCESS(status))
            break;
        for (i = 0; i < in->count; i++)
            V383Wr(ctx, in->ops[i].offset, in->ops[i].value);
        break;
    }
    case IOCTL_VDPU383_RUN:
        status = V383Run(ctx, Request, &info);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}

/* ------------------------------------------------------------- setup */

NTSTATUS
V383EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_FILEOBJECT_CONFIG fcfg;
    WDF_INTERRUPT_CONFIG icfg;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\VDPU383");
    DECLARE_CONST_UNICODE_STRING(link, VDPU383_DOS_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    /* UserAccess 1: users may open the device too, so that a video
     * player's process (the display driver's user-mode part decoding for
     * D3D11 video) reaches it; 2: also low-integrity processes (a browser's
     * GPU process). Off by default: the bring-up interface lets the caller
     * program the decoder's registers, checked only as far as V383Run's
     * address check goes. 2 also grants RESTRICTED (RC) and ALL
     * APPLICATION PACKAGES (AC): Chromium's GPU process runs with a
     * restricted token (the restricting SIDs are checked too) and may run
     * in an AppContainer. */
    DECLARE_CONST_UNICODE_STRING(sddl_users, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)");
    DECLARE_CONST_UNICODE_STRING(sddl_low,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)(A;;GRGW;;;RC)(A;;GRGW;;;AC)S:(ML;;NW;;;LW)");
    ULONG userAccess = 0;

    UNREFERENCED_PARAMETER(Driver);

    {
        WDFKEY key;
        DECLARE_CONST_UNICODE_STRING(n, L"UserAccess");
        if (NT_SUCCESS(WdfFdoInitOpenRegistryKey(DeviceInit, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                                 WDF_NO_OBJECT_ATTRIBUTES, &key))) {
            if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &userAccess)))
                userAccess = 0;
            WdfRegistryClose(key);
        }
        if (userAccess > 2)
            userAccess = 0;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = V383EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = V383EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = V383EvtD0Entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = V383EvtD0EntryPostInterrupts;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = V383EvtD0ExitPreInterrupts;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG_INIT(&fcfg, V383EvtFileCreate, V383EvtFileClose, V383EvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, V383_FILE);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fcfg, &attr);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, V383EvtIoInCallerContext);
    /* Not exclusive: the display driver opens a session for each process
     * that decodes. With UserAccess 0 V383EvtFileCreate still lets one
     * user-mode handle (the test tool) in at a time; with UserAccess every
     * player process opens it for itself. */
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    status = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfDeviceInitAssignSDDLString(DeviceInit,
                                           userAccess == 2 ? &sddl_low :
                                           userAccess == 1 ? &sddl_users : &sddl);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    attr.EvtCleanupCallback = V383EvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;

    ctx = GetContext(device);
    ctx->Device = device;
    ctx->UserAccess = userAccess;
    ctx->ActiveDom = &ctx->Dom0;
    InitializeListHead(&ctx->Allocs);
    KeInitializeEvent(&ctx->RunDone, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->RunGate, SynchronizationEvent, TRUE);
    ExInitializeFastMutex(&ctx->MmuLock);
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->AllocLock);
    if (!NT_SUCCESS(status))
        return status;

    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status))
        return status;

    /* interrupt objects take the interrupt resources in creation order:
     * GSIV 340 (decoder), then 341 (IOMMU) */
    WDF_INTERRUPT_CONFIG_INIT(&icfg, V383EvtDecIsr, V383EvtDecDpc);
    status = WdfInterruptCreate(device, &icfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DecInterrupt);
    if (!NT_SUCCESS(status))
        return status;
    WDF_INTERRUPT_CONFIG_INIT(&icfg, V383EvtMmuIsr, V383EvtMmuDpc);
    status = WdfInterruptCreate(device, &icfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->MmuInterrupt);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoDeviceControl = V383EvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ExecutionLevel = WdfExecutionLevelPassive;     /* RUN waits */
    status = WdfIoQueueCreate(device, &qcfg, &attr, &ctx->Queue);
    return status;
}

/* After every file object, so after every buffer: the IOMMU tables. */
VOID
V383EvtDeviceCleanup(
    _In_ WDFOBJECT Object
    )
{
    V383MmuTablesFree(GetContext((WDFDEVICE)Object));
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG cfg;

    WDF_DRIVER_CONFIG_INIT(&cfg, V383EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
