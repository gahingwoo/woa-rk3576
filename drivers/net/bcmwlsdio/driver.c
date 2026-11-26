/*
 * driver.c - bcmwlsdio: KMDF function driver for the BCM43456 (AP6256)
 * SDIO Wi-Fi on the TaishanPi 3M and the BCM43752 (BW3752-50B1) on the
 * ArmSoM CM5. W3: bring-up only, see docs/dev/wifi.md. The firmware file
 * names come from the chip ID (BwlChipFwName): brcmfmac43456-sdio.* or
 * brcmfmac43752-sdio.*.
 *
 * PnP: PrepareHardware opens the SDBUS interface, SelfManagedIoInit
 * queues the bring-up on a work item (it takes seconds and must not hold
 * up the PnP start), SelfManagedIoSuspend stops it and halts the chip's
 * CPU, ReleaseHardware closes the interface.
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
static EVT_WDF_WORKITEM BwlEvtBringUp;

/* ------------------------------------------------------------ registry */

static NTSTATUS
OpenKey(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    return WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, Access,
                                    WDF_NO_OBJECT_ATTRIBUTES, Key);
}

VOID
BwlPutULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING n;

    if (NT_SUCCESS(OpenKey(Ctx, KEY_WRITE, &key))) {
        RtlInitUnicodeString(&n, Name);
        (VOID)WdfRegistryAssignULong(key, &n, Value);
        WdfRegistryClose(key);
    }
}

VOID
BwlPutWString(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCWSTR Value)
{
    WDFKEY key;
    UNICODE_STRING n, v;

    if (NT_SUCCESS(OpenKey(Ctx, KEY_WRITE, &key))) {
        RtlInitUnicodeString(&n, Name);
        RtlInitUnicodeString(&v, Value);
        (VOID)WdfRegistryAssignUnicodeString(key, &n, &v);
        WdfRegistryClose(key);
    }
}

/* ASCII to a registry string; control characters other than newline become '?'. */
#define PUT_MAX 2048
VOID
BwlPutString(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value)
{
    PWCHAR buf;
    ULONG i;

    buf = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (PUT_MAX + 1) * sizeof(WCHAR), BWL_POOL_TAG);
    if (buf == NULL) {
        return;
    }
    for (i = 0; i < PUT_MAX && Value[i]; i++) {
        CHAR c = Value[i];

        buf[i] = (WCHAR)((c == '\n' || (c >= 0x20 && c < 0x7F)) ? c : '?');
    }
    buf[i] = 0;
    BwlPutWString(Ctx, Name, buf);
    ExFreePoolWithTag(buf, BWL_POOL_TAG);
}

static ULONG
GetULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY key;
    UNICODE_STRING n;
    ULONG v = Default;

    if (NT_SUCCESS(OpenKey(Ctx, KEY_READ, &key))) {
        RtlInitUnicodeString(&n, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v))) {
            v = Default;
        }
        WdfRegistryClose(key);
    }
    return v;
}

/*
 * The step the bring-up is in, flushed to disk at once, so that a hang
 * leaves it readable after a reset: LastStep, PrevStep, StepSeq.
 */
VOID
BwlCrumb(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCSTR Step)
{
    DECLARE_UNICODE_STRING_SIZE(prev, 96);
    WCHAR step[96];
    UNICODE_STRING n, v;
    WDFKEY key;
    ULONG i;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    for (i = 0; i + 1 < RTL_NUMBER_OF(step) && Step[i]; i++) {
        step[i] = (WCHAR)(UCHAR)Step[i];
    }
    step[i] = 0;
    if (!NT_SUCCESS(OpenKey(Ctx, KEY_READ | KEY_WRITE, &key))) {
        return;
    }
    RtlInitUnicodeString(&n, L"LastStep");
    if (NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &n, NULL, &prev))) {
        RtlInitUnicodeString(&v, L"PrevStep");
        (VOID)WdfRegistryAssignUnicodeString(key, &v, &prev);
    }
    RtlInitUnicodeString(&v, step);
    (VOID)WdfRegistryAssignUnicodeString(key, &n, &v);
    RtlInitUnicodeString(&n, L"StepSeq");
    (VOID)WdfRegistryAssignULong(key, &n, (ULONG)InterlockedIncrement(&Ctx->StepSeq));
    (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    WdfRegistryClose(key);
    BWL_LOG("step: %s", Step);
}

