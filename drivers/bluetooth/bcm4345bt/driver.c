/*
 * driver.c - KMDF bus driver for ACPI\RKCP4345, the BCM4345C5 Bluetooth half
 * of the AP6256 module on the LCKFB TaishanPi 3M
 *
 * The firmware (edk2-rk3576, Platform/LCKFB/TaishanPi3M/AcpiTables/
 * Bluetooth.asl) publishes \_SB.BTH0 with, in this order:
 *
 *   Memory     UART6 registers (0x2ad90000)
 *   Interrupt  UART6 (GSIV 114, level high)
 *   GpioIo     BT_REG_ON (GPIO1 PC7)
 *   GpioIo     BT_WAKE (GPIO1 PD4)
 *
 * and sets the UART's pins and its 24 MHz clock before boot. This driver
 * runs the UART itself (uart.c), powers and initialises the controller and
 * downloads its patchram (hci.c), and then enumerates one child that the
 * in-box BthMini binds to (pdo.c).
 *
 * Status of the last bring-up is written under the device's hardware key
 * (Device Parameters) so it can be read on a board without a debugger:
 * ResetStatus, BringUpStatus (NTSTATUS), FirmwareLoaded (0/1), RomVersion
 * and PatchedVersion ((HCI revision << 16) | LMP subversion), ChipName,
 * Baud, AddressFromChip. Settings read from the same key: FirmwarePath
 * (REG_SZ, NT path), OperatingBaud and UartClock (REG_DWORD), BdAddress
 * (REG_BINARY, 6 bytes, HCI byte order).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcm4345bt.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD BtEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE BtEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE BtEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY BtEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT BtEvtD0Exit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT BtEvtSelfManagedIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART BtEvtSelfManagedIoRestart;
static EVT_WDF_WORKITEM BtEvtStatsWork;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND BtEvtSelfManagedIoSuspend;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP BtEvtSelfManagedIoCleanup;

VOID
BtDelayMs(_In_ ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

VOID
BtRecordStatus(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING n;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&n, Name);
        (VOID)WdfRegistryAssignULong(key, &n, Value);
        WdfRegistryClose(key);
    }
}

VOID
BtRecordString(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value, _In_ ULONG Length)
{
    WCHAR buf[65];
    ULONG i;
    WDFKEY key;
    UNICODE_STRING n, v;

    for (i = 0; i < Length && i < 64; i++) {
        buf[i] = (Value[i] >= 0x20 && Value[i] < 0x7F) ? (WCHAR)Value[i] : L'?';
    }
    buf[i] = 0;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&n, Name);
        RtlInitUnicodeString(&v, buf);
        (VOID)WdfRegistryAssignUnicodeString(key, &n, &v);
        WdfRegistryClose(key);
    }
}

/* ------------------------------------------------------ instrumentation
 *
 * Every BthX IOCTL is counted, with the status it got back and the buffer
 * lengths, and flushed to Device Parameters at PASSIVE_LEVEL by a work
 * item (BthMini may call at DISPATCH_LEVEL):
 *
 *   BthX<Name>Count   calls received; <Name> is GetVersion, SetVersion,
 *                     QueryCaps, WriteHci, ReadHci, Other
 *   BthX<Name>Status  NTSTATUS of the last one (0x103 = pended)
 *   BthX<Name>Lens    (input length << 16) | output length of the last one
 *   BthX<Name>Done    reads and writes completed after pending
 *   BthXOtherIoctl    the last IOCTL code nobody handles (FDO or child)
 *   BthXSetVersion    the version BthMini chose
 *   BthXLastCommand   the last HCI opcode BthMini wrote
 *   BthXLastEvent     (event code << 16) | opcode of the last event read
 *   BthXCapsSco, BthXCapsScoChannels, BthXCapsMaxAcl, BthXCapsIdle
 *                     what QUERY_CAPABILITIES answers
 */
static const PCWSTR BtBxName[BX_MAX] = {
    L"GetVersion", L"SetVersion", L"QueryCaps", L"WriteHci", L"ReadHci", L"Other"
};

