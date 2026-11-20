/*
 * driver.c - Goodix GT9271 touch controller (I2C) as a HID touch screen
 *
 * A KMDF function driver for ACPI\RKCP9271 with vhf.sys as its lower
 * filter. It reads the controller over SPB (rk3xi2c), is woken by the
 * controller's interrupt line through GpioClx (rk3xgpio), and hands each
 * frame to the Virtual HID Framework, which enumerates the HID touch screen
 * that the in-box HID stack then drives.
 *
 * Written from the Goodix GT9xx register description (status at 0x814E,
 * points from 0x814F, 8 bytes each) and the Microsoft VHF, SPB and HID
 * touch screen documentation. No code is taken from the Linux goodix driver.
 *
 * The controller is never reset and its configuration area (0x8047 onward)
 * is never written. The GT9xx reset sequence also picks the I2C address
 * from the level of the interrupt line, so a reset done wrong moves the chip
 * off 0x5d; the firmware leaves it running, and that is the state used here.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#include "gt9271.h"

extern UCHAR GtReportDescriptor[];
extern USHORT GtReportDescriptorLength;

EVT_VHF_ASYNC_OPERATION GtEvtVhfGetFeature;
VOID GtReportFrame(_In_ PGT_DEVICE_CONTEXT Ctx, _In_reads_opt_(Count) const GT_POINT *Now, _In_ ULONG Count);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD GtEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP GtEvtDeviceCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE GtEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE GtEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY GtEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT GtEvtD0Exit;
EVT_WDF_INTERRUPT_ISR GtEvtInterruptIsr;
EVT_WDF_TIMER GtEvtTimer;

/* ------------------------------------------------------------------ I2C */

static
NTSTATUS
GtRead(
    _In_ PGT_DEVICE_CONTEXT Ctx,
    _In_ USHORT Reg,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    WDF_MEMORY_DESCRIPTOR in;
    WDF_REQUEST_SEND_OPTIONS opts;
    UCHAR addr[2];
    ULONG_PTR bytes = 0;
    NTSTATUS status;

    addr[0] = (UCHAR)(Reg >> 8);
    addr[1] = (UCHAR)(Reg & 0xFF);

    SPB_TRANSFER_LIST_INIT(&seq.List, 2);
    seq.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, addr, sizeof(addr));
    seq.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, Buffer, Length);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &seq, sizeof(seq));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(GT_I2C_TIMEOUT_MS));

    status = WdfIoTargetSendIoctlSynchronously(Ctx->SpbTarget, NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &in, NULL, &opts, &bytes);
    if (NT_SUCCESS(status) && bytes != sizeof(addr) + Length) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return status;
}

/* Only ever used for the status register: 0x814E <- 0. */
static
NTSTATUS
GtClearStatus(
    _In_ PGT_DEVICE_CONTEXT Ctx
    )
{
    UCHAR buf[3];
    WDF_MEMORY_DESCRIPTOR mem;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR bytes = 0;
    NTSTATUS status;

    buf[0] = (UCHAR)(GT_REG_STATUS >> 8);
    buf[1] = (UCHAR)(GT_REG_STATUS & 0xFF);
    buf[2] = 0;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, buf, sizeof(buf));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(GT_I2C_TIMEOUT_MS));

    status = WdfIoTargetSendWriteSynchronously(Ctx->SpbTarget, NULL, &mem,
                                               NULL, &opts, &bytes);
    if (NT_SUCCESS(status) && bytes != sizeof(buf)) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return status;
}

/* ------------------------------------------------------------ servicing */

/*
 * Read one frame if the controller has one: status, points, clear the
 * status, report. Returns TRUE if a frame was there. Lock held.
 */
static
BOOLEAN
GtServiceLocked(
    _In_ PGT_DEVICE_CONTEXT Ctx
    )
{
    UCHAR buf[1 + GT_POINT_SIZE * GT_MAX_POINTS];
    GT_POINT now[GT_MAX_POINTS];
    ULONG points;
    ULONG count = 0;
    ULONG i;
    const UCHAR *p;
    NTSTATUS status;

    /* Status and the first point in one go; most frames have one finger. */
    status = GtRead(Ctx, GT_REG_STATUS, buf, 1 + GT_POINT_SIZE);
    if (!NT_SUCCESS(status)) {
        if (Ctx->ReadErrors++ < 10) {
            GT_LOG("status read failed 0x%08X", status);
        }
        return FALSE;
    }

    if ((buf[0] & GT_STATUS_READY) == 0) {
        return FALSE;
    }

    points = buf[0] & GT_STATUS_POINTS_MASK;
    if (points > GT_MAX_POINTS) {
        points = GT_MAX_POINTS;
    }

    if (points > 1) {
        status = GtRead(Ctx, GT_REG_POINT0 + GT_POINT_SIZE,
                        &buf[1 + GT_POINT_SIZE],
                        (points - 1) * GT_POINT_SIZE);
        if (!NT_SUCCESS(status)) {
            if (Ctx->ReadErrors++ < 10) {
                GT_LOG("point read failed 0x%08X", status);
            }
            /* Keep what was read: the first point. */
            points = 1;
        }
    }

    /* The controller does not refill the buffer until this is cleared. */
    status = GtClearStatus(Ctx);
    if (!NT_SUCCESS(status) && Ctx->ReadErrors++ < 10) {
        GT_LOG("status clear failed 0x%08X", status);
    }

    for (i = 0; i < points; i++) {
        USHORT x, y;
        UCHAR id;

        p = &buf[1 + i * GT_POINT_SIZE];
        id = p[0];
        x = (USHORT)(p[1] | (p[2] << 8));
        y = (USHORT)(p[3] | (p[4] << 8));

        if (x > GT_LOGICAL_MAX_X) {
            x = GT_LOGICAL_MAX_X;
        }
        if (y > GT_LOGICAL_MAX_Y) {
            y = GT_LOGICAL_MAX_Y;
        }

        /* A track id twice in one frame: keep the first. */
        {
            ULONG k;
            BOOLEAN dup = FALSE;
            for (k = 0; k < count; k++) {
                if (now[k].Id == id) {
                    dup = TRUE;
                }
            }
            if (dup) {
                continue;
            }
        }

        now[count].Id = id;
        now[count].X = x;
        now[count].Y = y;
        count++;
    }

    GtReportFrame(Ctx, now, count);
    return TRUE;
}