static VOID
PutCounters(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    const bwl_stats *s = &Ctx->Bwl.Stats;

    BwlPutULong(Ctx, L"Cmd52Count", Ctx->Cmd52);
    BwlPutULong(Ctx, L"Cmd53Count", Ctx->Cmd53);
    BwlPutULong(Ctx, L"Cmd52Errors", Ctx->Cmd52Errors);
    BwlPutULong(Ctx, L"Cmd53Errors", Ctx->Cmd53Errors);
    BwlPutULong(Ctx, L"LastSdStatus", (ULONG)Ctx->LastSdStatus);
    BwlPutULong(Ctx, L"LastSdArg", Ctx->LastSdArg);
    BwlPutULong(Ctx, L"LastR5Flags", Ctx->LastR5Flags);
    BwlPutULong(Ctx, L"FramesTx", s->FramesTx);
    BwlPutULong(Ctx, L"FramesRx", s->FramesRx);
    BwlPutULong(Ctx, L"EventsRx", s->EventsRx);
    BwlPutULong(Ctx, L"LastEvent", s->LastEvent);
    BwlPutULong(Ctx, L"RxErrors", s->RxErrors);
    BwlPutULong(Ctx, L"RxAborts", s->RxAborts);
    BwlPutULong(Ctx, L"GlomRx", s->GlomRx);
    BwlPutULong(Ctx, L"MailboxData", s->MailboxData);
    BwlPutULong(Ctx, L"IntStatus", s->IntStatus);
    BwlPutULong(Ctx, L"Polls", s->Polls);
}

/* "failed: <step> (<error>)" into FwState, with the detail word. */
static VOID
Fail(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ int Error)
{
    CHAR msg[128];

    (VOID)RtlStringCbPrintfA(msg, sizeof(msg), "failed: %s (%d)", Ctx->Bwl.Step ? Ctx->Bwl.Step : "?", Error);
    BwlPutString(Ctx, L"FwState", msg);
    BwlPutULong(Ctx, L"LastError", (ULONG)Error);
    BwlPutULong(Ctx, L"ErrorDetail", Ctx->Bwl.Detail);
    PutCounters(Ctx);
    BwlCrumb(Ctx, msg);
}

/* ------------------------------------------------------------ bring-up */

static VOID
RecordBus(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    UCHAR fn = 0, width = 0;
    USHORT ver = 0, hostBlk = 0, fnBlk = 0;
    ULONG clock = 0;
    NTSTATUS status;

    status = BwlSdGetProperty(Ctx, SDP_FUNCTION_NUMBER, &fn, sizeof(fn));
    BwlPutULong(Ctx, L"FunctionNumberStatus", (ULONG)status);
    Ctx->Function = (UCHAR)(NT_SUCCESS(status) ? fn : 0xFF);
    BwlPutULong(Ctx, L"FunctionNumber", Ctx->Function);
    if (NT_SUCCESS(BwlSdGetProperty(Ctx, SDP_BUS_DRIVER_VERSION, &ver, sizeof(ver)))) {
        BwlPutULong(Ctx, L"BusDriverVersion", ver);
    }
    if (NT_SUCCESS(BwlSdGetProperty(Ctx, SDP_BUS_WIDTH, &width, sizeof(width)))) {
        BwlPutULong(Ctx, L"BusWidth", width);
    }
    if (NT_SUCCESS(BwlSdGetProperty(Ctx, SDP_BUS_CLOCK, &clock, sizeof(clock)))) {
        BwlPutULong(Ctx, L"BusClock", clock);
    }
    if (NT_SUCCESS(BwlSdGetProperty(Ctx, SDP_HOST_BLOCK_LENGTH, &hostBlk, sizeof(hostBlk)))) {
        BwlPutULong(Ctx, L"HostBlockLength", hostBlk);
    }
    if (NT_SUCCESS(BwlSdGetProperty(Ctx, SDP_FUNCTION_BLOCK_LENGTH, &fnBlk, sizeof(fnBlk)))) {
        BwlPutULong(Ctx, L"FunctionBlockLength", fnBlk);
    }
}

/* CCCR bytes 0..3 and 8, packed: rev | sdrev << 8 | ioex << 16 | iorx << 24; caps separately. */
static int
RecordCccr(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    u8 b[5] = { 0 };
    static const u32 reg[5] = { CCCR_REV, CCCR_SDREV, CCCR_IOEX, CCCR_IORX, CCCR_CAPS };
    ULONG i;
    int r;

    for (i = 0; i < 5; i++) {
        r = BwlRead8(&Ctx->Bwl, reg[i], &b[i]);
        if (r) {
            return r;
        }
    }
    BwlPutULong(Ctx, L"Cccr", (ULONG)b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24));
    BwlPutULong(Ctx, L"CccrCaps", b[4]);
    return 0;
}

