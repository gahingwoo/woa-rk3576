/*
 * driver.c - RK3576 SARADC (successive-approximation ADC) for Windows
 *
 * A KMDF function driver for ACPI\RKCP0ADC, the SARADC at 0x2AE00000 that
 * the edk2-rk3576 firmware describes (Saradc.asl).  It does three things,
 * all from what the board's _DSD says is wired to the ADC:
 *
 *   adc-keys   a resistor-ladder button group on one channel, sampled every
 *              poll interval and reported as HID buttons through vhf.sys
 *              (the in-box Virtual HID Framework, this driver's lower
 *              filter): volume and media keys as consumer usages, power
 *              keys as system control, anything else (Recovery, Maskrom)
 *              as buttons on the vendor page 0xFF00.
 *   jack       a headphone jack read on one channel (ROCK 4D: the
 *              microphone contact, SARADC_VIN3_HP_HOOK), debounced with
 *              hysteresis and announced on the callback object
 *              \Callback\Rk3xSaradcJack, which es8323.sys listens to.
 *   raw reads  \\.\RK3XSARADC, IOCTL_RK3X_SARADC_READ_RAW, for adctest.exe.
 *
 * With no keys and no jack in _DSD (or no _DSD at all) the driver starts,
 * converts only when asked through the IOCTL, and reports nothing.
 *
 * The converter, from RK3576 TRM V1.2 Part 1 chapter 18 and Linux
 * drivers/iio/adc/rockchip_saradc.c (rockchip_saradc_start_v2, read_v2):
 * single mode, one channel per conversion, completion seen in END_INT_ST.
 * The end-of-conversion interrupt status is polled, not taken as an
 * interrupt: a conversion at the firmware's 1 MHz converter clock takes
 * about 60 us, and the driver converts at most two channels every 100 ms.
 * The status bit is cleared after every conversion, so the level-high
 * interrupt line in _CRS is never left asserted.
 *
 * What this driver never touches: the CRU.  Linux and U-Boot reset the
 * controller through SRST_P_SARADC (CRU SOFTRST_CON13 bit 6) before every
 * v2 conversion, because, as U-Boot quotes Rockchip's kernel, "if read
 * other chn at anytime, then chn1 will error".  Windows has no reset
 * framework for an ACPI device, and a wrong CRU write once reset the BUS
 * interconnect on every boot (edk2-rk3576 history: RK3588's SRST_P_SARADC
 * number decoded on RK3576 is hresetn_bus_biu).  So the firmware owns the
 * reset: \_SB.ADC0.RSTC pulses that one bit, and this driver evaluates it
 * at start and whenever a conversion is on a different channel from the
 * last one.  A firmware without RSTC gets no resets, and the driver says
 * so (DiagResetAvailable 0).  The firmware also opens the clock gates
 * (their reset state) and sets the converter clock to 24 MHz / 24 = 1 MHz,
 * the rate Linux asks for.  This driver itself only reads and writes the
 * SARADC's own registers.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xsaradc.h"
#include <acpiioct.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD RkAdcEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP RkAdcEvtDeviceCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE RkAdcEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE RkAdcEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY RkAdcEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT RkAdcEvtD0Exit;
EVT_WDF_TIMER RkAdcEvtTimer;

#define RKADC_READ(ctx, off)        READ_REGISTER_ULONG((volatile ULONG *)((ctx)->Regs + (off)))
#define RKADC_WRITE(ctx, off, v)    WRITE_REGISTER_ULONG((volatile ULONG *)((ctx)->Regs + (off)), (v))

/* ------------------------------------------------------------ converter */

