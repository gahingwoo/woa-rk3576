/*
 * driver.c - bcmwlsdio: KMDF function driver for the BCM43456 (AP6256)
 * SDIO Wi-Fi on the TaishanPi 3M and the BCM43752 (BW3752-50B1) on the
 * ArmSoM CM5; see docs/dev/wifi.md. The firmware file names come from the
 * chip ID (BwlChipFwName): brcmfmac43456-sdio.* or brcmfmac43752-sdio.*.
 *
 * Stages 0 to 2 (bring-up): PrepareHardware opens the SDBUS interface,
 * SelfManagedIoInit queues the bring-up on a work item (it takes seconds
 * and must not hold up the PnP start), SelfManagedIoSuspend stops it and
 * halts the chip's CPU, ReleaseHardware closes the interface.
 *
 * Stage 3 (Wi-Fi adapter, bcmwlwifi.inf, function 1 only): DeviceAdd sets
 * the device up for NetAdapterCx and WiFiCx; PrepareHardware runs the same
 * bring-up synchronously (WiFiCx wants the capabilities before it returns),
 * then the firmware's station set-up, then reports the capabilities;
 * SelfManagedIoInit starts the worker thread (wificx.cpp BwlWifiRun), which
 * from then on is the only thread that talks to the card: WDI requests,
 * the poller, the TX FIFO. Suspend stops the worker and leaves the
 * firmware running; ReleaseHardware halts the chip. No work runs at DIRQL:
 * there is no interrupt, the card is polled at PASSIVE_LEVEL.
 *
 * Everything is reported under the device's registry key (Device
 * Parameters) and flushed; docs/dev/wifi.md lists the values.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"
#include "bwl_regs.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD BwlEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE BwlEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE BwlEvtReleaseHardware;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT BwlEvtSelfManagedIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND BwlEvtSelfManagedIoSuspend;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP BwlEvtSelfManagedIoCleanup;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART BwlEvtSelfManagedIoRestart;
static EVT_WDF_WORKITEM BwlEvtBringUp;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP BwlEvtDeviceCleanup;
static EVT_WDF_DEVICE_SURPRISE_REMOVAL BwlEvtSurpriseRemoval;

static VOID EarlyCrumb(_In_ PCSTR Step);
static VOID EarlyCrumbF(_In_ PCSTR Fmt, _In_ ULONG A, _In_ ULONG B);
/* DEVPKEY_Device_InstanceId */
static const DEVPROPKEY BwlInstanceIdKey = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256
};

static VOID
BwlEvtBringUp(_In_ WDFWORKITEM WorkItem)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));

    /* 1 ms timer resolution while polling; restored when done. */
    (VOID)ExSetTimerResolution(10000, TRUE);
    (VOID)BwlBringUp(ctx);
    (VOID)ExSetTimerResolution(0, FALSE);
}

/* ------------------------------------------------------------ PnP */