static VOID
RecordChip(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    const bwl_dev *d = &Ctx->Bwl;
    const bwl_core *sd = BwlChipCore(d, CORE_SDIO_DEV, 0);
    CHAR cores[BWL_MAX_CORES * 32];
    size_t used = 0;
    unsigned i;

    BwlPutULong(Ctx, L"ChipIdReg", d->ChipIdReg);
    BwlPutULong(Ctx, L"ChipId", d->Chip);
    BwlPutULong(Ctx, L"ChipRev", d->ChipRev);
    BwlPutULong(Ctx, L"RamBase", d->RamBase);
    BwlPutULong(Ctx, L"RamSize", d->RamSize);
    BwlPutULong(Ctx, L"PmuRev", d->PmuRev);
    BwlPutULong(Ctx, L"SrCapable", d->SrCapable);
    BwlPutULong(Ctx, L"SdioCoreRev", sd ? sd->Rev : 0xFFFFFFFFu);
    cores[0] = 0;
    for (i = 0; i < d->NumCores; i++) {
        if (!NT_SUCCESS(RtlStringCbPrintfA(cores + used, sizeof(cores) - used, "%s%03x r%u @%08x",
                                           i ? ", " : "", d->Cores[i].Id, d->Cores[i].Rev, d->Cores[i].Base))) {
            break;
        }
        (VOID)RtlStringCbLengthA(cores, sizeof(cores), &used);
    }
    BwlPutString(Ctx, L"Cores", cores);
}

static VOID
RecordConsole(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PCHAR buf = (PCHAR)ExAllocatePool2(POOL_FLAG_PAGED, PUT_MAX + 1, BWL_POOL_TAG);

    if (buf == NULL) {
        return;
    }
    if (BwlConsoleRead(&Ctx->Bwl, buf, PUT_MAX + 1) > 0) {
        BwlPutString(Ctx, L"FwConsole", buf);
    }
    ExFreePoolWithTag(buf, BWL_POOL_TAG);
}

