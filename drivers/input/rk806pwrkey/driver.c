/*
 * driver.c - the RK806 power key as the Windows power button
 *
 * A KMDF function driver for ACPI\RKCP806B (\_SB.I2C1.PWRK, edk2-rk3576
 * PwrKey.asl, setup option "Power button (RK806)") with vhf.sys as its lower
 * filter. The board's power button is wired to the PMIC's PWRON pin; the
 * PMIC reports a press (PWRON_FALL) and a release (PWRON_RISE) in INT_STS0
 * and holds its INT line (GPIO0_A6) low until the status is written back.
 * The interrupt comes through GpioClx (rk3xgpio) as a passive-level ISR, so
 * the I2C read and acknowledge are done right in it, over SPB (rk3xi2c).
 *
 * A press is reported as HID Generic Desktop / System Control / System
 * Power Down, which Windows' HID button handling treats as the power button
 * ("Power and sleep buttons" in Power Options decides what it does). The
 * PMIC's own long press, which cuts the power, is not affected.
 *
 * Registers (mainline Linux include/linux/mfd/rk808.h, drivers/mfd/
 * rk8xx-core.c rk806_irqs and rk806_pre_init_reg, drivers/input/misc/
 * rk805-pwrkey.c):
 *   0x77 INT_STS0   bit 0 PWRON_FALL (pressed), bit 1 PWRON_RISE (released);
 *                   write 1 to clear
 *   0x78 INT_MSK0   1 = masked
 *   0x79 INT_STS1, 0x7A INT_MSK1
 *   0x7B GPIO_INT_CONFIG  bit 1: INT polarity, 0 = active low
 * Nothing else in the PMIC is written. The firmware sets the same masks at
 * ExitBootServices (Rk806I2cPowerOffDxe); they are set again here because
 * the driver can start after a PMIC that has been reset since.
 *
 * Written from the register description above and the Microsoft VHF, SPB
 * and GpioClx documentation. No code is taken from the Linux drivers.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <vhf.h>

#define PK_POOL_TAG             'K68R'
#define PK_I2C_TIMEOUT_MS       100

#define RK806_INT_STS0          0x77
#define RK806_INT_MSK0          0x78
#define RK806_INT_STS1          0x79
#define RK806_INT_MSK1          0x7A
#define RK806_GPIO_INT_CONFIG   0x7B
#define RK806_INT_POL_H         0x02
#define RK806_PWRON_FALL        0x01
#define RK806_PWRON_RISE        0x02

#define PK_REPORT_ID            1

#define PK_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "rk806pwrkey: " fmt "\n", __VA_ARGS__)

typedef struct _PK_CONTEXT {
    WDFDEVICE       Device;
    WDFIOTARGET     SpbTarget;
    LARGE_INTEGER   SpbConnectionId;
    BOOLEAN         HaveSpb;
    WDFINTERRUPT    Interrupt;
    VHFHANDLE       Vhf;
    BOOLEAN         VhfStarted;
    BOOLEAN         Pressed;
    ULONG           Presses;
    ULONG           I2cErrors;
} PK_CONTEXT, *PPK_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PK_CONTEXT, PkGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD PkEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP PkEvtDeviceCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE PkEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE PkEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY PkEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT PkEvtD0Exit;
EVT_WDF_INTERRUPT_ISR PkEvtInterruptIsr;

/*
 * Generic Desktop / System Control, one 2-bit array field: 0 = nothing,
 * 1 = System Power Down. The same collection rk3xsaradc reports its power
 * key through.
 */
static const UCHAR PkReportDescriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x80,             /* Usage (System Control) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, PK_REPORT_ID,     /*   Report ID (1) */
    0x19, 0x81,             /*   Usage Minimum (System Power Down) */
    0x29, 0x81,             /*   Usage Maximum (System Power Down) */
    0x15, 0x01,             /*   Logical Minimum (1) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x02,             /*   Report Size (2) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x00,             /*   Input (Data, Array, Absolute) */
    0x75, 0x06,             /*   Report Size (6) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x03,             /*   Input (Constant): padding */
    0xC0,                   /* End Collection */
};

/* ------------------------------------------------------------------ I2C */

static
NTSTATUS
PkRead(
    _In_ PPK_CONTEXT Ctx,
    _In_ UCHAR Reg,
    _Out_ PUCHAR Value
    )
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    WDF_MEMORY_DESCRIPTOR in;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR bytes = 0;
    NTSTATUS status;

    *Value = 0;
    SPB_TRANSFER_LIST_INIT(&seq.List, 2);
    seq.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, &Reg, 1);
    seq.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, Value, 1);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &seq, sizeof(seq));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(PK_I2C_TIMEOUT_MS));

    status = WdfIoTargetSendIoctlSynchronously(Ctx->SpbTarget, NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &in, NULL, &opts, &bytes);
    if (NT_SUCCESS(status) && bytes != 2) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return status;
}