static NTSTATUS
BwlEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    EarlyCrumbF("PrepareHardware (function %u, wifi %u)", ctx->AddFunction, ctx->WifiMode);
    InterlockedExchange(&ctx->Stopping, 0);
    ctx->Stage = BwlGetULong(ctx, L"Stage", BWL_STAGE_OPEN);
    ctx->MaxBlocks = BwlGetULong(ctx, L"MaxBlocks", 32);
    ctx->F2BlockMode = BwlGetULong(ctx, L"F2BlockMode", 0);
    ctx->Verify = BwlGetULong(ctx, L"Verify", 1);
    BwlPutULong(ctx, L"StageSeen", ctx->Stage);
    if (ctx->Stage >= BWL_STAGE_NETWORK && !ctx->WifiMode) {
        /* the Class System INF, or not function 1: the bring-up only */
        BwlCrumb(ctx, "stage 3 without the Wi-Fi adapter (bcmwlwifi.inf, function 1): running stage 2");
    }

    ctx->PollMs = BwlGetULong(ctx, L"PollMs", 2);
    ctx->IdlePollMs = BwlGetULong(ctx, L"IdlePollMs", 10);

    status = BwlSdOpen(ctx);
    BwlPutULong(ctx, L"SdOpenStatus", (ULONG)status);
    if (NT_SUCCESS(status)) {
        UCHAR fn = 0;

        if (NT_SUCCESS(BwlSdGetProperty(ctx, SDP_FUNCTION_NUMBER, &fn, sizeof(fn)))) {
            ctx->Function = fn;
        }
    }
    BwlCrumb(ctx, NT_SUCCESS(status) ? "SDBUS open" : "SDBUS open failed");
    if (!ctx->WifiMode) {
        /* The device still starts, so that the breadcrumbs can be read. */
        return STATUS_SUCCESS;
    }

    /* Stage 3: the firmware now, then the capabilities; WiFiCx asks for
     * the adapter after this returns (EvtWifiDeviceCreateAdapter). */
    ctx->FwReady = FALSE;
    if (NT_SUCCESS(status)) {
        ctx->FwReady = BwlNetworkUp(ctx);
        if (ctx->Function != 1) {
            BwlCrumb(ctx, "stage 3: this instance is not function 1 after all");
        }
    }
    status = BwlWifiReportCapabilities(ctx);
    BwlPutULong(ctx, L"WifiCapsStatus", (ULONG)status);
    if (NT_SUCCESS(status)) {
        /* WiFiCx may send WDI commands (the port, the adapter) and wait
         * for them before this device's start reaches SelfManagedIoInit:
         * the worker has to run from here on */
        status = BwlStartWorker(ctx);
        BwlPutULong(ctx, L"WorkerStartStatus", (ULONG)status);
    }
    return status;
}

static NTSTATUS
BwlEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    BWL_LOG("FN%u teardown: release hardware", ctx->Function);
    BwlStopWorker(ctx);
    BwlWifiReleaseHardware(ctx);
    BwlStopChip(ctx);
    ctx->FwReady = FALSE;
    ctx->Bwl.FwRunning = 0;
    BwlSdClose(ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    if (ctx->WifiMode) {
        /* started in PrepareHardware; the worker runs the WDI requests
         * even without firmware, to refuse them */
        return BwlStartWorker(ctx);
    }
    if (ctx->SdOpen && ctx->BringUp != NULL) {
        WdfWorkItemEnqueue(ctx->BringUp);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtSelfManagedIoRestart(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    /* back from a low-power state: the firmware was left running */
    if (!ctx->WifiMode) {
        return STATUS_SUCCESS;
    }
    BwlWifiResume(ctx);
    return BwlStartWorker(ctx);
}

static NTSTATUS
BwlEvtSelfManagedIoSuspend(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    BWL_LOG("FN%u teardown: self-managed I/O suspend", ctx->Function);
    if (ctx->WifiMode) {
        /* stop polling; the chip keeps its firmware for the restart.
         * WDI requests from now until the restart (or ReleaseHardware)
         * are failed at once: nobody is left to run them, and WiFiCx may
         * wait for one while it stops the adapter */
        BwlStopWorker(ctx);
        BwlWifiPause(ctx);
        return STATUS_SUCCESS;
    }
    BwlStopChip(ctx);
    return STATUS_SUCCESS;
}

static VOID
BwlEvtSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    BWL_LOG("FN%u teardown: self-managed I/O cleanup", ctx->Function);
    BwlStopChip(ctx);
}

/* The card is gone: no more CMD52/CMD53, not even the CPU halt in BwlStopChip. */
static VOID
BwlEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    BWL_LOG("FN%u teardown: surprise removal", ctx->Function);
    InterlockedExchange(&ctx->Gone, 1);
}

/*
 * The device object is being deleted: the worker and the work item are
 * stopped and the interface closed if the device ever started. Close again
 * in case it did not, then free the bounce buffer, checking its guard.
 */
