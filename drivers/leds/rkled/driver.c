/*
 * driver.c - the board's LEDs: heartbeat, disk activity, on or off
 *
 * A KMDF function driver for ACPI\RKCP1ED0 (\_SB.LEDS, edk2-rk3576 board
 * Leds.asl, setup option "Board LEDs for the OS"). Each GpioIo in _CRS is
 * one LED; _DSD says whether it is active low and what it shows (dsd.c).
 * The pins are driven through GpioClx (rk3xgpio) with IOCTL_GPIO_WRITE_PINS
 * from a passive-level timer every 50 ms, and written only when they change.
 *
 *   heartbeat      two short flashes a second, like Linux's heartbeat
 *                  trigger at no load (100 ms on, 150 off, 100 on, 650 off)
 *   disk activity  on for the 100 ms after any read or write on
 *                  PhysicalDrive0..3 (IOCTL_DISK_PERFORMANCE, the counters
 *                  partmgr keeps; asking for them is what turns them on)
 *   on, off
 *
 * At D0 exit (shutdown, disable, driver update) every LED is turned off.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rkled.h"
#include <ntdddisk.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD RkLedEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE RkLedEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE RkLedEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY RkLedEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT RkLedEvtD0Exit;
EVT_WDF_TIMER RkLedEvtTimer;

/* One second of heartbeat, in 50 ms ticks: on, off, on, then off. */
static const UCHAR RkLedHeartbeat[20] = {
    1, 1, 0, 0, 0, 1, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#define RKLED_DISK_POLL_TICKS   2       /* 100 ms */
#define RKLED_DISK_RETRY_TICKS  100     /* 5 s between attempts to open a disk */
#define RKLED_NO_COUNT          ((ULONGLONG)-1)

static
NTSTATUS
RkLedOpen(
    _In_ PRKLED_CONTEXT Ctx,
    _In_ PUNICODE_STRING Path,
    _In_ ACCESS_MASK Access,
    _Out_ WDFIOTARGET *Target
    )
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_TARGET_OPEN_PARAMS open;
    NTSTATUS status;

    *Target = NULL;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Ctx->Device;
    status = WdfIoTargetCreate(Ctx->Device, &attr, Target);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, Path, Access);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    status = WdfIoTargetOpen(*Target, &open);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return status;
}

static
VOID
RkLedSet(
    _In_ PRKLED_CONTEXT Ctx,
    _In_ ULONG Index,
    _In_ BOOLEAN Lit
    )
{
    PRKLED_LED led = &Ctx->Led[Index];
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_REQUEST_SEND_OPTIONS opts;
    UCHAR value;
    NTSTATUS status;

    if (led->Target == NULL || led->Lit == (LONG)Lit) {
        return;
    }
    value = (UCHAR)((Lit ? 1 : 0) ^ (led->ActiveLow ? 1 : 0));
    /* The GPIO client documentation passes the same buffer both ways. */
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &value, sizeof(value));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &value, sizeof(value));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(200));
    status = WdfIoTargetSendIoctlSynchronously(led->Target, NULL, IOCTL_GPIO_WRITE_PINS,
                                               &in, &out, &opts, NULL);
    if (NT_SUCCESS(status)) {
        led->Lit = Lit ? 1 : 0;
    } else if (led->Lit != -2) {
        RKLED_LOG("LED %u: write failed 0x%08X", Index, status);
        led->Lit = -2;      /* log once, keep trying */
    }
}

/* Any read or write on any disk since the last poll. */
static
BOOLEAN
RkLedDiskPoll(
    _In_ PRKLED_CONTEXT Ctx
    )
{
    BOOLEAN active = FALSE;
    BOOLEAN retry = (Ctx->Tick - Ctx->DiskRetryTick) >= RKLED_DISK_RETRY_TICKS;
    ULONG i;

    for (i = 0; i < RKLED_MAX_DISKS; i++) {
        DISK_PERFORMANCE perf;
        WDF_MEMORY_DESCRIPTOR out;
        WDF_REQUEST_SEND_OPTIONS opts;
        ULONGLONG ops;
        NTSTATUS status;

        if (Ctx->Disk[i] == NULL) {
            DECLARE_UNICODE_STRING_SIZE(path, 32);

            if (!retry ||
                !NT_SUCCESS(RtlUnicodeStringPrintf(&path, L"\\??\\PhysicalDrive%u", i))) {
                continue;
            }
            /* No data access: a handle for IOCTLs only, that locks nothing. */
            if (!NT_SUCCESS(RkLedOpen(Ctx, &path, FILE_READ_ATTRIBUTES, &Ctx->Disk[i]))) {
                continue;
            }
            Ctx->DiskOps[i] = RKLED_NO_COUNT;
        }

        RtlZeroMemory(&perf, sizeof(perf));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &perf, sizeof(perf));
        WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(200));
        status = WdfIoTargetSendIoctlSynchronously(Ctx->Disk[i], NULL, IOCTL_DISK_PERFORMANCE,
                                                   NULL, &out, &opts, NULL);
        if (!NT_SUCCESS(status)) {
            /* Gone (removed) or not a disk that keeps counters: drop it. */
            WdfIoTargetClose(Ctx->Disk[i]);
            WdfObjectDelete(Ctx->Disk[i]);
            Ctx->Disk[i] = NULL;
            continue;
        }
        ops = (ULONGLONG)perf.ReadCount + perf.WriteCount;
        if (Ctx->DiskOps[i] != RKLED_NO_COUNT && ops != Ctx->DiskOps[i]) {
            active = TRUE;
        }
        Ctx->DiskOps[i] = ops;
    }
    if (retry) {
        Ctx->DiskRetryTick = Ctx->Tick;
    }
    return active;
}

