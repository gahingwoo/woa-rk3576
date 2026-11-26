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
static KSTART_ROUTINE BwlWorkerMain;

static VOID EarlyCrumb(_In_ PCSTR Step);
static VOID EarlyCrumbF(_In_ PCSTR Fmt, _In_ ULONG A, _In_ ULONG B);
/* DEVPKEY_Device_InstanceId */
static const DEVPROPKEY BwlInstanceIdKey = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256
};

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

ULONG
BwlGetULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default)
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

ULONG
BwlGetAscii(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _Out_writes_(Cap) PCHAR Out, _In_ ULONG Cap)
{
    DECLARE_UNICODE_STRING_SIZE(v, 128);
    UNICODE_STRING n;
    WDFKEY key;
    ULONG i = 0;

    if (Cap == 0) {
        return 0;
    }
    Out[0] = 0;
    if (!NT_SUCCESS(OpenKey(Ctx, KEY_READ, &key))) {
        return 0;
    }
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &n, NULL, &v))) {
        for (i = 0; i + 1 < Cap && i < v.Length / sizeof(WCHAR) && v.Buffer[i]; i++) {
            Out[i] = (CHAR)(v.Buffer[i] >= 0x20 && v.Buffer[i] < 0x7F ? v.Buffer[i] : '?');
        }
        Out[i] = 0;
    }
    WdfRegistryClose(key);
    return i;
}

VOID
BwlDelayMs(_In_ ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -10000LL * (LONGLONG)Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
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

#define PutCounters BwlPutCounters
VOID
BwlPutCounters(_In_ PBWL_DEVICE_CONTEXT Ctx)
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

/* TRUE when the firmware runs (Stage 2 or more) and answered. */
static BOOLEAN
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
    BOOLEAN ok = FALSE;
    bwl_dev *d = &Ctx->Bwl;

    BwlCrumb(Ctx, "bring-up start");
    BwlPutString(Ctx, L"FwState", "starting");
    RecordBus(Ctx);
    if (Ctx->Stage == BWL_STAGE_OPEN) {
        BwlPutString(Ctx, L"FwState", "stage 0: SDBUS open, card not touched");
        BwlCrumb(Ctx, "stage 0 done");
        return FALSE;
    }
    if (Ctx->Function != 1) {
        /* The F2 PDO, if sdbus makes one: F1's driver instance does all the work. */
        BwlPutString(Ctx, L"FwState", "idle: not function 1");
        BwlCrumb(Ctx, "not function 1");
        return FALSE;
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
        return FALSE;
    }

    BwlCrumb(Ctx, "attach");
    r = BwlAttach(d);
    RecordChip(Ctx);
    if (r) {
        Fail(Ctx, r);
        return FALSE;
    }
    base = BwlChipFwName(d->Chip, d->ChipRev);
    BwlPutString(Ctx, L"FwState", "identified");
    PutCounters(Ctx);
    if (Ctx->Stage < BWL_STAGE_FIRMWARE) {
        BwlCrumb(Ctx, "stage 1 done");
        return FALSE;
    }
    if (base == NULL) {
        d->Step = "no firmware name for this chip";
        Fail(Ctx, BWL_E_NOCHIP);
        return FALSE;
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
        (VOID)RtlStringCbCopyA(Ctx->FwVer, sizeof(Ctx->FwVer), ver);
    }

    (VOID)BwlPollOnce(d);
    RecordConsole(Ctx);
    PutCounters(Ctx);
    BwlPutString(Ctx, L"FwState", "running");
    BwlCrumb(Ctx, "firmware running");
    ok = TRUE;

out:
    if (convMem) WdfObjectDelete(convMem);
    if (clmMem) WdfObjectDelete(clmMem);
    if (nvMem) WdfObjectDelete(nvMem);
    if (fwMem) WdfObjectDelete(fwMem);
    return ok;
}

static VOID
BwlEvtBringUp(_In_ WDFWORKITEM WorkItem)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));

    /* 1 ms timer resolution while polling; restored when done. */
    (VOID)ExSetTimerResolution(10000, TRUE);
    (VOID)BringUp(ctx);
    (VOID)ExSetTimerResolution(0, FALSE);
}

/* ------------------------------------------------------------ stage 3 */