static VOID
BwlEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext((WDFDEVICE)Object);

    BWL_LOG("FN%u teardown: device cleanup", ctx->Function);
    BwlStopWorker(ctx);
    BwlWifiCleanup(ctx);
    BwlNetFree(ctx);
    BwlSdClose(ctx);
    if (ctx->Bounce != NULL) {
        BwlBufFree(NULL, ctx->Bounce, "device cleanup: bounce");
        ctx->Bounce = NULL;
    }
}

/* ------------------------------------------------------------ DeviceAdd */

/* Stage from Device Parameters, before WdfDeviceCreate (through the FDO init). */
static ULONG
ReadStage(_In_ PWDFDEVICE_INIT DeviceInit)
{
    DECLARE_CONST_UNICODE_STRING(name, L"Stage");
    WDFKEY key;
    ULONG v = BWL_STAGE_OPEN;

    if (NT_SUCCESS(WdfFdoInitOpenRegistryKey(DeviceInit, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &name, &v))) {
            v = BWL_STAGE_OPEN;
        }
        WdfRegistryClose(key);
    }
    return v;
}

/*
 * Early breadcrumbs: before the device exists there is no Device
 * Parameters key, so DriverEntry and DeviceAdd append to EarlyTrace in the
 * service's Parameters key, flushed at each step, so that a bugcheck
 * leaves them readable:
 *   reg query HKLM\SYSTEM\CurrentControlSet\Services\bcmwlsdio\Parameters
 * DriverEntry starts it afresh each boot.
 */
#define EARLY_MAX 3000
static WCHAR g_ParamsPath[256];
static CHAR g_Early[EARLY_MAX];
static ULONG g_EarlyLen, g_AddSeq;
static FAST_MUTEX g_EarlyLock;
static BOOLEAN g_EarlyReady;

static VOID
EarlyCrumb(_In_ PCSTR Step)
{
    UNICODE_STRING path, name;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    PWCHAR w;
    ULONG i, n;

    if (!g_EarlyReady || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    ExAcquireFastMutex(&g_EarlyLock);
    n = (ULONG)strlen(Step);
    if (g_EarlyLen + n + 2 < EARLY_MAX) {
        RtlCopyMemory(g_Early + g_EarlyLen, Step, n);
        g_EarlyLen += n;
        g_Early[g_EarlyLen++] = '\n';
        g_Early[g_EarlyLen] = 0;
    }
    w = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (g_EarlyLen + 1) * sizeof(WCHAR), BWL_POOL_TAG);
    if (w != NULL) {
        for (i = 0; i < g_EarlyLen; i++) {
            w[i] = (WCHAR)(UCHAR)g_Early[i];
        }
        w[g_EarlyLen] = 0;
        RtlInitUnicodeString(&path, g_ParamsPath);
        InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateKey(&key, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
            RtlInitUnicodeString(&name, L"EarlyTrace");
            (VOID)ZwSetValueKey(key, &name, 0, REG_SZ, w, (g_EarlyLen + 1) * sizeof(WCHAR));
            (VOID)ZwFlushKey(key);
            ZwClose(key);
        }
        ExFreePoolWithTag(w, BWL_POOL_TAG);
    }
    ExReleaseFastMutex(&g_EarlyLock);
    BWL_LOG("early: %s", Step);
}

static VOID
EarlyCrumbF(_In_ PCSTR Fmt, _In_ ULONG A, _In_ ULONG B)
{
    CHAR t[96];

    if (NT_SUCCESS(RtlStringCbPrintfA(t, sizeof(t), Fmt, A, B))) {
        EarlyCrumb(t);
    }
}

/*
 * One property of the device-to-be into Buf (UTF-16, Cap characters),
 * before WdfDeviceCreate. Fixed buffers only: the memory objects
 * WdfFdoInitAllocAndQueryProperty returns belong to the device init and
 * must not be deleted by the driver. Returns the characters read, 0 if
 * the property is missing or does not fit.
 */