static VOID
PutULong(_In_ WDFKEY Key, _In_ PCWSTR Name, _In_ ULONG Value)
{
    UNICODE_STRING n;

    RtlInitUnicodeString(&n, Name);
    (VOID)WdfRegistryAssignULong(Key, &n, Value);
}

static VOID
BtEvtStatsWork(_In_ WDFWORKITEM WorkItem)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    WCHAR name[40];
    WDFKEY key;
    ULONG i;

    InterlockedExchange(&ctx->StatsQueued, 0);
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    for (i = 0; i < BX_MAX; i++) {
        if (NT_SUCCESS(RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"BthX%wsCount", BtBxName[i]))) {
            PutULong(key, name, (ULONG)ctx->BxCount[i]);
        }
        if (NT_SUCCESS(RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"BthX%wsStatus", BtBxName[i]))) {
            PutULong(key, name, (ULONG)ctx->BxStatus[i]);
        }
        if (NT_SUCCESS(RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"BthX%wsLens", BtBxName[i]))) {
            PutULong(key, name, (ULONG)ctx->BxLens[i]);
        }
        if (NT_SUCCESS(RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"BthX%wsDone", BtBxName[i]))) {
            PutULong(key, name, (ULONG)ctx->BxDone[i]);
        }
    }
    PutULong(key, L"BthXOtherIoctl", (ULONG)ctx->BxLastUnknown);
    PutULong(key, L"BthXSetVersion", (ULONG)ctx->BxSetVersion);
    PutULong(key, L"BthXLastCommand", (ULONG)ctx->BxLastCommand);
    PutULong(key, L"BthXLastEvent", (ULONG)ctx->BxLastEvent);
    PutULong(key, L"BthXCapsSco", (ULONG)ctx->Caps.ScoSupport);
    PutULong(key, L"BthXCapsScoChannels", ctx->Caps.MaxScoChannels);
    PutULong(key, L"BthXCapsMaxAcl", ctx->Caps.MaxAclTransferInSize);
    PutULong(key, L"BthXCapsIdle", ctx->Caps.IsDeviceIdleCapable);
    PutULong(key, L"UartIsrCount", ctx->IsrCount);
    PutULong(key, L"UartIsrUnclaimed", ctx->IsrUnclaimed);
    PutULong(key, L"UartRxBytes", ctx->RxBytes);
    PutULong(key, L"UartRxOverflow", ctx->RxOverflow);
    PutULong(key, L"UartLineErrors", ctx->LineErrors);
    PutULong(key, L"UartThrottles", ctx->ThrottleCount);
    PutULong(key, L"UartBusyTimeouts", ctx->BusyTimeouts);
    PutULong(key, L"UartBaud", ctx->Baud);
    PutULong(key, L"HciResyncs", ctx->ParseResyncs);
    WdfRegistryClose(key);
}

/* Any IRQL up to DISPATCH_LEVEL. */
VOID
BtStatsKick(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    if (Ctx->StatsWork != NULL && InterlockedExchange(&Ctx->StatsQueued, 1) == 0) {
        WdfWorkItemEnqueue(Ctx->StatsWork);
    }
}

VOID
BtNote(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ BT_BX_INDEX Index, _In_ NTSTATUS Status,
       _In_ size_t InLen, _In_ size_t OutLen)
{
    InterlockedIncrement(&Ctx->BxCount[Index]);
    InterlockedExchange(&Ctx->BxStatus[Index], Status);
    InterlockedExchange(&Ctx->BxLens[Index], (LONG)((min(InLen, 0xFFFF) << 16) | min(OutLen, 0xFFFF)));
    BtStatsKick(Ctx);
}

VOID
BtNoteDone(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ BT_BX_INDEX Index)
{
    InterlockedIncrement(&Ctx->BxDone[Index]);
    BtStatsKick(Ctx);
}

/*
 * Breadcrumb: the step the driver is in, flushed to disk at once, so that a
 * hang or a power cut leaves the last step readable afterwards
 * (Device Parameters: LastStep, PrevStep, StepSeq). PASSIVE_LEVEL only;
 * every caller is a PnP or power callback or the bring-up.
 */
