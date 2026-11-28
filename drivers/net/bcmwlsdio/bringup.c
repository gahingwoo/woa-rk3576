/*
 * bringup.c - what both front ends of the Broadcom driver share around the
 * portable core: the Device Parameters records and breadcrumbs, the
 * bring-up (SDBUS, chip, firmware files, download, start, CLM), the
 * station set-up, the worker thread, and stopping the chip.
 *
 * Front ends: driver.c (KMDF function driver: the Stage 0-2 bring-up and
 * the WiFiCx adapter of Windows 11) and wdi.cpp (NDIS WDI miniport of
 * Windows 10). Everything here runs at PASSIVE_LEVEL.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"
#include "bwl_regs.h"

static KSTART_ROUTINE BwlWorkerMain;

/* ------------------------------------------------------------ registry */

/*
 * The device's Device Parameters key. The KMDF function driver opens it
 * through its device; the WDI miniport (wdi.cpp), whose miniport-mode
 * device may not open PnP keys, by the absolute path it read once from
 * IoOpenDeviceRegistryKey (DevKeyPath).
 */
NTSTATUS
BwlOpenDeviceKey(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    if (Ctx->DevKeyPath.Length != 0) {
        return WdfRegistryOpenKey(NULL, &Ctx->DevKeyPath, Access, WDF_NO_OBJECT_ATTRIBUTES, Key);
    }
    return WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, Access,
                                    WDF_NO_OBJECT_ATTRIBUTES, Key);
}