/* The firmware and the station, in PrepareHardware; TRUE if both are up. */
static BOOLEAN
NetworkUp(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    bwl_dev *d = &Ctx->Bwl;
    CHAR country[4], text[32];
    BOOLEAN ok;
    int r;

    (VOID)ExSetTimerResolution(10000, TRUE);
    ok = BringUp(Ctx);
    if (ok) {
        d->Wlan.Net = &BwlNetOps;
        (VOID)BwlGetAscii(Ctx, L"Country", country, sizeof(country));
        BwlCrumb(Ctx, "wlan: set-up");
        r = BwlWlanInit(d, country[0] ? country : NULL);
        BwlPutULong(Ctx, L"WlanInitResult", (ULONG)r);
        BwlPutULong(Ctx, L"WlanFwStatus", d->Wlan.LastFwStatus);
        BwlPutULong(Ctx, L"IoType", d->Wlan.IoType);
        BwlPutULong(Ctx, L"Has5GHz", d->Wlan.Has5g);
        BwlPutULong(Ctx, L"FwSupCapable", d->Wlan.FwSupCapable);
        (VOID)RtlStringCbPrintfA(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", d->Wlan.Mac[0],
                                 d->Wlan.Mac[1], d->Wlan.Mac[2], d->Wlan.Mac[3], d->Wlan.Mac[4], d->Wlan.Mac[5]);
        BwlPutString(Ctx, L"MacAddress", text);
        if (r) {
            Fail(Ctx, r);
            ok = FALSE;
        } else {
            BwlPutString(Ctx, L"FwState", "station up");
            BwlCrumb(Ctx, "wlan: up");
        }
    }
    (VOID)ExSetTimerResolution(0, FALSE);
    PutCounters(Ctx);
    return ok;
}

static VOID
BwlWorkerMain(_In_ PVOID Context)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)Context;

    /* 1 ms timer resolution while the card is polled */
    (VOID)ExSetTimerResolution(10000, TRUE);
    BwlWifiRun(ctx);
    (VOID)ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS
StartWorker(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    HANDLE h;
    NTSTATUS status;

    if (!Ctx->WifiMode || Ctx->Worker != NULL) {
        return STATUS_SUCCESS;
    }
    InterlockedExchange(&Ctx->WorkerStop, 0);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, BwlWorkerMain, Ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Ctx->Worker,
                                       NULL);
    ZwClose(h);
    return status;
}

static VOID
StopWorker(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Worker == NULL) {
        return;
    }
    InterlockedExchange(&Ctx->WorkerStop, 1);
    KeSetEvent(&Ctx->WakeEvent, IO_NO_INCREMENT, FALSE);
    (VOID)KeWaitForSingleObject(Ctx->Worker, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Ctx->Worker);
    Ctx->Worker = NULL;
}

/* Stop the bring-up; then, with I/O allowed again, halt the chip's CPU and turn F1/F2 off. */
static VOID
StopBringUp(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    StopWorker(Ctx);
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
    BwlCrumb(ctx, NT_SUCCESS(status) ? "SDBUS open" : "SDBUS open failed");
    if (!ctx->WifiMode) {
        /* The device still starts, so that the breadcrumbs can be read. */
        return STATUS_SUCCESS;
    }

    /* Stage 3: the firmware now, then the capabilities; WiFiCx asks for
     * the adapter after this returns (EvtWifiDeviceCreateAdapter). */
    ctx->FwReady = FALSE;
    if (NT_SUCCESS(status)) {
        ctx->FwReady = NetworkUp(ctx);
        if (ctx->Function != 1) {
            BwlCrumb(ctx, "stage 3: this instance is not function 1 after all");
        }
    }
    status = BwlWifiReportCapabilities(ctx);
    BwlPutULong(ctx, L"WifiCapsStatus", (ULONG)status);
    return status;
}

static NTSTATUS
BwlEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    StopWorker(ctx);
    BwlWifiReleaseHardware(ctx);
    StopBringUp(ctx);
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
        /* the worker runs the WDI requests even without firmware, to refuse them */
        return StartWorker(ctx);
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
    return ctx->WifiMode ? StartWorker(ctx) : STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtSelfManagedIoSuspend(_In_ WDFDEVICE Device)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);

    if (ctx->WifiMode) {
        /* stop polling; the chip keeps its firmware for the restart */
        StopWorker(ctx);
        return STATUS_SUCCESS;
    }
    StopBringUp(ctx);
    return STATUS_SUCCESS;
}

static VOID
BwlEvtSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    StopBringUp(BwlGetContext(Device));
}

static VOID
BwlEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext((WDFDEVICE)Object);

    StopWorker(ctx);
    BwlWifiCleanup(ctx);
    BwlNetFree(ctx);
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
#ifndef NETADAPTER_VERSION_MINOR
#define NETADAPTER_VERSION_MAJOR 0
#define NETADAPTER_VERSION_MINOR 0
#endif
#ifndef WIFI_VERSION_MINOR
#define WIFI_VERSION_MAJOR 0
#define WIFI_VERSION_MINOR 0
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
             L"entered; binds NetAdapterCx " BWL_WSTR(NETADAPTER_VERSION_MAJOR) L"." BWL_WSTR(NETADAPTER_VERSION_MINOR)
             L", WiFiCx " BWL_WSTR(WIFI_VERSION_MAJOR) L"." BWL_WSTR(WIFI_VERSION_MINOR) L", KMDF 1.33",
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