VOID
BtCrumb(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ PCWSTR Step)
{
    WDFKEY key;
    UNICODE_STRING n, v;
    DECLARE_UNICODE_STRING_SIZE(prev, 64);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    RtlInitUnicodeString(&n, L"LastStep");
    if (NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &n, NULL, &prev))) {
        RtlInitUnicodeString(&v, L"PrevStep");
        (VOID)WdfRegistryAssignUnicodeString(key, &v, &prev);
    }
    RtlInitUnicodeString(&v, Step);
    (VOID)WdfRegistryAssignUnicodeString(key, &n, &v);
    PutULong(key, L"StepSeq", (ULONG)InterlockedIncrement(&Ctx->StepSeq));
    (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    WdfRegistryClose(key);
    BT_LOG("step %ws", Step);
}

/* Write the counters now, at PASSIVE_LEVEL, not from the work item. */
static VOID
BtStatsFlushNow(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    if (Ctx->StatsWork != NULL) {
        WdfWorkItemFlush(Ctx->StatsWork);
        InterlockedExchange(&Ctx->StatsQueued, 1);
        BtEvtStatsWork(Ctx->StatsWork);
    }
}

static ULONG
ReadSetting(_In_ WDFDEVICE Device, _In_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY key;
    UNICODE_STRING n;
    ULONG v = Default;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&n, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v)) || v == 0) {
            v = Default;
        }
        WdfRegistryClose(key);
    }
    return v;
}

static NTSTATUS
BtEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);
    ULONG i, count = WdfCmResourceListGetCount(Translated);
    ULONG gpios = 0;
    BOOLEAN haveIrq = FALSE;
    NTSTATUS status;

    BtCrumb(ctx, L"PrepareHardware: start");
    ctx->HaveRegOn = FALSE;
    ctx->HaveWake = FALSE;

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR t = WdfCmResourceListGetDescriptor(Translated, i);

        switch (t->Type) {
        case CmResourceTypeMemory:
            if (ctx->Regs == NULL) {
                ctx->RegsLength = t->u.Memory.Length;
                ctx->Regs = (volatile UCHAR *)MmMapIoSpaceEx(t->u.Memory.Start, t->u.Memory.Length,
                                                            PAGE_READWRITE | PAGE_NOCACHE);
                if (ctx->Regs == NULL) {
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            break;

        case CmResourceTypeInterrupt:
            if (!haveIrq) {
                WDF_INTERRUPT_CONFIG icfg;
                WDF_INTERRUPT_CONFIG_INIT(&icfg, BtEvtInterruptIsr, BtEvtInterruptDpc);
                icfg.EvtInterruptEnable = BtEvtInterruptEnable;
                icfg.EvtInterruptDisable = BtEvtInterruptDisable;
                icfg.InterruptRaw = WdfCmResourceListGetDescriptor(Raw, i);
                icfg.InterruptTranslated = t;
                status = WdfInterruptCreate(Device, &icfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->Interrupt);
                if (!NT_SUCCESS(status)) {
                    BT_LOG("WdfInterruptCreate failed 0x%08X", status);
                    return status;
                }
                haveIrq = TRUE;
            }
            break;

        case CmResourceTypeConnection:
            if (t->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
                t->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
                LARGE_INTEGER id;
                id.LowPart = t->u.Connection.IdLowPart;
                id.HighPart = t->u.Connection.IdHighPart;
                if (gpios == 0) {
                    ctx->RegOnConnection = id;
                    ctx->HaveRegOn = TRUE;
                } else if (gpios == 1) {
                    ctx->WakeConnection = id;
                    ctx->HaveWake = TRUE;
                }
                gpios++;
            }
            break;

        default:
            break;
        }
    }

    if (ctx->Regs == NULL || ctx->RegsLength < 0x100 || !haveIrq) {
        BT_LOG("_CRS needs the UART registers and its interrupt (regs %p len 0x%lx irq %u)",
               ctx->Regs, ctx->RegsLength, haveIrq);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ctx->UartClock = ReadSetting(Device, L"UartClock", BT_DEFAULT_UART_CLOCK);
    ctx->OperBaud = ReadSetting(Device, L"OperatingBaud", BT_DEFAULT_OPER_BAUD);
    /* 0 means "not set" to ReadSetting, so these are stored plus one. */
    ctx->Caps.ScoSupport = (BTHX_SCO_SUPPORT)(ReadSetting(Device, L"BthXScoSupport", (ULONG)ctx->Caps.ScoSupport + 1) - 1);
    ctx->Caps.MaxScoChannels = ReadSetting(Device, L"BthXMaxScoChannels", ctx->Caps.MaxScoChannels + 1) - 1;
    ctx->Caps.IsDeviceIdleCapable = (BOOLEAN)(ReadSetting(Device, L"BthXIdleCapable", (ULONG)ctx->Caps.IsDeviceIdleCapable + 1) > 1);
    BtStatsKick(ctx);

    status = BtGpioOpen(ctx);
    BtCrumb(ctx, NT_SUCCESS(status) ? L"PrepareHardware: done" : L"PrepareHardware: GPIO open failed");
    return status;
}

static NTSTATUS
BtEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    BtCrumb(ctx, L"ReleaseHardware: start");
    /* Interrupts are disconnected by now; make sure the UART agrees. */
    UartQuiesce(ctx);
    /* Close the GPIO connections, so a later start can open them again
     * (they are Exclusive). */
    if (ctx->RegOnTarget != NULL) {
        WdfObjectDelete(ctx->RegOnTarget);
        ctx->RegOnTarget = NULL;
    }
    if (ctx->WakeTarget != NULL) {
        WdfObjectDelete(ctx->WakeTarget);
        ctx->WakeTarget = NULL;
    }
    if (ctx->Regs != NULL) {
        volatile UCHAR *regs = ctx->Regs;
        ctx->Regs = NULL;
        MmUnmapIoSpace((PVOID)regs, ctx->RegsLength);
    }
    BtCrumb(ctx, L"ReleaseHardware: done");
    return STATUS_SUCCESS;
}