#define PROP_CHARS 512
static ULONG
QueryString(_In_ PWDFDEVICE_INIT DeviceInit, _In_opt_ const DEVPROPKEY *Key, _In_ DEVICE_REGISTRY_PROPERTY Prop,
            _Out_writes_(Cap) PWCHAR Buf, _In_ ULONG Cap)
{
    ULONG len = 0;
    NTSTATUS status;

    RtlZeroMemory(Buf, Cap * sizeof(WCHAR));
    if (Key != NULL) {
        WDF_DEVICE_PROPERTY_DATA pd;
        DEVPROPTYPE type = 0;

        WDF_DEVICE_PROPERTY_DATA_INIT(&pd, Key);
        status = WdfFdoInitQueryPropertyEx(DeviceInit, &pd, (Cap - 1) * sizeof(WCHAR), Buf, &len, &type);
    } else {
        status = WdfFdoInitQueryProperty(DeviceInit, Prop, (Cap - 1) * sizeof(WCHAR), Buf, &len);
    }
    if (!NT_SUCCESS(status) || len > (Cap - 1) * sizeof(WCHAR)) {
        EarlyCrumbF("  query: status %08x, %u bytes", (ULONG)status, len);
        Buf[0] = 0;
        return 0;
    }
    return len / sizeof(WCHAR);
}

/* The setup class comes back as text: "{4d36e972-e325-11ce-bfc1-08002be10318}" is Net. */
static BOOLEAN
IsNetClass(_In_ PWDFDEVICE_INIT DeviceInit, _Out_writes_(PROP_CHARS) PWCHAR Buf)
{
    UNICODE_STRING got, net;

    if (QueryString(DeviceInit, NULL, DevicePropertyClassGuid, Buf, PROP_CHARS) == 0) {
        return FALSE;
    }
    RtlInitUnicodeString(&got, Buf);
    RtlInitUnicodeString(&net, L"{4d36e972-e325-11ce-bfc1-08002be10318}");
    return RtlEqualUnicodeString(&got, &net, TRUE);
}

/*
 * Which SDIO function this device is, before the device exists (WiFiCx
 * has to be set up before WdfDeviceCreate, and only function 1 gets it).
 * sdbus names it in the instance ID ("SD\VID_02D0&PID_A9BF&FN_1\3&...")
 * and in the hardware IDs; the instance ID, the hardware IDs and the
 * compatible IDs are tried in that order (BwlFindFn, host-tested). Which
 * one answered is recorded (FunctionSource). PrepareHardware checks the
 * answer against SDP_FUNCTION_NUMBER.
 */
static UCHAR
FunctionFromIds(_In_ PWDFDEVICE_INIT DeviceInit, _Out_writes_(PROP_CHARS) PWCHAR Buf, _Out_ PULONG Source)
{
    static const DEVICE_REGISTRY_PROPERTY props[] = { DevicePropertyHardwareID, DevicePropertyCompatibleIDs };
    ULONG i, n;
    int fn;

    *Source = 0;
    EarlyCrumb(" add: instance ID");
    n = QueryString(DeviceInit, &BwlInstanceIdKey, DevicePropertyHardwareID, Buf, PROP_CHARS);
    fn = BwlFindFn((const u16 *)Buf, n);
    if (fn >= 0) {
        *Source = 1;
        return (UCHAR)fn;
    }
    for (i = 0; i < RTL_NUMBER_OF(props); i++) {
        EarlyCrumb(i == 0 ? " add: hardware IDs" : " add: compatible IDs");
        n = QueryString(DeviceInit, NULL, props[i], Buf, PROP_CHARS);
        fn = BwlFindFn((const u16 *)Buf, n);
        if (fn >= 0) {
            *Source = 2 + i;
            return (UCHAR)fn;
        }
    }
    return 0xFF;
}