static
NTSTATUS
PkWrite(
    _In_ PPK_CONTEXT Ctx,
    _In_ UCHAR Reg,
    _In_ UCHAR Value
    )
{
    UCHAR buf[2];
    WDF_MEMORY_DESCRIPTOR mem;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR bytes = 0;
    NTSTATUS status;

    buf[0] = Reg;
    buf[1] = Value;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&mem, buf, sizeof(buf));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(PK_I2C_TIMEOUT_MS));

    status = WdfIoTargetSendWriteSynchronously(Ctx->SpbTarget, NULL, &mem,
                                               NULL, &opts, &bytes);
    if (NT_SUCCESS(status) && bytes != sizeof(buf)) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return status;
}

/* ------------------------------------------------------------------ HID */

static
VOID
PkReport(
    _In_ PPK_CONTEXT Ctx,
    _In_ BOOLEAN Pressed
    )
{
    HID_XFER_PACKET packet;
    UCHAR report[2];
    NTSTATUS status;

    if (Ctx->Vhf == NULL || !Ctx->VhfStarted || Ctx->Pressed == Pressed) {
        return;
    }
    report[0] = PK_REPORT_ID;
    report[1] = Pressed ? 1 : 0;
    packet.reportBuffer = report;
    packet.reportBufferLen = sizeof(report);
    packet.reportId = PK_REPORT_ID;
    /* VHF copies the report. */
    status = VhfReadReportSubmit(Ctx->Vhf, &packet);
    if (!NT_SUCCESS(status)) {
        PK_LOG("VhfReadReportSubmit failed 0x%08X", status);
        return;
    }
    Ctx->Pressed = Pressed;
    if (Pressed) {
        Ctx->Presses++;
        PK_LOG("power key pressed (%u)", Ctx->Presses);
    }
}

/* ------------------------------------------------------------ interrupt */

/*
 * Active low, only the key's press and release unmasked, nothing pending.
 * Called at PASSIVE_LEVEL from D0Entry, before the interrupt is enabled.
 */
static
NTSTATUS
PkConfigurePmic(
    _In_ PPK_CONTEXT Ctx
    )
{
    UCHAR config;
    NTSTATUS status;

    status = PkRead(Ctx, RK806_GPIO_INT_CONFIG, &config);
    if (!NT_SUCCESS(status)) {
        PK_LOG("the RK806 does not answer at the I2C address in _CRS (0x%08X)", status);
        return status;
    }
    if (config & RK806_INT_POL_H) {
        (void)PkWrite(Ctx, RK806_GPIO_INT_CONFIG, (UCHAR)(config & ~RK806_INT_POL_H));
    }
    (void)PkWrite(Ctx, RK806_INT_MSK0, (UCHAR)~(RK806_PWRON_FALL | RK806_PWRON_RISE));
    (void)PkWrite(Ctx, RK806_INT_MSK1, 0xFF);
    (void)PkWrite(Ctx, RK806_INT_STS0, 0xFF);
    status = PkWrite(Ctx, RK806_INT_STS1, 0xFF);
    PK_LOG("RK806 interrupt set up (GPIO_INT_CONFIG was 0x%02X): 0x%08X", config, status);
    return status;
}