static VOID
BringUp(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    WDFMEMORY fwMem = NULL, nvMem = NULL, clmMem = NULL, convMem = NULL;
    PUCHAR fw = NULL, nvTxt = NULL, clm = NULL, conv = NULL;
    ULONG fwLen = 0, nvTxtLen = 0, clmLen = 0;
    WCHAR name[64];
    CHAR text[256], ver[64];
    const char *base;
    u8 mac[6];
    u32 v, chunks = 0, clmStatus = 0, outLen = 0;
    int fws = 0, r, n;
    USHORT blk = 64;
    WDF_OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    bwl_dev *d = &Ctx->Bwl;

    BwlCrumb(Ctx, "bring-up start");
    BwlPutString(Ctx, L"FwState", "starting");
    RecordBus(Ctx);
    if (Ctx->Stage == BWL_STAGE_OPEN) {
        BwlPutString(Ctx, L"FwState", "stage 0: SDBUS open, card not touched");
        BwlCrumb(Ctx, "stage 0 done");
        return;
    }
    if (Ctx->Function != 1) {
        /* The F2 PDO, if sdbus makes one: F1's driver instance does all the work. */
        BwlPutString(Ctx, L"FwState", "idle: not function 1");
        BwlCrumb(Ctx, "not function 1");
        return;
    }

    status = BwlSdSetProperty(Ctx, SDP_FUNCTION_BLOCK_LENGTH, &blk, sizeof(blk));
    BwlPutULong(Ctx, L"SetBlockLengthStatus", (ULONG)status);

    BwlInit(d, &BwlSdOps, Ctx, Ctx->Bounce, BWL_BOUNCE_MIN);
    d->MaxBlocks = (u16)(Ctx->MaxBlocks ? min(Ctx->MaxBlocks, 511u) : 32u);
    d->F2ByteModeOnly = (u8)(Ctx->F2BlockMode ? 0 : 1);

    BwlCrumb(Ctx, "CCCR");
    r = RecordCccr(Ctx);
    if (r) {
        d->Step = "CCCR read";
        Fail(Ctx, r);
        return;
    }

    BwlCrumb(Ctx, "attach");
    r = BwlAttach(d);
    RecordChip(Ctx);
    if (r) {
        Fail(Ctx, r);
        return;
    }
    base = BwlChipFwName(d->Chip, d->ChipRev);
    BwlPutString(Ctx, L"FwState", "identified");
    PutCounters(Ctx);
    if (Ctx->Stage < BWL_STAGE_FIRMWARE) {
        BwlCrumb(Ctx, "stage 1 done");
        return;
    }
    if (base == NULL) {
        d->Step = "no firmware name for this chip";
        Fail(Ctx, BWL_E_NOCHIP);
        return;
    }

    /* The three files */
    BwlCrumb(Ctx, "files");
    (VOID)RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"brcmfmac%S-sdio.bin", base);
    status = BwlLoadFile(Ctx, name, BWL_FW_MAX_BYTES, &fwMem, &fw, &fwLen);
    if (NT_SUCCESS(status)) {
        (VOID)RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"brcmfmac%S-sdio.txt", base);
        status = BwlLoadFile(Ctx, name, BWL_NVRAM_MAX_BYTES, &nvMem, &nvTxt, &nvTxtLen);
    }
    if (NT_SUCCESS(status)) {
        (VOID)RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"brcmfmac%S-sdio.clm_blob", base);
        status = BwlLoadFile(Ctx, name, BWL_CLM_MAX_BYTES, &clmMem, &clm, &clmLen);
    }
    BwlPutULong(Ctx, L"FileStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        BwlPutWString(Ctx, L"FileMissing", name);
        d->Step = "firmware files";
        Fail(Ctx, BWL_E_FORMAT);
        goto out;
    }
    BwlPutULong(Ctx, L"FwFileSize", fwLen);
    BwlPutULong(Ctx, L"NvramFileSize", nvTxtLen);
    BwlPutULong(Ctx, L"ClmFileSize", clmLen);
    if (BwlFwImageVersion(fw, fwLen, ver, sizeof(ver)) == 0) {
        BwlPutString(Ctx, L"FwFileVersion", ver);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Ctx->Device;
    status = WdfMemoryCreate(&attr, PagedPool, BWL_POOL_TAG, nvTxtLen + BWL_NVRAM_SLACK, &convMem, (PVOID *)&conv);
    if (!NT_SUCCESS(status)) {
        d->Step = "memory";
        Fail(Ctx, BWL_E_SPACE);
        goto out;
    }
    n = BwlNvramConvert(nvTxt, nvTxtLen, conv, nvTxtLen + BWL_NVRAM_SLACK);
    if (n < 0) {
        d->Step = "NVRAM text";
        Fail(Ctx, n);
        goto out;
    }
    BwlPutULong(Ctx, L"NvramLength", (ULONG)n);

    BwlCrumb(Ctx, "download");
    r = BwlDownload(d, fw, fwLen, conv, (u32)n, Ctx->Verify ? 1 : 0);
    if (r) {
        Fail(Ctx, r);
        goto out;
    }
    BwlPutString(Ctx, L"FwState", "downloaded");

    BwlCrumb(Ctx, "start");
    r = BwlStart(d, 3000);
    if (r) {
        (VOID)BwlReadShared(d);
        RecordConsole(Ctx);
        Fail(Ctx, r);
        goto out;
    }
    BwlPutString(Ctx, L"FwState", "started");
    if (BwlReadShared(d) == 0) {
        BwlPutULong(Ctx, L"SharedAddr", d->SharedAddr);
        BwlPutULong(Ctx, L"ConsoleAddr", d->Console.Addr);
    }

    BwlCrumb(Ctx, "control: version");
    r = BwlGetInt(d, C_GET_VERSION, &v, &fws);
    if (r) {
        BwlPutULong(Ctx, L"FwCmdStatus", (ULONG)fws);
        RecordConsole(Ctx);
        Fail(Ctx, r);
        goto out;
    }
    BwlPutULong(Ctx, L"BcdcIoType", v);

    BwlCrumb(Ctx, "control: cur_etheraddr");
    if (BwlGetVar(d, "cur_etheraddr", mac, sizeof(mac), &outLen, &fws) == 0) {
        (VOID)RtlStringCbPrintfA(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        BwlPutString(Ctx, L"MacAddress", text);
    }

    BwlCrumb(Ctx, "control: clmload");
    r = BwlClmLoad(d, clm, clmLen, &chunks, &clmStatus, &fws);
    BwlPutULong(Ctx, L"ClmChunks", chunks);
    BwlPutULong(Ctx, L"ClmStatus", clmStatus);
    BwlPutULong(Ctx, L"ClmResult", (ULONG)r);

    BwlCrumb(Ctx, "control: ver");
    RtlZeroMemory(text, sizeof(text));
    r = BwlGetVar(d, "ver", (u8 *)text, sizeof(text) - 1, &outLen, &fws);
    if (r) {
        RecordConsole(Ctx);
        Fail(Ctx, r);
        goto out;
    }
    text[sizeof(text) - 1] = 0;
    BwlPutString(Ctx, L"FwVersion", text);
    if (BwlVerString(text, BwlStrLen(text), ver, sizeof(ver)) == 0) {
        BwlPutString(Ctx, L"FwVersionNumber", ver);
    }

    (VOID)BwlPollOnce(d);
    RecordConsole(Ctx);
    PutCounters(Ctx);
    BwlPutString(Ctx, L"FwState", "running");
    BwlCrumb(Ctx, "firmware running");

out:
    if (convMem) WdfObjectDelete(convMem);
    if (clmMem) WdfObjectDelete(clmMem);
    if (nvMem) WdfObjectDelete(nvMem);
    if (fwMem) WdfObjectDelete(fwMem);
}

static VOID
BwlEvtBringUp(_In_ WDFWORKITEM WorkItem)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));

    /* 1 ms timer resolution while polling; restored when done. */
    (VOID)ExSetTimerResolution(10000, TRUE);
    BringUp(ctx);
    (VOID)ExSetTimerResolution(0, FALSE);
}

