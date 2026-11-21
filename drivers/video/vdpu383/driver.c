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
 * interrupt is acknowledged. The warm-up decode at D0 entry is the vendor
 * kernel's (warmup.c). The IOMMU is left with paging off, or turned off if
 * it is found on, so the decoder's addresses are physical; buffers are
 * therefore allocated below 4 GiB.
 *
 * Only administrators and SYSTEM can open the device, and one handle at a
 * time. Every address register of a decode is checked against the
 * caller's buffers before the decoder is started, unless the caller asks
 * not to.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
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
EVT_WDF_IO_IN_CALLER_CONTEXT V383EvtIoInCallerContext;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL V383EvtIoDeviceControl;
EVT_WDF_FILE_CLEANUP V383EvtFileCleanup;

#define LINK_IRQ_CLEAR      0x00030000      /* FIELD_PREP_WM16(irq | line irq, 0) */
#define LINK_STATUS_CLEAR   0x03FF0000      /* FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0) */

/* ------------------------------------------------------------ IOMMU */

static VOID
V383MmuPagingOff(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Base
    )
{
    ULONG st = V383Rd(Ctx, Base + RK_MMU_STATUS);
    ULONG i;

    if (!(st & RK_MMU_STATUS_PAGING))
        return;

    /* rk_iommu_disable(): stall, paging off, unstall */
    V383_LOG("IOMMU at +0x%x has paging on (status 0x%08x); turning it off", Base, st);
    V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_ENABLE_STALL);
    for (i = 0; i < 1000 && !(V383Rd(Ctx, Base + RK_MMU_STATUS) & RK_MMU_STATUS_STALL); i++)
        KeStallExecutionProcessor(1);
    V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_DISABLE_PAGING);
    for (i = 0; i < 1000 && (V383Rd(Ctx, Base + RK_MMU_STATUS) & RK_MMU_STATUS_PAGING); i++)
        KeStallExecutionProcessor(1);
    V383Wr(Ctx, Base + RK_MMU_COMMAND, RK_MMU_CMD_DISABLE_STALL);
    V383_LOG("IOMMU at +0x%x: status now 0x%08x", Base, V383Rd(Ctx, Base + RK_MMU_STATUS));
}

/* ------------------------------------------------------------ reset */

/* rkvdec_vdpu383_reset() in the vendor kernel: IP reset through the link
 * block, which resets the core and the IOMMU. Returns link status. */