static NTSTATUS
BtEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Previous)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    UNREFERENCED_PARAMETER(Previous);
    BtCrumb(ctx, L"D0Entry: start");
    /* Interrupts are connected after this returns; bring-up needs them. */
    UartInit(ctx, BT_INIT_BAUD);
    BtCrumb(ctx, L"D0Entry: done");
    return STATUS_SUCCESS;
}

/*
 * Stop. The framework has already run BtEvtSelfManagedIoSuspend (no new
 * HCI traffic, pended requests failed) and disabled the interrupt, which
 * quiesced the UART. Nothing here waits on the controller: the module is
 * switched off through the GPIO with a bounded wait and that is all.
 */
static NTSTATUS
BtEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Target)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    UNREFERENCED_PARAMETER(Target);
    BtCrumb(ctx, L"D0Exit: start");
    ctx->Ready = FALSE;
    UartQuiesce(ctx);
    BtCancelPending(ctx);
    BtCrumb(ctx, L"D0Exit: power off");
    BtPower(ctx, FALSE);
    BtStatsFlushNow(ctx);
    BtCrumb(ctx, L"D0Exit: done");
    return STATUS_SUCCESS;
}

static VOID
BtEvtSelfManagedIoSuspend2(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    BtCrumb(Ctx, L"SelfManagedIoSuspend: start");
    Ctx->Ready = FALSE;
    BtCancelPending(Ctx);
    BtCrumb(Ctx, L"SelfManagedIoSuspend: done");
}

static NTSTATUS
BtEvtSelfManagedIoSuspend(_In_ WDFDEVICE Device)
{
    BtEvtSelfManagedIoSuspend2(BtGetContext(Device));
    return STATUS_SUCCESS;
}

static VOID
BtEvtSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    BtCrumb(ctx, L"SelfManagedIoCleanup");
    if (ctx->StatsWork != NULL) {
        WdfWorkItemFlush(ctx->StatsWork);
    }
}