/* Stop the bring-up; then, with I/O allowed again, halt the chip's CPU and turn F1/F2 off. */
static VOID
StopBringUp(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    if (InterlockedCompareExchange(&Ctx->Stopping, 1, 0) != 0) {
        return;
    }
    if (Ctx->BringUp != NULL) {
        WdfWorkItemFlush(Ctx->BringUp);
    }
    if (Ctx->Bwl.NumCores != 0) {
        InterlockedExchange(&Ctx->Stopping, 2);
        (VOID)BwlChipSetPassive(&Ctx->Bwl);
        (VOID)BwlFuncEnable(&Ctx->Bwl, 2, 0, 0);
        (VOID)BwlFuncEnable(&Ctx->Bwl, 1, 0, 0);
        InterlockedExchange(&Ctx->Stopping, 1);
        BwlCrumb(Ctx, "stopped: CPU halted, F1/F2 off");
    }
}

/* ------------------------------------------------------------ PnP */

static NTSTATUS
BwlEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    InterlockedExchange(&ctx->Stopping, 0);
    ctx->Stage = GetULong(ctx, L"Stage", BWL_STAGE_OPEN);
    ctx->MaxBlocks = GetULong(ctx, L"MaxBlocks", 32);
    ctx->F2BlockMode = GetULong(ctx, L"F2BlockMode", 0);
    ctx->Verify = GetULong(ctx, L"Verify", 1);
    BwlPutULong(ctx, L"StageSeen", ctx->Stage);

    status = BwlSdOpen(ctx);
    BwlPutULong(ctx, L"SdOpenStatus", (ULONG)status);
    BwlCrumb(ctx, NT_SUCCESS(status) ? "SDBUS open" : "SDBUS open failed");
    /* The device still starts, so that the breadcrumbs can be read. */
    return STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    StopBringUp(ctx);
    BwlSdClose(ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    if (ctx->SdOpen && ctx->BringUp != NULL) {
        WdfWorkItemEnqueue(ctx->BringUp);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtSelfManagedIoSuspend(_In_ WDFDEVICE Device)
{
    StopBringUp(BwlGetContext(Device));
    return STATUS_SUCCESS;
}

static VOID
BwlEvtSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    StopBringUp(BwlGetContext(Device));
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

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BwlEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = BwlEvtReleaseHardware;
    pnp.EvtDeviceSelfManagedIoInit = BwlEvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoSuspend = BwlEvtSelfManagedIoSuspend;
    pnp.EvtDeviceSelfManagedIoCleanup = BwlEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BWL_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = BwlGetContext(device);
    ctx->Device = device;

    /* Every CMD53 goes through this buffer: non-paged, for the MDLs. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfMemoryCreate(&attr, NonPagedPoolNx, BWL_POOL_TAG, BWL_BOUNCE_MIN, &ctx->BounceMemory,
                             (PVOID *)&ctx->Bounce);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&wic, BwlEvtBringUp);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    return WdfWorkItemCreate(&wic, &attr, &ctx->BringUp);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, BwlEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