static
VOID
GtArmTimer(
    _In_ PGT_DEVICE_CONTEXT Ctx
    )
{
    if (!Ctx->Running) {
        return;
    }
    if (Ctx->Polled) {
        WdfTimerStart(Ctx->Timer, WDF_REL_TIMEOUT_IN_MS(GT_POLL_MS));
    } else if (Ctx->DownCount != 0) {
        WdfTimerStart(Ctx->Timer, WDF_REL_TIMEOUT_IN_MS(GT_GUARD_POLL_MS));
    }
}

BOOLEAN
GtEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageID);

    /* Passive-level ISR (PassiveHandling): I2C can be done right here. */
    WdfWaitLockAcquire(ctx->Lock, NULL);
    if (ctx->Running) {
        GtServiceLocked(ctx);
        GtArmTimer(ctx);
    }
    WdfWaitLockRelease(ctx->Lock);

    /* The line is ours alone (Exclusive in _CRS). */
    return TRUE;
}

/*
 * Without an interrupt: the poll. With one: while a contact is down, a look
 * at the controller in case an edge was missed, so that the frame that lifts
 * the last finger cannot be lost. No contact is ever released on a timeout:
 * a finger held still keeps producing frames, and the frame that says it
 * lifted stays in the buffer until it is read.
 */
VOID
GtEvtTimer(
    _In_ WDFTIMER Timer
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext(WdfTimerGetParentObject(Timer));

    WdfWaitLockAcquire(ctx->Lock, NULL);
    if (ctx->Running) {
        GtServiceLocked(ctx);
        GtArmTimer(ctx);
    }
    WdfWaitLockRelease(ctx->Lock);
}

/* ------------------------------------------------------------------ PnP */