static NTSTATUS
BwlEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_WORKITEM_CONFIG wic;
    PBWL_DEVICE_CONTEXT ctx;
    WDFDEVICE device;
    NTSTATUS status;
    BOOLEAN net, wifi;
    ULONG stage, source = 0;
    UCHAR fn;
    PWCHAR buf;

    UNREFERENCED_PARAMETER(Driver);

    /* Stage 3 on function 1 under the Class Net INF: a WiFiCx device.
     * Every other instance (function 2 and 3, a lower stage, the Class
     * System INF) is a plain device that does what Stage says. An instance
     * whose IDs name no function is taken for function 1 (one PDO for the
     * card), and PrepareHardware tells if that was wrong. */
    buf = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, PROP_CHARS * sizeof(WCHAR), BWL_POOL_TAG);
    if (buf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    EarlyCrumbF("DeviceAdd #%u (IRQL %u)", InterlockedIncrement((volatile LONG *)&g_AddSeq), KeGetCurrentIrql());
    EarlyCrumb(" add: class");
    net = IsNetClass(DeviceInit, buf);
    EarlyCrumb(" add: stage");
    stage = ReadStage(DeviceInit);
    fn = FunctionFromIds(DeviceInit, buf, &source);
    ExFreePoolWithTag(buf, BWL_POOL_TAG);
    wifi = net && stage >= BWL_STAGE_NETWORK && (fn == 1 || fn == 0xFF);
    EarlyCrumbF(" add: net %u stage %u", net, stage);
    EarlyCrumbF(" add: function %u (source %u)", fn, source);
    if (wifi) {
        EarlyCrumb(" add: NetDeviceInitConfig");
        status = NetDeviceInitConfig(DeviceInit);
        if (NT_SUCCESS(status)) {
            EarlyCrumb(" add: WifiDeviceInitConfig");
            status = BwlWifiPreDeviceCreate(DeviceInit);
        }
        if (!NT_SUCCESS(status)) {
            EarlyCrumbF(" add: init config failed %08x", (ULONG)status, 0);
            return status;
        }
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BwlEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = BwlEvtReleaseHardware;
    pnp.EvtDeviceSelfManagedIoInit = BwlEvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoSuspend = BwlEvtSelfManagedIoSuspend;
    pnp.EvtDeviceSelfManagedIoRestart = BwlEvtSelfManagedIoRestart;
    pnp.EvtDeviceSelfManagedIoCleanup = BwlEvtSelfManagedIoCleanup;
    pnp.EvtDeviceSurpriseRemoval = BwlEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BWL_DEVICE_CONTEXT);
    attr.EvtCleanupCallback = BwlEvtDeviceCleanup;
    EarlyCrumb(" add: WdfDeviceCreate");
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    EarlyCrumbF(" add: WdfDeviceCreate %08x", (ULONG)status, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = BwlGetContext(device);
    ctx->Device = device;
    ctx->Function = 0xFF;
    ctx->WifiMode = wifi;
    ctx->AddFunction = fn;
    KeInitializeEvent(&ctx->WakeEvent, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&ctx->DataLock);
    ctx->RxHold = TRUE;
    BwlPutULong(ctx, L"NetClass", net);
    BwlPutULong(ctx, L"AddFunction", fn);
    BwlPutULong(ctx, L"FunctionSource", source);
    BwlPutULong(ctx, L"ActiveMode", wifi ? 1 : 0);
    if (wifi) {
        /* the adapter comes later, when WiFiCx asks for it */
        EarlyCrumb(" add: WifiDeviceInitialize");
        status = BwlWifiPostDeviceCreate(ctx);
        EarlyCrumbF(" add: WifiDeviceInitialize %08x", (ULONG)status, 0);
        BwlPutULong(ctx, L"WifiInitStatus", (ULONG)status);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Every CMD53 goes through this buffer: non-paged, for the MDLs. Freed in BwlEvtDeviceCleanup. */
    ctx->Bounce = (PUCHAR)BwlBufAlloc(POOL_FLAG_NON_PAGED, BWL_BOUNCE_MIN, "bounce");
    if (ctx->Bounce == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDF_WORKITEM_CONFIG_INIT(&wic, BwlEvtBringUp);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfWorkItemCreate(&wic, &attr, &ctx->BringUp);
    EarlyCrumbF(" add: done %08x (wifi %u)", (ULONG)status, wifi);
    return status;
}

/*
 * The image's real entry point (the linker's /ENTRY): before anything of
 * WDF runs, record in Services\bcmwlsdio\Parameters that this image was
 * entered and which class extension versions it asks for (PreEntry), then
 * call the KMDF stub's FxDriverEntry, which binds KMDF, NetAdapterCx and
 * WiFiCx and then calls DriverEntry; its result goes to PreEntryStatus.
 * PreEntry without EarlyTrace: a bind failed in the stub. Neither: the
 * image was never run. Plain Zw calls only; the security cookie is set up
 * inside FxDriverEntry, so the frame that spans it has no cookie check.
 */
#ifndef BWL_NETCX_MINOR
#error "BWL_NETCX_* and BWL_WIFICX_* come from bcmwlsdio.vcxproj"
#endif

EXTERN_C NTSTATUS FxDriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath);
DRIVER_INITIALIZE BwlPreEntry;

static VOID
PreCrumb(_In_ PUNICODE_STRING RegistryPath, _In_opt_ PCWSTR Text, _In_ ULONG Status)
{
    WCHAR path[300];
    UNICODE_STRING p, n;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;

    if (!NT_SUCCESS(RtlStringCchCopyNW(path, RTL_NUMBER_OF(path), RegistryPath->Buffer,
                                       RegistryPath->Length / sizeof(WCHAR))) ||
        !NT_SUCCESS(RtlStringCchCatW(path, RTL_NUMBER_OF(path), L"\\Parameters"))) {
        return;
    }
    RtlInitUnicodeString(&p, path);
    InitializeObjectAttributes(&oa, &p, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        return;
    }
    if (Text != NULL) {
        RtlInitUnicodeString(&n, L"PreEntry");
        (VOID)ZwSetValueKey(key, &n, 0, REG_SZ, (PVOID)Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
        RtlInitUnicodeString(&n, L"EarlyTrace");
        (VOID)ZwDeleteValueKey(key, &n);
    }
    RtlInitUnicodeString(&n, L"PreEntryStatus");
    (VOID)ZwSetValueKey(key, &n, 0, REG_DWORD, &Status, sizeof(Status));
    (VOID)ZwFlushKey(key);
    ZwClose(key);
}

#define BWL_WSTR2(x) L ## #x
#define BWL_WSTR(x) BWL_WSTR2(x)

__declspec(safebuffers)
NTSTATUS
BwlPreEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;

    PreCrumb(RegistryPath,
             L"entered; binds NetAdapterCx " BWL_WSTR(BWL_NETCX_MAJOR) L"." BWL_WSTR(BWL_NETCX_MINOR)
             L", WiFiCx " BWL_WSTR(BWL_WIFICX_MAJOR) L"." BWL_WSTR(BWL_WIFICX_MINOR) L", KMDF 1.33",
             0xFFFFFFFFu);
    status = FxDriverEntry(DriverObject, RegistryPath);
    PreCrumb(RegistryPath, NULL, (ULONG)status);
    return status;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    NTSTATUS status;

    ExInitializeFastMutex(&g_EarlyLock);
    if (NT_SUCCESS(RtlStringCchCopyNW(g_ParamsPath, RTL_NUMBER_OF(g_ParamsPath), RegistryPath->Buffer,
                                      RegistryPath->Length / sizeof(WCHAR))) &&
        NT_SUCCESS(RtlStringCchCatW(g_ParamsPath, RTL_NUMBER_OF(g_ParamsPath), L"\\Parameters"))) {
        g_EarlyReady = TRUE;
    }
    EarlyCrumb("DriverEntry");
    WDF_DRIVER_CONFIG_INIT(&config, BwlEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    EarlyCrumbF("WdfDriverCreate %08x", (ULONG)status, 0);
    return status;
}