static
VOID
RkLedDiskClose(
    _In_ PRKLED_CONTEXT Ctx
    )
{
    ULONG i;

    for (i = 0; i < RKLED_MAX_DISKS; i++) {
        if (Ctx->Disk[i] != NULL) {
            WdfIoTargetClose(Ctx->Disk[i]);
            WdfObjectDelete(Ctx->Disk[i]);
            Ctx->Disk[i] = NULL;
        }
    }
}

VOID
RkLedEvtTimer(
    _In_ WDFTIMER Timer
    )
{
    PRKLED_CONTEXT ctx = RkLedGetContext(WdfTimerGetParentObject(Timer));
    BOOLEAN beat;
    ULONG i;

    if (!ctx->Running) {
        return;
    }
    ctx->Tick++;
    beat = RkLedHeartbeat[ctx->Tick % RTL_NUMBER_OF(RkLedHeartbeat)] != 0;
    if (ctx->WantDisk && (ctx->Tick % RKLED_DISK_POLL_TICKS) == 0) {
        ctx->DiskActive = RkLedDiskPoll(ctx);
    }

    for (i = 0; i < ctx->Count; i++) {
        switch (ctx->Led[i].Trigger) {
        case RKLED_TRIGGER_ON:
            RkLedSet(ctx, i, TRUE);
            break;
        case RKLED_TRIGGER_HEARTBEAT:
            RkLedSet(ctx, i, beat);
            break;
        case RKLED_TRIGGER_DISK:
            RkLedSet(ctx, i, ctx->DiskActive);
            break;
        default:
            RkLedSet(ctx, i, FALSE);
            break;
        }
    }

    if (ctx->Running) {
        WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(RKLED_TICK_MS));
    }
}

NTSTATUS
RkLedEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PRKLED_CONTEXT ctx = RkLedGetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
    ULONG count, i, opened = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    ctx->Count = 0;
    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count && ctx->Count < RKLED_MAX_LEDS; i++) {
        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc != NULL && desc->Type == CmResourceTypeConnection &&
            desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
            ctx->Led[ctx->Count].Connection.LowPart = desc->u.Connection.IdLowPart;
            ctx->Led[ctx->Count].Connection.HighPart = desc->u.Connection.IdHighPart;
            ctx->Led[ctx->Count].Target = NULL;
            ctx->Led[ctx->Count].Lit = -1;
            ctx->Count++;
        }
    }
    if (ctx->Count == 0) {
        RKLED_LOG("no GpioIo in _CRS%s", "");
        return STATUS_NOT_FOUND;
    }

    RkLedReadConfig(ctx);

    for (i = 0; i < ctx->Count; i++) {
        DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);

        status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, ctx->Led[i].Connection.LowPart,
                                                  ctx->Led[i].Connection.HighPart);
        if (NT_SUCCESS(status)) {
            status = RkLedOpen(ctx, &path, GENERIC_READ | GENERIC_WRITE, &ctx->Led[i].Target);
        }
        if (NT_SUCCESS(status)) {
            opened++;
        } else {
            RKLED_LOG("LED %u: opening its GPIO failed 0x%08X (is rk3xgpio installed?)", i, status);
        }
    }
    return opened != 0 ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS
RkLedEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PRKLED_CONTEXT ctx = RkLedGetContext(Device);
    ULONG i;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    RkLedDiskClose(ctx);
    for (i = 0; i < ctx->Count; i++) {
        if (ctx->Led[i].Target != NULL) {
            WdfIoTargetClose(ctx->Led[i].Target);
            WdfObjectDelete(ctx->Led[i].Target);
            ctx->Led[i].Target = NULL;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
RkLedEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PRKLED_CONTEXT ctx = RkLedGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    ctx->Running = TRUE;
    ctx->DiskRetryTick = ctx->Tick - RKLED_DISK_RETRY_TICKS;
    WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(RKLED_TICK_MS));
    return STATUS_SUCCESS;
}

NTSTATUS
RkLedEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PRKLED_CONTEXT ctx = RkLedGetContext(Device);
    ULONG i;

    UNREFERENCED_PARAMETER(TargetState);

    ctx->Running = FALSE;
    WdfTimerStop(ctx->Timer, TRUE);
    for (i = 0; i < ctx->Count; i++) {
        RkLedSet(ctx, i, FALSE);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
RkLedEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_TIMER_CONFIG tcfg;
    WDFDEVICE device;
    PRKLED_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = RkLedEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = RkLedEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = RkLedEvtD0Entry;
    pnp.EvtDeviceD0Exit = RkLedEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, RKLED_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        RKLED_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }
    ctx = RkLedGetContext(device);
    ctx->Device = device;

    /*
     * Passive (GPIO and disk IOCTLs are sent synchronously), so one-shot:
     * the callback starts it again.
     */
    WDF_TIMER_CONFIG_INIT(&tcfg, RkLedEvtTimer);
    tcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&tcfg, &attr, &ctx->Timer);
    if (!NT_SUCCESS(status)) {
        RKLED_LOG("WdfTimerCreate failed 0x%08X", status);
    }
    return status;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, RkLedEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}