NTSTATUS
GtEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR raw, trans;
    WDF_INTERRUPT_CONFIG icfg;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_TARGET_OPEN_PARAMS open;
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    UCHAR id[4] = { 0 };
    UCHAR fw[2] = { 0 };
    UCHAR res[4] = { 0 };
    ULONG count, i;
    NTSTATUS status;

    ctx->HaveSpb = FALSE;
    ctx->Interrupt = NULL;

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i++) {
        trans = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        raw = WdfCmResourceListGetDescriptor(ResourcesRaw, i);
        if (trans == NULL || raw == NULL) {
            continue;
        }

        switch (trans->Type) {
        case CmResourceTypeConnection:
            if (trans->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                trans->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C &&
                !ctx->HaveSpb) {
                ctx->SpbConnectionId.LowPart = trans->u.Connection.IdLowPart;
                ctx->SpbConnectionId.HighPart = trans->u.Connection.IdHighPart;
                ctx->HaveSpb = TRUE;
            }
            break;

        case CmResourceTypeInterrupt:
            if (ctx->Interrupt != NULL) {
                break;
            }
            WDF_INTERRUPT_CONFIG_INIT(&icfg, GtEvtInterruptIsr, NULL);
            icfg.PassiveHandling = TRUE;
            icfg.InterruptRaw = raw;
            icfg.InterruptTranslated = trans;
            status = WdfInterruptCreate(Device, &icfg, WDF_NO_OBJECT_ATTRIBUTES,
                                        &ctx->Interrupt);
            if (!NT_SUCCESS(status)) {
                GT_LOG("WdfInterruptCreate failed 0x%08X", status);
                return status;
            }
            break;

        default:
            break;
        }
    }

    if (!ctx->HaveSpb) {
        GT_LOG("no I2cSerialBus resource in _CRS%s", "");
        return STATUS_NOT_FOUND;
    }

    ctx->Polled = (ctx->Interrupt == NULL);

    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path,
                                              ctx->SpbConnectionId.LowPart,
                                              ctx->SpbConnectionId.HighPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Device;
    status = WdfIoTargetCreate(Device, &attr, &ctx->SpbTarget);
    if (!NT_SUCCESS(status)) {
        GT_LOG("WdfIoTargetCreate failed 0x%08X", status);
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &path,
                                                GENERIC_READ | GENERIC_WRITE);
    status = WdfIoTargetOpen(ctx->SpbTarget, &open);
    if (!NT_SUCCESS(status)) {
        GT_LOG("opening the I2C connection failed 0x%08X", status);
        WdfObjectDelete(ctx->SpbTarget);
        ctx->SpbTarget = NULL;
        return status;
    }

    /*
     * Is it there? With no screen the controller does not answer. The
     * firmware normally hides this device then (_STA), so this is the
     * fallback: fail the start, and Device Manager shows code 10.
     */
    status = GtRead(ctx, GT_REG_PRODUCT_ID, id, sizeof(id));
    if (!NT_SUCCESS(status) ||
        id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') {
        GT_LOG("no Goodix controller answered at the I2C address in _CRS "
               "(0x%08X, id %02X %02X %02X %02X)",
               status, id[0], id[1], id[2], id[3]);
        WdfIoTargetClose(ctx->SpbTarget);
        WdfObjectDelete(ctx->SpbTarget);
        ctx->SpbTarget = NULL;
        return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_CONNECTED : status;
    }

    /* Read-only looks at the firmware version and the configured range. */
    (void)GtRead(ctx, GT_REG_FW_VERSION, fw, sizeof(fw));
    (void)GtRead(ctx, GT_REG_CONFIG_XMAX, res, sizeof(res));
    GT_LOG("Goodix \"%c%c%c%c\" firmware 0x%04X, configured %ux%u, %s",
           id[0], id[1], id[2], id[3] ? id[3] : ' ',
           fw[0] | (fw[1] << 8),
           res[0] | (res[1] << 8), res[2] | (res[3] << 8),
           ctx->Polled ? "polled (no interrupt in _CRS)" : "interrupt");

    if (!ctx->VhfStarted) {
        status = VhfStart(ctx->Vhf);
        if (!NT_SUCCESS(status)) {
            GT_LOG("VhfStart failed 0x%08X", status);
            return status;
        }
        ctx->VhfStarted = TRUE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
GtEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->SpbTarget != NULL) {
        WdfIoTargetClose(ctx->SpbTarget);
        WdfObjectDelete(ctx->SpbTarget);
        ctx->SpbTarget = NULL;
    }
    /* An interrupt created in PrepareHardware is deleted by the framework. */
    ctx->Interrupt = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
GtEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    ctx->DownCount = 0;
    ctx->ReadErrors = 0;
    /*
     * A frame the firmware read last may still be flagged ready; the
     * controller sends nothing new until the flag is cleared.
     */
    (void)GtClearStatus(ctx);
    ctx->Running = TRUE;
    GtArmTimer(ctx);
    WdfWaitLockRelease(ctx->Lock);

    return STATUS_SUCCESS;
}

NTSTATUS
GtEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    ctx->Running = FALSE;
    /* Lift whatever is down, so that no touch outlives the device. */
    GtReportFrame(ctx, NULL, 0);
    WdfWaitLockRelease(ctx->Lock);

    WdfTimerStop(ctx->Timer, TRUE);
    return STATUS_SUCCESS;
}

VOID
GtEvtDeviceCleanup(
    _In_ WDFOBJECT Object
    )
{
    PGT_DEVICE_CONTEXT ctx = GtGetContext((WDFDEVICE)Object);

    if (ctx->Vhf != NULL) {
        VhfDelete(ctx->Vhf, TRUE);
        ctx->Vhf = NULL;
    }
}

NTSTATUS
GtEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_TIMER_CONFIG tcfg;
    WDFDEVICE device;
    PGT_DEVICE_CONTEXT ctx;
    VHF_CONFIG vcfg;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = GtEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = GtEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = GtEvtD0Entry;
    pnp.EvtDeviceD0Exit = GtEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, GT_DEVICE_CONTEXT);
    attr.EvtCleanupCallback = GtEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        GT_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    ctx = GtGetContext(device);
    ctx->Device = device;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfWaitLockCreate(&attr, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&tcfg, GtEvtTimer);
    tcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&tcfg, &attr, &ctx->Timer);
    if (!NT_SUCCESS(status)) {
        GT_LOG("WdfTimerCreate failed 0x%08X", status);
        return status;
    }

    /* Created now, started once the controller has answered. */
    VHF_CONFIG_INIT(&vcfg, WdfDeviceWdmGetDeviceObject(device),
                    GtReportDescriptorLength, GtReportDescriptor);
    vcfg.VhfClientContext = ctx;
    vcfg.EvtVhfAsyncOperationGetFeature = GtEvtVhfGetFeature;
    status = VhfCreate(&vcfg, &ctx->Vhf);
    if (!NT_SUCCESS(status)) {
        GT_LOG("VhfCreate failed 0x%08X (is vhf the lower filter?)", status);
        ctx->Vhf = NULL;
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, GtEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}
