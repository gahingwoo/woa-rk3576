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
 * kernel's (warmup.c), and runs only when the Device Parameters value
 * WarmupAtD0 is 1 (default 0: the first board test reads registers and
 * nothing more; `vdpu383test warmup` runs it on demand). The IOMMU is left with paging off, or turned off if
 * it is found on, so the decoder's addresses are physical; buffers are
 * therefore allocated below 4 GiB.
 *
 * Only administrators and SYSTEM can open the device, and one handle at a
 * time. Every address register of a decode is checked against the
 * caller's buffers before the decoder is started, unless the caller asks
 * not to.
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
EVT_WDF_IO_IN_CALLER_CONTEXT V383EvtIoInCallerContext;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL V383EvtIoDeviceControl;
EVT_WDF_FILE_CLEANUP V383EvtFileCleanup;

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

    /* An IP reset resets the IOMMU too; it comes back with paging off,
     * which is how this driver runs it. Mainline reprograms its page
     * table here (rkvdec_iommu_restore()); there is none to restore. */
    V383CacheConfig(Ctx);
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

    V383Crumb(ctx, L"PrepareHardware: start");
    ctx->WarmupAtD0 = V383Setting(ctx, L"WarmupAtD0", 0);

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

    /* not fatal: the warm-up is then skipped */
    (VOID)V383WarmupInit(ctx);
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

    V383MmuPagingOff(ctx, V383_MMU0_OFFSET);
    V383MmuPagingOff(ctx, V383_MMU1_OFFSET);

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
     * connected here. Off unless WarmupAtD0 = 1. */
    if (ctx->WarmupAtD0 == 1) {
        V383Crumb(ctx, L"D0Entry: warm-up decode");
        V383WarmupRun(ctx, &ctx->Warm);
        V383Fact(ctx, L"WarmupIrq", ctx->Warm.irq);
        V383Fact(ctx, L"WarmupStatus", ctx->Warm.status);
        V383Fact(ctx, L"WarmupTimedOut", ctx->Warm.timed_out);
    } else {
        RtlZeroMemory(&ctx->Warm, sizeof(ctx->Warm));
    }
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
    UNREFERENCED_PARAMETER(TargetState);
    GetContext(Device)->InterruptsConnected = FALSE;
    V383Crumb(GetContext(Device), L"D0Exit");
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

    /* At most V383_MAX_PER_FILE of contiguous memory per open handle: a
     * 1080p H.264 decoder with 32 surfaces needs about 160 MiB. */
    {
        PLIST_ENTRY e;
        SIZE_T held = 0;
        WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);

        WdfWaitLockAcquire(Ctx->AllocLock, NULL);
        for (e = Ctx->Allocs.Flink; e != &Ctx->Allocs; e = e->Flink) {
            PV383_ALLOC h = CONTAINING_RECORD(e, V383_ALLOC, Link);
            if (h->Owner == fo)
                held += h->Size;
        }
        WdfWaitLockRelease(Ctx->AllocLock);
        if (held + size > V383_MAX_PER_FILE)
            return STATUS_QUOTA_EXCEEDED;
    }

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
    BOOLEAN timedOut;
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

    /* With users let in, nobody may skip the check. */
    if (!(in->flags & VDPU383_RUN_NO_ADDR_CHECK) || Ctx->UserAccess) {
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
    if (Ctx->Runs == 1)
        V383Crumb(Ctx, L"Run: first decode of the session, starting");
    KeClearEvent(&Ctx->RunDone);
    Ctx->RunIrq = 0;
    Ctx->RunStatus = 0;

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
    if (timedOut) {
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
    if (timedOut || (out->result != VDPU383_RUN_OK && (in->flags & VDPU383_RUN_RESET_ON_ERROR))) {
        WdfInterruptAcquireLock(Ctx->DecInterrupt);
        out->reset_status = V383IpReset(Ctx);
        WdfInterruptReleaseLock(Ctx->DecInterrupt);
    }
    /*
     * An interrupt taken just before the reset may still have its DPC
     * queued; drained here, it cannot complete the next run (mainline
     * "Handle interrupts without a current context" closes the same race
     * between its watchdog and its interrupt thread).
     */
    if (timedOut)
        KeFlushQueuedDpcs();
    if (Ctx->Runs == 1) {
        V383Fact(Ctx, L"FirstRunResult", out->result);
        V383Fact(Ctx, L"FirstRunStatus", out->status);
        V383Crumb(Ctx, L"Run: first decode returned");
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

    WDF_FILEOBJECT_CONFIG_INIT(&fcfg, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, V383EvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fcfg, WDF_NO_OBJECT_ATTRIBUTES);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, V383EvtIoInCallerContext);
    /* Exclusive for the test tool alone; with UserAccess every player
     * process opens it for itself. */
    WdfDeviceInitSetExclusive(DeviceInit, userAccess ? FALSE : TRUE);

    status = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfDeviceInitAssignSDDLString(DeviceInit,
                                           userAccess == 2 ? &sddl_low :
                                           userAccess == 1 ? &sddl_users : &sddl);
    if (!NT_SUCCESS(status))
        return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;

    ctx = GetContext(device);
    ctx->Device = device;
    ctx->UserAccess = userAccess;
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