BOOLEAN
PkEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PPK_CONTEXT ctx = PkGetContext(WdfInterruptGetDevice(Interrupt));
    UCHAR sts0 = 0, sts1 = 0;
    NTSTATUS status;
    LARGE_INTEGER backoff;

    UNREFERENCED_PARAMETER(MessageID);

    /* Passive-level ISR (PassiveHandling): I2C can be done here. */
    status = PkRead(ctx, RK806_INT_STS0, &sts0);
    if (NT_SUCCESS(status)) {
        (void)PkRead(ctx, RK806_INT_STS1, &sts1);
        /* Write back what was set: clears it, and releases the INT line. */
        if (sts0 != 0) {
            status = PkWrite(ctx, RK806_INT_STS0, sts0);
        }
        if (sts1 != 0) {
            (void)PkWrite(ctx, RK806_INT_STS1, sts1);
        }
    }

    if (!NT_SUCCESS(status)) {
        /*
         * The line stays low and the interrupt comes straight back. Do not
         * spin a CPU on it: an I2C bus that is gone answers no faster.
         */
        if ((ctx->I2cErrors++ % 100) == 0) {
            PK_LOG("cannot read or clear INT_STS0 (0x%08X, %u times)", status, ctx->I2cErrors);
        }
        backoff.QuadPart = -10 * 1000 * 100;   /* 100 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &backoff);
        return TRUE;
    }

    /*
     * A short tap can deliver both edges in one read: report the press and
     * then the release, so that Windows still sees the button.
     */
    if (sts0 & RK806_PWRON_FALL) {
        PkReport(ctx, TRUE);
    }
    if (sts0 & RK806_PWRON_RISE) {
        PkReport(ctx, FALSE);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ PnP */

NTSTATUS
PkEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PPK_CONTEXT ctx = PkGetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR raw, trans;
    WDF_INTERRUPT_CONFIG icfg;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_TARGET_OPEN_PARAMS open;
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
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
        if (trans->Type == CmResourceTypeConnection &&
            trans->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            trans->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C &&
            !ctx->HaveSpb) {
            ctx->SpbConnectionId.LowPart = trans->u.Connection.IdLowPart;
            ctx->SpbConnectionId.HighPart = trans->u.Connection.IdHighPart;
            ctx->HaveSpb = TRUE;
        } else if (trans->Type == CmResourceTypeInterrupt && ctx->Interrupt == NULL) {
            WDF_INTERRUPT_CONFIG_INIT(&icfg, PkEvtInterruptIsr, NULL);
            icfg.PassiveHandling = TRUE;
            icfg.InterruptRaw = raw;
            icfg.InterruptTranslated = trans;
            status = WdfInterruptCreate(Device, &icfg, WDF_NO_OBJECT_ATTRIBUTES,
                                        &ctx->Interrupt);
            if (!NT_SUCCESS(status)) {
                PK_LOG("WdfInterruptCreate failed 0x%08X", status);
                return status;
            }
        }
    }

    if (!ctx->HaveSpb || ctx->Interrupt == NULL) {
        PK_LOG("_CRS needs an I2cSerialBus and a GpioInt (have I2C %u, interrupt %u)",
               ctx->HaveSpb, ctx->Interrupt != NULL);
        return STATUS_NOT_FOUND;
    }

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
        return status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &path, GENERIC_READ | GENERIC_WRITE);
    status = WdfIoTargetOpen(ctx->SpbTarget, &open);
    if (!NT_SUCCESS(status)) {
        PK_LOG("opening the I2C connection failed 0x%08X", status);
        WdfObjectDelete(ctx->SpbTarget);
        ctx->SpbTarget = NULL;
        return status;
    }

    if (!ctx->VhfStarted) {
        status = VhfStart(ctx->Vhf);
        if (!NT_SUCCESS(status)) {
            PK_LOG("VhfStart failed 0x%08X", status);
            return status;
        }
        ctx->VhfStarted = TRUE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
PkEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PPK_CONTEXT ctx = PkGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->SpbTarget != NULL) {
        WdfIoTargetClose(ctx->SpbTarget);
        WdfObjectDelete(ctx->SpbTarget);
        ctx->SpbTarget = NULL;
    }
    ctx->Interrupt = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
PkEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    UNREFERENCED_PARAMETER(PreviousState);

    /*
     * A PMIC that does not answer is a failed start (code 10) rather than an
     * interrupt line nobody can clear.
     */
    return PkConfigurePmic(PkGetContext(Device));
}

NTSTATUS
PkEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PPK_CONTEXT ctx = PkGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    /* No key left down after the device goes. */
    PkReport(ctx, FALSE);
    return STATUS_SUCCESS;
}

VOID
PkEvtDeviceCleanup(
    _In_ WDFOBJECT Object
    )
{
    PPK_CONTEXT ctx = PkGetContext((WDFDEVICE)Object);

    if (ctx->Vhf != NULL) {
        VhfDelete(ctx->Vhf, TRUE);
        ctx->Vhf = NULL;
    }
}

NTSTATUS
PkEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE device;
    PPK_CONTEXT ctx;
    VHF_CONFIG vcfg;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = PkEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = PkEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = PkEvtD0Entry;
    pnp.EvtDeviceD0Exit = PkEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, PK_CONTEXT);
    attr.EvtCleanupCallback = PkEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        PK_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    ctx = PkGetContext(device);
    ctx->Device = device;

    VHF_CONFIG_INIT(&vcfg, WdfDeviceWdmGetDeviceObject(device),
                    (USHORT)sizeof(PkReportDescriptor), (PUCHAR)PkReportDescriptor);
    vcfg.VhfClientContext = ctx;
    status = VhfCreate(&vcfg, &ctx->Vhf);
    if (!NT_SUCCESS(status)) {
        PK_LOG("VhfCreate failed 0x%08X (is vhf the lower filter?)", status);
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

    WDF_DRIVER_CONFIG_INIT(&config, PkEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}