static ULONG
V383IpReset(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONG st = 0, i;

    V383Wr(Ctx, V383_LINK_IP_ENABLE, 0x00008000);          /* irq off */
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
    V383_LOG("IP reset: status 0x%08x after %u polls", st, i);

    /* An IP reset resets the IOMMU too; it comes back with paging off. */
    return st;
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

BOOLEAN
V383EvtMmuIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PDEVICE_CONTEXT ctx = GetContext(WdfInterruptGetDevice(Interrupt));
    static const ULONG base[2] = { V383_MMU0_OFFSET, V383_MMU1_OFFSET };
    BOOLEAN ours = FALSE;
    ULONG i;

    UNREFERENCED_PARAMETER(MessageID);

    for (i = 0; i < 2; i++) {
        ULONG st = V383Rd(ctx, base[i] + RK_MMU_INT_STATUS);
        if (!st)
            continue;
        ctx->MmuLastStatus[i] = V383Rd(ctx, base[i] + RK_MMU_STATUS);
        ctx->MmuLastFault[i] = V383Rd(ctx, base[i] + RK_MMU_PAGE_FAULT_ADDR);
        V383Wr(ctx, base[i] + RK_MMU_INT_CLEAR, st);
        ours = TRUE;
    }
    if (ours)
        InterlockedIncrement(&ctx->MmuIrqCount);
    return ours;
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
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ctx->Regs = (PUCHAR)MmMapIoSpaceEx(ctx->RegsPa, V383_REG_BLOCK_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!ctx->Regs)
        return STATUS_INSUFFICIENT_RESOURCES;

    V383_LOG("registers at 0x%llx, SRAM %s 0x%llx+0x%x", ctx->RegsPa.QuadPart,
             ctx->SramPresent ? "at" : "absent", ctx->SramPa.QuadPart, ctx->SramLen);

    /* not fatal: the warm-up is then skipped */
    (VOID)V383WarmupInit(ctx);
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

    UNREFERENCED_PARAMETER(PreviousState);

    V383_LOG("D0 entry: version 0x%08x, link irq 0x%08x status 0x%08x, mmu 0x%08x/0x%08x",
             V383Rd(ctx, V383_FUNC_OFFSET), V383Rd(ctx, V383_LINK_IRQ), V383Rd(ctx, V383_LINK_STATUS),
             V383Rd(ctx, V383_MMU0_OFFSET + RK_MMU_STATUS), V383Rd(ctx, V383_MMU1_OFFSET + RK_MMU_STATUS));

    V383MmuPagingOff(ctx, V383_MMU0_OFFSET);
    V383MmuPagingOff(ctx, V383_MMU1_OFFSET);

    /* stale interrupt state from the firmware or a previous OS */
    V383Wr(ctx, V383_LINK_IRQ, 0xFFFF0000);
    V383Wr(ctx, V383_LINK_STATUS, 0xFFFF0000);

    /* rk3576_workaround_run(): probe and resume; interrupts are not yet
     * connected here */
    V383WarmupRun(ctx, &ctx->Warm);
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
    UNREFERENCED_PARAMETER(TargetState);
    GetContext(Device)->InterruptsConnected = FALSE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------- DMA buffers */

static VOID
V383FreeAlloc(
    _In_ PV383_ALLOC A
    )
{
    KAPC_STATE apc;
    BOOLEAN attach = (A->Process && A->Process != PsGetCurrentProcess());

    if (A->UserVa && A->Mdl) {
        if (attach)
            KeStackAttachProcess(A->Process, &apc);
        MmUnmapLockedPages(A->UserVa, A->Mdl);
        if (attach)
            KeUnstackDetachProcess(&apc);
    }
    if (A->Mdl)
        IoFreeMdl(A->Mdl);
    if (A->Kva)
        MmFreeContiguousMemory(A->Kva);
    if (A->Process)
        ObDereferenceObject(A->Process);
    ExFreePoolWithTag(A, V383_POOL_TAG);
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
    PHYSICAL_ADDRESS low, high, skip;
    PV383_ALLOC a;
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

    a = (PV383_ALLOC)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), V383_POOL_TAG);
    if (!a)
        return STATUS_INSUFFICIENT_RESOURCES;

    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    /* Write-combined is Normal non-cacheable on ARM64: no stale cache lines
     * for a device that is not coherent (_CCA 0), and unaligned CPU access
     * still works. */
    a->Kva = MmAllocateContiguousNodeMemory(size, low, high, skip,
                                            PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
    if (!a->Kva) {
        ExFreePoolWithTag(a, V383_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(a->Kva, size);
    a->Size = size;
    a->Pa = MmGetPhysicalAddress(a->Kva);
    a->Mdl = IoAllocateMdl(a->Kva, (ULONG)size, FALSE, FALSE, NULL);
    if (!a->Mdl) {
        V383FreeAlloc(a);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(a->Mdl);

    __try {
        a->UserVa = MmMapLockedPagesSpecifyCache(a->Mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                                 NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a->UserVa = NULL;
    }
    if (!a->UserVa) {
        V383FreeAlloc(a);
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
    out->dma = (ULONG)a->Pa.QuadPart;
    out->size = (ULONG)size;
    out->reserved = 0;
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
    if (!found)
        return STATUS_INVALID_HANDLE;
    V383FreeAlloc(found);
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
    while (!IsListEmpty(&mine))
        V383FreeAlloc(CONTAINING_RECORD(RemoveHeadList(&mine), V383_ALLOC, Link));
}

/* Is [addr] inside a buffer this file allocated, or the SRAM? Lock held. */
static BOOLEAN
V383AddrOk(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFFILEOBJECT Fo,
    _In_ ULONG Addr
    )
{
    PLIST_ENTRY e;

    if (Addr == 0)
        return TRUE;
    if (Ctx->SramPresent && Addr >= Ctx->SramPa.QuadPart && Addr < Ctx->SramPa.QuadPart + Ctx->SramLen)
        return TRUE;
    for (e = Ctx->Allocs.Flink; e != &Ctx->Allocs; e = e->Flink) {
        PV383_ALLOC a = CONTAINING_RECORD(e, V383_ALLOC, Link);
        if (a->Owner == Fo && Addr >= a->Pa.QuadPart && Addr < a->Pa.QuadPart + a->Size)
            return TRUE;
    }
    return FALSE;
}

/* The H.264 address registers (MPP's trans_vdpu383_tbl_h264d) */
static BOOLEAN
V383IsAddrReg(
    _In_ ULONG R
    )
{
    if (R >= 128 && R <= 134)
        return TRUE;
    if (R >= 140 && R <= 160 && !(R & 1))
        return TRUE;
    if (R >= 168 && R <= 185)
        return TRUE;
    if (R == 192 || (R >= 194 && R <= 210))
        return TRUE;
    if (R >= 216 && R <= 232)
        return TRUE;
    return FALSE;
}

/* --------------------------------------------------------- decode */

static NTSTATUS
V383Run(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _Out_ ULONG_PTR *Info
    )
{
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT *out;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    LARGE_INTEGER timeout, t0, t1, freq;
    NTSTATUS status, wait;
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
    RtlZeroMemory(out, sizeof(*out));
    *Info = sizeof(*out);

    if (!Ctx->InterruptsConnected) {
        out->result = VDPU383_RUN_BUSY;
        return STATUS_SUCCESS;
    }

    if (!(in->flags & VDPU383_RUN_NO_ADDR_CHECK)) {
        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        for (r = 0; r < V383_NUM_REGS; r++) {
            if (!(in->write_mask[r / 32] & (1u << (r % 32))) || !V383IsAddrReg(r))
                continue;
            if (!V383AddrOk(Ctx, fo, in->regs[r])) {
                out->result = VDPU383_RUN_BAD_ADDRESS;
                out->bad_reg = r;
                break;
            }
        }
        WdfWaitLockRelease(Ctx->AllocLock);
        if (out->result)
            return STATUS_SUCCESS;
    }

    Ctx->Runs++;
    KeClearEvent(&Ctx->RunDone);
    Ctx->RunIrq = 0;
    Ctx->RunStatus = 0;

    for (r = 0; r < V383_NUM_REGS; r++)
        if (in->write_mask[r / 32] & (1u << (r % 32)))
            V383Wr(Ctx, V383_FUNC_OFFSET + 4 * r, in->regs[r]);

    /* buffers written by user mode reach memory before the decoder reads */
    KeMemoryBarrier();
    __dsb(_ARM64_BARRIER_SY);

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
    if (wait == STATUS_TIMEOUT) {
        InterlockedExchange(&Ctx->RunPending, 0);
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

    if (out->result != VDPU383_RUN_OK && (in->flags & VDPU383_RUN_RESET_ON_ERROR)) {
        WdfInterruptAcquireLock(Ctx->DecInterrupt);
        out->reset_status = V383IpReset(Ctx);
        WdfInterruptReleaseLock(Ctx->DecInterrupt);
    }
    return STATUS_SUCCESS;
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
        switch (params.Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_VDPU383_ALLOC:
            status = V383Alloc(ctx, Request, &info);
            WdfRequestCompleteWithInformation(Request, status, info);
            return;
        case IOCTL_VDPU383_FREE:
            status = V383Free(ctx, Request);
            WdfRequestComplete(Request, status);
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
        VDPU383_INFO *o;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*o), (PVOID *)&o, NULL);
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
        info = sizeof(*o);
        break;
    }
    case IOCTL_VDPU383_WARMUP: {
        VDPU383_WARMUP_RESULT *o;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*o), (PVOID *)&o, NULL);
        if (!NT_SUCCESS(status))
            break;
        /* the warm-up masks the decoder interrupt (link 0x58 bit 15) and
         * polls; hold the interrupt lock so the ISR cannot run in between */
        if (ctx->InterruptsConnected)
            WdfInterruptAcquireLock(ctx->DecInterrupt);
        V383WarmupRun(ctx, o);
        if (ctx->InterruptsConnected)
            WdfInterruptReleaseLock(ctx->DecInterrupt);
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

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = V383EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = V383EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = V383EvtD0Entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = V383EvtD0EntryPostInterrupts;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = V383EvtD0ExitPreInterrupts;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG_INIT(&fcfg, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, V383EvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fcfg, WDF_NO_OBJECT_ATTRIBUTES);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, V383EvtIoInCallerContext);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    status = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;

    ctx = GetContext(device);
    ctx->Device = device;
    InitializeListHead(&ctx->Allocs);
    KeInitializeEvent(&ctx->RunDone, NotificationEvent, FALSE);
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
    WDF_INTERRUPT_CONFIG_INIT(&icfg, V383EvtMmuIsr, NULL);
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