/*
 * \_SB.ADC0.RSTC: assert and release SRST_P_SARADC, in AML (Saradc.asl).
 * Every SARADC register goes back to its reset value; the conversion code
 * programs the ones it needs each time.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static
NTSTATUS
RkAdcResetLocked(
    _In_ PRKADC_CONTEXT ctx
    )
{
    ACPI_EVAL_INPUT_BUFFER input;
    WDF_MEMORY_DESCRIPTOR inDesc;
    NTSTATUS status;

    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    input.MethodNameAsUlong = (ULONG)'CTSR';        /* "RSTC" */
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, &input, (ULONG)sizeof(input));
    status = WdfIoTargetSendInternalIoctlSynchronously(WdfDeviceGetIoTarget(ctx->Device), NULL,
                                                       IOCTL_ACPI_EVAL_METHOD, &inDesc, NULL,
                                                       NULL, NULL);
    if (NT_SUCCESS(status)) {
        ctx->Resets++;
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
RkAdcConvertLocked(
    _In_ PRKADC_CONTEXT ctx,
    _In_ ULONG channel,
    _Out_ PULONG raw
    )
{
    ULONG mask = SARADC_CONV_START | SARADC_CONV_SINGLE | SARADC_CONV_CHANNEL;
    ULONG waited;

    *raw = 0;
    if (ctx->Regs == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (channel >= RKADC_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (ctx->HaveReset && channel != ctx->LastChannel) {
        if (!NT_SUCCESS(RkAdcResetLocked(ctx))) {
            /* Go on without: a conversion is still better than none. */
            ctx->HaveReset = FALSE;
            RKADC_LOG("RSTC failed, no more controller resets%s", "");
        }
    }
    ctx->LastChannel = channel;

    /* The timings Linux programs before every v2 conversion. */
    RKADC_WRITE(ctx, SARADC_T_DAS_SOC, 0xC);
    RKADC_WRITE(ctx, SARADC_T_PD_SOC, 0x20);
    /* A stale end-of-conversion must not be taken for this one. */
    RKADC_WRITE(ctx, SARADC_END_INT_ST, SARADC_END_INT);
    RKADC_WRITE(ctx, SARADC_END_INT_EN, (SARADC_END_INT << 16) | SARADC_END_INT);
    RKADC_WRITE(ctx, SARADC_CONV_CON,
                (mask << 16) | SARADC_CONV_START | SARADC_CONV_SINGLE | channel);

    for (waited = 0; waited < RKADC_CONV_TIMEOUT_US; waited += 2) {
        if (RKADC_READ(ctx, SARADC_END_INT_ST) & SARADC_END_INT) {
            break;
        }
        KeStallExecutionProcessor(2);
    }
    if (waited >= RKADC_CONV_TIMEOUT_US) {
        ctx->Timeouts++;
        ctx->TimeoutsInRow++;
        RKADC_LOG("channel %u: no end of conversion in %u us (STATUS 0x%08X)",
                  channel, RKADC_CONV_TIMEOUT_US, RKADC_READ(ctx, SARADC_STATUS));
        return STATUS_IO_TIMEOUT;
    }

    RKADC_WRITE(ctx, SARADC_END_INT_ST, SARADC_END_INT);
    *raw = RKADC_READ(ctx, SARADC_DATA(channel)) & SARADC_DATA_MASK;
    ctx->LastRaw[channel] = *raw;
    ctx->Conversions++;
    ctx->TimeoutsInRow = 0;
    return STATUS_SUCCESS;
}

static
VOID
RkAdcQuiesceLocked(
    _In_ PRKADC_CONTEXT ctx
    )
{
    if (ctx->Regs != NULL) {
        RKADC_WRITE(ctx, SARADC_END_INT_EN, SARADC_END_INT << 16);
        RKADC_WRITE(ctx, SARADC_END_INT_ST, SARADC_END_INT);
    }
}

/* ------------------------------------------------------------ diagnostics */

static
VOID
DiagPut(
    _In_ WDFKEY key,
    _In_ PCWSTR name,
    _In_ ULONG value
    )
{
    UNICODE_STRING us;

    RtlInitUnicodeString(&us, name);
    (VOID)WdfRegistryAssignULong(key, &us, value);
}

/*
 * Written into the device's "Device Parameters" key, which is what a
 * tester can read back without a debugger:
 *   reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0ADC\0\Device Parameters"
 */
VOID
RkAdcDiagSave(
    _In_ PRKADC_CONTEXT ctx
    )
{
    WDFKEY key;
    RKADC_CONTEXT snap;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    WdfWaitLockAcquire(ctx->Lock, NULL);
    snap = *ctx;
    WdfWaitLockRelease(ctx->Lock);

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(ctx->Device, PLUGPLAY_REGKEY_DEVICE,
                                             KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    DiagPut(key, L"DiagDsdStatus", (ULONG)snap.DsdStatus);
    DiagPut(key, L"DiagVrefMicrovolts", snap.VrefMicrovolts);
    DiagPut(key, L"DiagPollMs", snap.PollMs);
    DiagPut(key, L"DiagKeysChannel", snap.HaveKeys ? snap.KeysChannel : RK3X_SARADC_NO_CHANNEL);
    DiagPut(key, L"DiagKeyCount", snap.HaveKeys ? snap.Keymap.Count : 0);
    DiagPut(key, L"DiagHidStarted", snap.VhfStarted);
    DiagPut(key, L"DiagJackChannel", snap.HaveJack ? snap.JackChannel : RK3X_SARADC_NO_CHANNEL);
    DiagPut(key, L"DiagJackConnected",
            snap.Jack.Known ? (ULONG)snap.Jack.Connected : 0xFFFFFFFFu);
    DiagPut(key, L"DiagJackMicrovolts", snap.JackMicrovolts);
    DiagPut(key, L"DiagConversions", snap.Conversions);
    DiagPut(key, L"DiagTimeouts", snap.Timeouts);
    DiagPut(key, L"DiagStopped", snap.Stopped);
    DiagPut(key, L"DiagNotifications", snap.Notifications);
    DiagPut(key, L"DiagHidReports", snap.HidReports);
    DiagPut(key, L"DiagKeysArmed", snap.HaveKeys ? (ULONG)snap.KeyArm.Armed : 0);
    DiagPut(key, L"DiagResetAvailable", snap.HaveReset);
    DiagPut(key, L"DiagResets", snap.Resets);
    WdfRegistryClose(key);
}

/* ------------------------------------------------------------ polling */

static
VOID
RkAdcArmTimerLocked(
    _In_ PRKADC_CONTEXT ctx
    )
{
    if (ctx->Polling) {
        WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(ctx->PollMs));
    }
}

VOID
RkAdcEvtTimer(
    _In_ WDFTIMER Timer
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext((WDFDEVICE)WdfTimerGetParentObject(Timer));
    ULONGLONG nowMs = KeQueryInterruptTime() / 10000;
    BOOLEAN jackChanged = FALSE;
    BOOLEAN stopped = FALSE;
    ULONG raw = 0;
    ULONG keysUv = 0;
    BOOLEAN haveKeysSample = FALSE;
    BOOLEAN armedNow = FALSE;
    NTSTATUS status;

    WdfWaitLockAcquire(ctx->Lock, NULL);
    if (!ctx->Polling) {
        WdfWaitLockRelease(ctx->Lock);
        return;
    }

    if (ctx->HaveKeys) {
        status = RkAdcConvertLocked(ctx, ctx->KeysChannel, &raw);
        if (NT_SUCCESS(status)) {
            keysUv = SaradcRawToMicrovolts(raw, ctx->VrefMicrovolts, RKADC_BITS);
            haveKeysSample = TRUE;
            /* No key counts until the keys have been seen released: a
             * key held through boot, or a converter reading 0 V, must not
             * press the power button. */
            if (SaradcDebounceStep(&ctx->KeyDebounce,
                                   SaradcKeyArmStep(&ctx->KeyArm,
                                                    SaradcKeysClassify(&ctx->Keymap, keysUv)))) {
                RkAdcHidReport(ctx, ctx->KeyDebounce.Stable);
            }
            armedNow = (BOOLEAN)(ctx->KeyArm.Armed && !ctx->KeyArmSaved);
            if (armedNow) {
                ctx->KeyArmSaved = TRUE;
            }
        }
    }

    if (ctx->HaveJack) {
        if (haveKeysSample && ctx->JackChannel == ctx->KeysChannel) {
            status = STATUS_SUCCESS;
        } else {
            status = RkAdcConvertLocked(ctx, ctx->JackChannel, &raw);
        }
        if (NT_SUCCESS(status)) {
            ULONG uv = SaradcRawToMicrovolts(raw, ctx->VrefMicrovolts, RKADC_BITS);
            if (SaradcJackStep(&ctx->Jack, uv, nowMs)) {
                ctx->JackMicrovolts = uv;
                ctx->JackSequence++;
                jackChanged = TRUE;
            }
        }
    }

    if (ctx->TimeoutsInRow >= RKADC_MAX_TIMEOUTS) {
        /* The converter does not answer: its clock is off, or this is not
         * the block the firmware says it is.  Stop rather than spin. */
        RKADC_LOG("%u conversions in a row timed out, polling stopped", ctx->TimeoutsInRow);
        ctx->Polling = FALSE;
        ctx->Stopped = TRUE;
        stopped = TRUE;
        if (ctx->HaveKeys && ctx->KeyDebounce.Stable != SARADC_NO_KEY) {
            SaradcDebounceInit(&ctx->KeyDebounce, SARADC_NO_KEY, ctx->KeySamples);
            RkAdcHidReport(ctx, SARADC_NO_KEY);
        }
    }

    RkAdcArmTimerLocked(ctx);
    WdfWaitLockRelease(ctx->Lock);

    if (jackChanged) {
        RkAdcJackNotify(ctx);
    }
    if (jackChanged || stopped || armedNow) {
        RkAdcDiagSave(ctx);
    }
}

/* ------------------------------------------------------------ PnP and power */

NTSTATUS
RkAdcEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR res;
    ULONG i, count;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i++) {
        res = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (res->Type != CmResourceTypeMemory || ctx->Regs != NULL) {
            /* The interrupt in _CRS is not used: completion is polled. */
            continue;
        }
        if (res->u.Memory.Length < SARADC_REGS_MIN) {
            RKADC_LOG("register window 0x%X bytes, need 0x%X", res->u.Memory.Length, SARADC_REGS_MIN);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        ctx->RegsLength = PAGE_SIZE;
        ctx->Regs = (PUCHAR)MmMapIoSpaceEx(res->u.Memory.Start, ctx->RegsLength,
                                           PAGE_READWRITE | PAGE_NOCACHE);
        if (ctx->Regs == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (ctx->Regs == NULL) {
        RKADC_LOG("no register window in _CRS%s", "");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    RkAdcReadConfig(ctx);

    /* The reset Linux does at probe, and the test that RSTC exists. */
    WdfWaitLockAcquire(ctx->Lock, NULL);
    status = RkAdcResetLocked(ctx);
    ctx->HaveReset = (BOOLEAN)NT_SUCCESS(status);
    ctx->LastChannel = RK3X_SARADC_NO_CHANNEL;
    WdfWaitLockRelease(ctx->Lock);
    if (!ctx->HaveReset) {
        RKADC_LOG("no controller reset (RSTC: 0x%08X); reading more than one channel "
                  "may give wrong values on channel 1", status);
    }

    if (ctx->HaveJack) {
        status = RkAdcJackOpen(ctx);
        if (!NT_SUCCESS(status)) {
            RKADC_LOG("jack callback object: 0x%08X, no jack reports", status);
            ctx->HaveJack = FALSE;
        }
    }

    if (ctx->HaveKeys && ctx->Vhf != NULL && !ctx->VhfStarted) {
        status = VhfStart(ctx->Vhf);
        if (NT_SUCCESS(status)) {
            ctx->VhfStarted = TRUE;
        } else {
            RKADC_LOG("VhfStart failed 0x%08X, no key reports", status);
        }
    }

    RkAdcDiagSave(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS
RkAdcEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    RkAdcJackClose(ctx);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    RkAdcQuiesceLocked(ctx);
    if (ctx->Regs != NULL) {
        MmUnmapIoSpace(ctx->Regs, ctx->RegsLength);
        ctx->Regs = NULL;
    }
    WdfWaitLockRelease(ctx->Lock);
    return STATUS_SUCCESS;
}

NTSTATUS
RkAdcEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    ctx->TimeoutsInRow = 0;
    ctx->Stopped = FALSE;
    if (ctx->HaveKeys) {
        /* After start and after every resume: released first. */
        SaradcKeyArmInit(&ctx->KeyArm, RKADC_KEY_ARM_SAMPLES);
        ctx->KeyArmSaved = FALSE;
        SaradcDebounceInit(&ctx->KeyDebounce, SARADC_NO_KEY, ctx->KeySamples);
    }
    ctx->Polling = (BOOLEAN)(ctx->HaveJack || (ctx->HaveKeys && ctx->VhfStarted));
    if (ctx->Polling) {
        WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(10));
    }
    WdfWaitLockRelease(ctx->Lock);
    return STATUS_SUCCESS;
}

NTSTATUS
RkAdcEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    ctx->Polling = FALSE;
    /* No key outlives the device. */
    if (ctx->HaveKeys && ctx->KeyDebounce.Stable != SARADC_NO_KEY) {
        SaradcDebounceInit(&ctx->KeyDebounce, SARADC_NO_KEY, ctx->KeySamples);
        RkAdcHidReport(ctx, SARADC_NO_KEY);
    }
    RkAdcQuiesceLocked(ctx);
    WdfWaitLockRelease(ctx->Lock);

    WdfTimerStop(ctx->Timer, TRUE);
    RkAdcDiagSave(ctx);
    return STATUS_SUCCESS;
}

VOID
RkAdcEvtDeviceCleanup(
    _In_ WDFOBJECT Object
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext((WDFDEVICE)Object);

    if (ctx->Vhf != NULL) {
        VhfDelete(ctx->Vhf, TRUE);
        ctx->Vhf = NULL;
    }
}

NTSTATUS
RkAdcEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_TIMER_CONFIG tcfg;
    WDF_WORKITEM_CONFIG wcfg;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    PRKADC_CONTEXT ctx;
    VHF_CONFIG vcfg;
    UNICODE_STRING link;
    NTSTATUS status;
    ULONG i;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = RkAdcEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = RkAdcEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = RkAdcEvtD0Entry;
    pnp.EvtDeviceD0Exit = RkAdcEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, RKADC_CONTEXT);
    attr.EvtCleanupCallback = RkAdcEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        RKADC_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    ctx = RkAdcGetContext(device);
    ctx->Device = device;
    ctx->KeysChannel = RK3X_SARADC_NO_CHANNEL;
    ctx->JackChannel = RK3X_SARADC_NO_CHANNEL;
    ctx->LastChannel = RK3X_SARADC_NO_CHANNEL;
    for (i = 0; i < RKADC_CHANNELS; i++) {
        ctx->LastRaw[i] = 0xFFFFFFFFu;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfWaitLockCreate(&attr, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&tcfg, RkAdcEvtTimer);
    tcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&tcfg, &attr, &ctx->Timer);
    if (!NT_SUCCESS(status)) {
        RKADC_LOG("WdfTimerCreate failed 0x%08X", status);
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&wcfg, RkAdcJackWork);
    wcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfWorkItemCreate(&wcfg, &attr, &ctx->JackWork);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoDeviceControl = RkAdcEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&link, RK3X_SARADC_DOS_NAME);
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) {
        /* adctest cannot reach it; keys and jack still work. */
        RKADC_LOG("WdfDeviceCreateSymbolicLink failed 0x%08X", status);
    }

    /*
     * The HID descriptor is fixed (hid.c), so VHF can be created before
     * _DSD is read.  It is started in PrepareHardware only when the
     * firmware describes keys; until then there is no HID child at all.
     */
    VHF_CONFIG_INIT(&vcfg, WdfDeviceWdmGetDeviceObject(device),
                    RkAdcReportDescriptorLength, (PUCHAR)RkAdcReportDescriptor);
    vcfg.VhfClientContext = ctx;
    status = VhfCreate(&vcfg, &ctx->Vhf);
    if (!NT_SUCCESS(status)) {
        RKADC_LOG("VhfCreate failed 0x%08X (is vhf the lower filter?), no key reports", status);
        ctx->Vhf = NULL;
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

    WDF_DRIVER_CONFIG_INIT(&config, RkAdcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}