VOID
BwlPutULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING n;

    if (NT_SUCCESS(BwlOpenDeviceKey(Ctx, KEY_WRITE, &key))) {
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

    if (NT_SUCCESS(BwlOpenDeviceKey(Ctx, KEY_WRITE, &key))) {
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

    buf = (PWCHAR)BwlBufAlloc(POOL_FLAG_PAGED, (PUT_MAX + 1) * sizeof(WCHAR), "registry text");
    if (buf == NULL) {
        return;
    }
    for (i = 0; i < PUT_MAX && Value[i]; i++) {
        CHAR c = Value[i];

        buf[i] = (WCHAR)((c == '\n' || (c >= 0x20 && c < 0x7F)) ? c : '?');
    }
    buf[i] = 0;
    BwlPutWString(Ctx, Name, buf);
    BwlBufFree(NULL, buf, "registry text");     /* NULL: pool.c's report writes through here */
}

ULONG
BwlGetULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY key;
    UNICODE_STRING n;
    ULONG v = Default;

    if (NT_SUCCESS(BwlOpenDeviceKey(Ctx, KEY_READ, &key))) {
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
    if (!NT_SUCCESS(BwlOpenDeviceKey(Ctx, KEY_READ, &key))) {
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

    /* The debugger line first: it must not depend on the registry being writable. */
    BWL_LOG("FN%u step: %s", Ctx->Function, Step);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    for (i = 0; i + 1 < RTL_NUMBER_OF(step) && Step[i]; i++) {
        step[i] = (WCHAR)(UCHAR)Step[i];
    }
    step[i] = 0;
    if (!NT_SUCCESS(BwlOpenDeviceKey(Ctx, KEY_READ | KEY_WRITE, &key))) {
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
    BwlPutULong(Ctx, L"Cmd53OutsideBounce", Ctx->Cmd53OutsideBounce);
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
    PCHAR buf = (PCHAR)BwlBufAlloc(POOL_FLAG_PAGED, PUT_MAX + 1, "console");

    if (buf == NULL) {
        return;
    }
    if (BwlConsoleRead(&Ctx->Bwl, buf, PUT_MAX + 1) > 0) {
        BwlPutString(Ctx, L"FwConsole", buf);
    }
    BwlBufFree(Ctx, buf, "console");
}

/* TRUE when the firmware runs (Stage 2 or more) and answered. */
BOOLEAN
BwlBringUp(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PUCHAR fw = NULL, nvTxt = NULL, clm = NULL, conv = NULL;
    ULONG fwLen = 0, nvTxtLen = 0, clmLen = 0;
    WCHAR name[64];
    CHAR text[256], ver[64];
    const char *base;
    u8 mac[6];
    u32 v, chunks = 0, clmStatus = 0, outLen = 0;
    int fws = 0, r, n;
    USHORT blk = 64;
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
    status = BwlLoadFile(Ctx, name, BWL_FW_MAX_BYTES, &fw, &fwLen);
    if (NT_SUCCESS(status)) {
        (VOID)RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"brcmfmac%S-sdio.txt", base);
        status = BwlLoadFile(Ctx, name, BWL_NVRAM_MAX_BYTES, &nvTxt, &nvTxtLen);
    }
    if (NT_SUCCESS(status)) {
        (VOID)RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"brcmfmac%S-sdio.clm_blob", base);
        status = BwlLoadFile(Ctx, name, BWL_CLM_MAX_BYTES, &clm, &clmLen);
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

    /* BwlNvramConvert writes up to nvTxtLen + BWL_NVRAM_SLACK bytes, never more (bwl.h). */
    conv = (PUCHAR)BwlBufAlloc(POOL_FLAG_PAGED, nvTxtLen + BWL_NVRAM_SLACK, "nvram");
    if (conv == NULL) {
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
    BwlBufFree(Ctx, conv, "bring-up: nvram");
    BwlBufFree(Ctx, clm, "bring-up: clm_blob");
    BwlBufFree(Ctx, nvTxt, "bring-up: nvram text");
    BwlBufFree(Ctx, fw, "bring-up: firmware");
    /* The bounce buffer stays until the device goes; check it now, while the breadcrumbs can say so. */
    BwlPutULong(Ctx, L"BounceGuardOk", BwlBufVerify(Ctx, Ctx->Bounce, "bring-up: bounce") ? 1 : 0);
    return ok;
}

/* ------------------------------------------------------------ stage 3 */

/* The firmware and the station, in PrepareHardware; TRUE if both are up. */
BOOLEAN
BwlNetworkUp(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    bwl_dev *d = &Ctx->Bwl;
    CHAR country[4], text[32];
    BOOLEAN ok;
    int r;

    (VOID)ExSetTimerResolution(10000, TRUE);
    ok = BwlBringUp(Ctx);
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

/* ntifs.h, not in the wdm.h this driver builds with; exported by ntoskrnl */
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(_In_ HANDLE Handle, _In_ BOOLEAN Alertable,
                                              _In_opt_ PLARGE_INTEGER Timeout);

NTSTATUS
BwlStartWorker(_In_ PBWL_DEVICE_CONTEXT Ctx)
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
    if (!NT_SUCCESS(status)) {
        /* the thread runs, but StopWorker could not wait for it: stop it now */
        Ctx->Worker = NULL;
        InterlockedExchange(&Ctx->WorkerStop, 1);
        KeSetEvent(&Ctx->WakeEvent, IO_NO_INCREMENT, FALSE);
        (VOID)ZwWaitForSingleObject(h, FALSE, NULL);
    }
    ZwClose(h);
    return status;
}

VOID
BwlStopWorker(_In_ PBWL_DEVICE_CONTEXT Ctx)
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
VOID
BwlStopChip(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    BwlStopWorker(Ctx);
    if (InterlockedCompareExchange(&Ctx->Stopping, 1, 0) != 0) {
        return;
    }
    if (Ctx->BringUp != NULL) {
        WdfWorkItemFlush(Ctx->BringUp);
    }
    if (Ctx->Gone) {
        BWL_LOG("FN%u teardown: surprise-removed, chip left as it is", Ctx->Function);
        return;
    }
    if (Ctx->Bwl.NumCores != 0) {
        InterlockedExchange(&Ctx->Stopping, 2);
        (VOID)BwlChipSetPassive(&Ctx->Bwl);
        (VOID)BwlFuncEnable(&Ctx->Bwl, 2, 0, 0);
        (VOID)BwlFuncEnable(&Ctx->Bwl, 1, 0, 0);
        InterlockedExchange(&Ctx->Stopping, 1);
        BwlCrumb(Ctx, "stopped: CPU halted, F1/F2 off");
    }
    if (Ctx->Bounce != NULL && !BwlBufVerify(Ctx, Ctx->Bounce, "teardown: bounce")) {
        BwlCrumb(Ctx, "teardown: bounce buffer guard broken");
    }
}
