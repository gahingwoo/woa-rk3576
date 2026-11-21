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

    return BtGpioOpen(ctx);
}

static NTSTATUS
BtEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (ctx->Regs != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Regs, ctx->RegsLength);
        ctx->Regs = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
BtEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Previous)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    UNREFERENCED_PARAMETER(Previous);
    /* Interrupts are connected after this returns; bring-up needs them. */
    UartInit(ctx, BT_INIT_BAUD);
    return STATUS_SUCCESS;
}

static NTSTATUS
BtEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Target)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);

    UNREFERENCED_PARAMETER(Target);
    ctx->Ready = FALSE;
    BtPower(ctx, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS
BtEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(Device);
    NTSTATUS status;

    status = BtBringUp(ctx);
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

    (VOID)BtBringUp(ctx);
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
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BT_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&Init, &attr, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = BtGetContext(device);
    ctx->Device = device;
    KeInitializeEvent(&ctx->SyncEvent, NotificationEvent, FALSE);
    BtInitQueuesAndFifos(ctx);

    /*
     * What BthMini is told. No SCO over HCI and no HCI bypass: voice would
     * need the module's PCM lines (SAI2) and an audio driver for them, so
     * only data profiles (A2DP, HID, file transfer) are offered. The
     * controller stays in D0: a D3 round trip would cost a patchram reload.
     */
    ctx->Caps.MaxAclTransferInSize = BT_MAX_ACL_PAYLOAD;
    ctx->Caps.ScoSupport = ScoSupportNone;
    ctx->Caps.MaxScoChannels = 0;
    ctx->Caps.IsDeviceIdleCapable = FALSE;
    ctx->Caps.IsDeviceWakeCapable = FALSE;

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