static NTSTATUS
BtEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);
    NTSTATUS status;

    BtCrumb(ctx, L"SelfManagedIoInit: bring-up");
    status = BtBringUp(ctx);
    BtCrumb(ctx, NT_SUCCESS(status) ? L"SelfManagedIoInit: bring-up done" : L"SelfManagedIoInit: bring-up failed");
    if (!NT_SUCCESS(status)) {
        /* Stay started, so the status values above can be read. */
        BT_LOG("bring-up failed 0x%08X; no Bluetooth radio", status);
        return STATUS_SUCCESS;
    }
    status = BtCreateChild(ctx);
    if (!NT_SUCCESS(status)) {
        BT_LOG("child creation failed 0x%08X", status);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
BtEvtSelfManagedIoRestart(_In_ WDFDEVICE Device)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    BtCrumb(ctx, L"SelfManagedIoRestart: bring-up");
    (VOID)BtBringUp(ctx);
    BtCrumb(ctx, L"SelfManagedIoRestart: done");
    if (ctx->Ready && !ctx->ChildCreated) {
        (VOID)BtCreateChild(ctx);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
BtEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_WORKITEM_CONFIG wcfg;
    WDFDEVICE device;
    PBT_DEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(Init, FILE_DEVICE_BUS_EXTENDER);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BtEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = BtEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = BtEvtD0Entry;
    pnp.EvtDeviceD0Exit = BtEvtD0Exit;
    pnp.EvtDeviceSelfManagedIoInit = BtEvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoRestart = BtEvtSelfManagedIoRestart;
    pnp.EvtDeviceSelfManagedIoSuspend = BtEvtSelfManagedIoSuspend;
    pnp.EvtDeviceSelfManagedIoCleanup = BtEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BT_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&Init, &attr, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = BtGetContext(device);
    ctx->Device = device;
    BtCrumb(ctx, L"DeviceAdd");
    KeInitializeEvent(&ctx->SyncEvent, NotificationEvent, FALSE);
    BtInitQueuesAndFifos(ctx);

    /*
     * What BthMini is told. Its start path (BthMini.sys, 22621 ARM64, the
     * code after IOCTL_BTHX_QUERY_CAPABILITIES) fails with
     * STATUS_DEVICE_CONFIGURATION_ERROR unless MaxAclTransferInSize is at
     * least 0x103, ScoSupport is ScoSupportHCIBypass (2) and MaxScoChannels
     * is 1; it caps MaxAclTransferInSize at 0x1000. ScoSupportNone, which
     * the first version of this driver sent, is refused, and so is
     * ScoSupportHCI. HCI bypass means SCO audio would travel on the
     * module's PCM lines (SAI2 here), for which there is no audio driver:
     * calls and a headset microphone still will not work, but the radio
     * starts. The controller stays in D0: a D3 round trip would cost a
     * patchram reload. PrepareHardware lets the registry override each
     * field (BthXScoSupport, BthXMaxScoChannels, BthXIdleCapable).
     */
    ctx->Caps.MaxAclTransferInSize = BT_MAX_ACL_PAYLOAD;
    ctx->Caps.ScoSupport = ScoSupportHCIBypass;
    ctx->Caps.MaxScoChannels = 1;
    ctx->Caps.IsDeviceIdleCapable = FALSE;
    ctx->Caps.IsDeviceWakeCapable = FALSE;

    WDF_WORKITEM_CONFIG_INIT(&wcfg, BtEvtStatsWork);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfWorkItemCreate(&wcfg, &attr, &ctx->StatsWork);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfSpinLockCreate(&attr, &ctx->PacketLock);
    if (NT_SUCCESS(status)) {
        status = WdfSpinLockCreate(&attr, &ctx->TxLock);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Default queue: BthX IOCTLs forwarded from the child. Not power managed. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.PowerManaged = WdfFalse;
    qcfg.EvtIoDeviceControl = BtEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->EventReadQueue);
    if (NT_SUCCESS(status)) {
        status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->AclReadQueue);
    }
    if (NT_SUCCESS(status)) {
        status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->TxWaitQueue);
    }
    return status;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;

    WDF_DRIVER_CONFIG_INIT(&cfg, BtEvtDeviceAdd);
    cfg.DriverPoolTag = BT_POOL_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
