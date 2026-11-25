/*
 * driver.c - aic8800usb: KMDF driver for the AIC8800D80 USB Wi-Fi, bring-up
 * stages (see aic8800usb.h and docs/dev/aic8800.md).
 *
 * PnP: PrepareHardware selects the configuration and sets up the
 * continuous readers, D0Entry starts them, SelfManagedIoInit starts the
 * worker thread that runs the stages (they take seconds, and stage 3 keeps
 * the link for as long as the device is up), SelfManagedIoSuspend stops it,
 * D0Exit stops the readers.
 *
 * The loader instance (PID_8D80) downloads the firmware and records that in
 * a volatile key, so that the runtime instance (PID_8D81) can tell firmware
 * it started this boot from firmware left running across a warm reboot; for
 * the latter it asks the chip to reboot into its loader, as AICSemi's Linux
 * driver does, and the download runs again.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic8800usb.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD AicEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE AicEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE AicEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY AicEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT AicEvtD0Exit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT AicEvtSelfManagedIoInit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND AicEvtSelfManagedIoSuspend;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART AicEvtSelfManagedIoRestart;
static KSTART_ROUTINE AicWorkerMain;

static BOOLEAN g_LoaderService;     /* this image runs as aic8800ldr */

/* ------------------------------------------------------------ registry */

/*
 * Two keys. Settings (Stage, Ssid, Passphrase, Pmk, timeouts, FirmwareDir)
 * are read from the service's Parameters key first, so one value covers
 * both device instances (the loader and the runtime are different devices
 * with different Device Parameters), and from Device Parameters second.
 * Everything the driver records goes to Device Parameters.
 */
static NTSTATUS
OpenKey(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    return WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, Access, WDF_NO_OBJECT_ATTRIBUTES, Key);
}

/* Services\aic8800usb\Parameters by its absolute path: the loader
 * service (aic8800ldr) reads the same settings. */
NTSTATUS
AicOpenParams(_In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    UNICODE_STRING path;

    RtlInitUnicodeString(&path, AIC_PARAMS_PATH);
    return WdfRegistryOpenKey(NULL, &path, Access, WDF_NO_OBJECT_ATTRIBUTES, Key);
}

#define OpenParams AicOpenParams

/* The settings key that has Name: Parameters, else Device Parameters. */
static NTSTATUS
OpenSetting(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    UNICODE_STRING n;
    ULONG len, type;

    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(OpenParams(Access, Key))) {
        NTSTATUS st = WdfRegistryQueryValue(*Key, &n, 0, NULL, &len, &type);

        if (NT_SUCCESS(st) || st == STATUS_BUFFER_OVERFLOW) {
            return STATUS_SUCCESS;
        }
        WdfRegistryClose(*Key);
    }
    return OpenKey(Ctx, Access, Key);
}

VOID
AicPutULong(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value)
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
AicPutBinary(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_reads_bytes_(Len) const VOID *Data, _In_ ULONG Len)
{
    WDFKEY key;
    UNICODE_STRING n;

    if (NT_SUCCESS(OpenKey(Ctx, KEY_WRITE, &key))) {
        RtlInitUnicodeString(&n, Name);
        (VOID)WdfRegistryAssignValue(key, &n, REG_BINARY, Len, (PVOID)Data);
        WdfRegistryClose(key);
    }
}

static VOID
PutWString(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCWSTR Value)
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

/* ASCII to a registry string; control characters other than newline and
 * tab, and bytes above 0x7E, become '?'. */
#define PUT_MAX 4096
VOID
AicPutString(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value)
{
    PWCHAR buf;
    ULONG i;

    buf = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (PUT_MAX + 1) * sizeof(WCHAR), AIC_POOL_TAG);
    if (buf == NULL) {
        return;
    }
    for (i = 0; i < PUT_MAX && Value[i]; i++) {
        CHAR c = Value[i];

        buf[i] = (WCHAR)((c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) ? c : '?');
    }
    buf[i] = 0;
    PutWString(Ctx, Name, buf);
    ExFreePoolWithTag(buf, AIC_POOL_TAG);
}

ULONG
AicGetULong(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY key;
    UNICODE_STRING n;
    ULONG v = Default;

    if (NT_SUCCESS(OpenSetting(Ctx, Name, KEY_READ, &key))) {
        RtlInitUnicodeString(&n, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v))) {
            v = Default;
        }
        WdfRegistryClose(key);
    }
    return v;
}

/* UTF-16 to UTF-8; 0 if it does not fit. */
static ULONG
Utf16ToUtf8(_In_reads_(N) const WCHAR *S, _In_ ULONG N, _Out_writes_bytes_(Max) PCHAR Out, _In_ ULONG Max)
{
    ULONG i, o = 0;

    for (i = 0; i < N; i++) {
        ULONG c = S[i];

        if (c >= 0xD800 && c < 0xDC00 && i + 1 < N && S[i + 1] >= 0xDC00 && S[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (S[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            if (o + 1 > Max) return 0;
            Out[o++] = (CHAR)c;
        } else if (c < 0x800) {
            if (o + 2 > Max) return 0;
            Out[o++] = (CHAR)(0xC0 | (c >> 6));
            Out[o++] = (CHAR)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            if (o + 3 > Max) return 0;
            Out[o++] = (CHAR)(0xE0 | (c >> 12));
            Out[o++] = (CHAR)(0x80 | ((c >> 6) & 0x3F));
            Out[o++] = (CHAR)(0x80 | (c & 0x3F));
        } else {
            if (o + 4 > Max) return 0;
            Out[o++] = (CHAR)(0xF0 | (c >> 18));
            Out[o++] = (CHAR)(0x80 | ((c >> 12) & 0x3F));
            Out[o++] = (CHAR)(0x80 | ((c >> 6) & 0x3F));
            Out[o++] = (CHAR)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

/* A REG_SZ setting as UTF-8 (SSIDs are bytes; the registry holds text).
 * Returns the byte count, 0 if absent. */
static ULONG
GetUtf8(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _Out_writes_bytes_(Max) PCHAR Out, _In_ ULONG Max)
{
    DECLARE_UNICODE_STRING_SIZE(v, 128);
    UNICODE_STRING n;
    WDFKEY key;
    ULONG got = 0;

    Out[0] = 0;
    if (!NT_SUCCESS(OpenSetting(Ctx, Name, KEY_READ, &key))) {
        return 0;
    }
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &n, NULL, &v)) && v.Length) {
        got = Utf16ToUtf8(v.Buffer, v.Length / sizeof(WCHAR), Out, Max - 1);
    }
    Out[got] = 0;
    RtlSecureZeroMemory(v.Buffer, v.MaximumLength);
    WdfRegistryClose(key);
    return got;
}

static BOOLEAN
GetBinary(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _Out_writes_bytes_(Len) PVOID Out, _In_ ULONG Len)
{
    UNICODE_STRING n;
    WDFKEY key;
    ULONG got = 0, type = 0;
    NTSTATUS status;

    if (!NT_SUCCESS(OpenSetting(Ctx, Name, KEY_READ, &key))) {
        return FALSE;
    }
    RtlInitUnicodeString(&n, Name);
    status = WdfRegistryQueryValue(key, &n, Len, Out, &got, &type);
    WdfRegistryClose(key);
    return NT_SUCCESS(status) && type == REG_BINARY && got == Len;
}

/* Remove a setting from wherever it is. */
static VOID
DeleteValue(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name)
{
    UNICODE_STRING n;
    WDFKEY key;

    if (NT_SUCCESS(OpenSetting(Ctx, Name, KEY_READ | KEY_WRITE, &key))) {
        RtlInitUnicodeString(&n, Name);
        (VOID)WdfRegistryRemoveValue(key, &n);
        (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
        WdfRegistryClose(key);
    }
}

/*
 * The step the bring-up is in, flushed to disk at once, so that a hang
 * leaves it readable after a reset: LastStep, PrevStep, StepSeq.
 */
VOID
AicCrumb(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCSTR Step)
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
    AIC_LOG("step: %s", Step);
}

/* ------------------------------------------- volatile driver state
 *
 * Services\aic8800usb\Parameters\Boot, created REG_OPTION_VOLATILE: gone
 * at the next boot, which is the point. FirmwareLoaded: the loader instance
 * started the firmware since boot. Reboots: how many times a runtime
 * instance sent the chip back to its loader since then.
 */
static NTSTATUS
OpenBootKey(_Out_ WDFKEY *Key)
{
    WDFKEY params;
    UNICODE_STRING n;
    NTSTATUS status;

    status = AicOpenParams(KEY_READ | KEY_WRITE, &params);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&n, L"Boot");
    status = WdfRegistryCreateKey(params, &n, KEY_READ | KEY_WRITE, REG_OPTION_VOLATILE, NULL,
                                  WDF_NO_OBJECT_ATTRIBUTES, Key);
    WdfRegistryClose(params);
    return status;
}

static ULONG
GetBoot(_In_ PCWSTR Name)
{
    WDFKEY key;
    UNICODE_STRING n;
    ULONG v = 0;

    if (NT_SUCCESS(OpenBootKey(&key))) {
        RtlInitUnicodeString(&n, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v))) {
            v = 0;
        }
        WdfRegistryClose(key);
    }
    return v;
}

static VOID
PutBoot(_In_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING n;

    if (NT_SUCCESS(OpenBootKey(&key))) {
        RtlInitUnicodeString(&n, Name);
        (VOID)WdfRegistryAssignULong(key, &n, Value);
        WdfRegistryClose(key);
    }
}

/* ------------------------------------------------------------ records */

static VOID
PutStats(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    aic_dev *d = &Ctx->Aic;

    AicPutULong(Ctx, L"TxMsg", d->Stats.TxMsg);
    AicPutULong(Ctx, L"RxMsg", d->Stats.RxMsg);
    AicPutULong(Ctx, L"RxInd", d->Stats.RxInd);
    AicPutULong(Ctx, L"RxData", d->Stats.RxData);
    AicPutULong(Ctx, L"RxDataDropped", d->Stats.RxDataDropped);
    AicPutULong(Ctx, L"RxEapol", d->Stats.RxEapol);
    AicPutULong(Ctx, L"TxData", d->Stats.TxData);
    AicPutULong(Ctx, L"RxPrint", d->Stats.RxPrint);
    AicPutULong(Ctx, L"RxErrors", d->Stats.RxErrors);
    AicPutULong(Ctx, L"RxUnknown", d->Stats.RxUnknown);
    AicPutULong(Ctx, L"LastUnknownMsg", d->Stats.LastUnknownId);
    AicPutULong(Ctx, L"FwPanic", d->Stats.FwPanic);
    AicPutULong(Ctx, L"MicFailures", d->Stats.MicFailures);
    AicPutULong(Ctx, L"UsbSends", Ctx->Sends);
    AicPutULong(Ctx, L"UsbSendErrors", Ctx->SendErrors);
    AicPutULong(Ctx, L"UsbLastSendStatus", (ULONG)Ctx->LastSendStatus);
    AicPutULong(Ctx, L"UsbRingDrops", Ctx->RingDrops);
    AicPutULong(Ctx, L"UsbReaderErrors", Ctx->ReaderErrors);
    AicPutULong(Ctx, L"LinkState", (ULONG)d->Link);
    if (Ctx->Adapter) {
        AicPutULong(Ctx, L"NetTxFrames", Ctx->TxFrames);
        AicPutULong(Ctx, L"NetTxDrops", Ctx->TxDrops);
        AicPutULong(Ctx, L"NetTxErrors", Ctx->TxErrors);
        AicPutULong(Ctx, L"NetRxFrames", Ctx->RxFrames);
        AicPutULong(Ctx, L"NetRxDrops", Ctx->RxDrops);
        AicPutULong(Ctx, L"NetLinkReported", Ctx->LinkReported);
    }
    if (d->LastError[0]) {
        AicPutString(Ctx, L"LastError", d->LastError);
    }
}

static VOID
PutChip(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    AicPutULong(Ctx, L"ChipWord", Ctx->Aic.ChipWord);
    AicPutULong(Ctx, L"ChipRev", Ctx->Aic.ChipRev);
    AicPutULong(Ctx, L"ChipSubRev", Ctx->Aic.ChipSubRev);
    AicPutULong(Ctx, L"ChipIsH", Ctx->Aic.ChipIsH);
    AicPutULong(Ctx, L"ChipMcu", Ctx->Aic.ChipMcu);
}

static VOID
MacText(_In_reads_(6) const UCHAR *M, _Out_writes_(18) PCHAR Out)
{
    (VOID)RtlStringCchPrintfA(Out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", M[0], M[1], M[2], M[3], M[4], M[5]);
}

static VOID
PutScan(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    static const char *sec[] = { "open", "WEP", "WPA", "WPA2-PSK", "RSN-other" };
    aic_dev *d = &Ctx->Aic;
    PCHAR text;
    size_t left = PUT_MAX;
    PCHAR p;
    ULONG i;

    text = (PCHAR)ExAllocatePool2(POOL_FLAG_PAGED, PUT_MAX + 1, AIC_POOL_TAG);
    if (text == NULL) {
        return;
    }
    p = text;
    text[0] = 0;
    for (i = 0; i < d->BssCount && left > 100; i++) {
        const aic_bss *b = &d->Bss[i];
        CHAR ssid[33], mac[18];

        RtlCopyMemory(ssid, b->Ssid, b->SsidLen);
        ssid[b->SsidLen] = 0;
        MacText(b->Bssid, mac);
        (VOID)RtlStringCchPrintfExA(p, left, &p, &left, 0, "%s\t%s\t%u\t%d\t%s\n", ssid, mac, (unsigned)b->Freq,
                                    (int)b->Rssi, b->Security < 5 ? sec[b->Security] : "?");
    }
    AicPutString(Ctx, L"ScanResults", text);
    AicPutULong(Ctx, L"ScanCount", d->BssCount);
    AicPutULong(Ctx, L"ScanStatus", d->ScanStatus);
    ExFreePoolWithTag(text, AIC_POOL_TAG);
}

/* ------------------------------------------------------------ stages */

static NTSTATUS
LoaderStage(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    static const PCWSTR ext[AIC_MAX_EXT_PATCH] = {
        L"fw_patch_8800d80_u02_ext0.bin", L"fw_patch_8800d80_u02_ext1.bin",
        L"fw_patch_8800d80_u02_ext2.bin", L"fw_patch_8800d80_u02_ext3.bin",
    };
    WDFMEMORY mem[4 + AIC_MAX_EXT_PATCH] = { 0 };
    aic_fw_files fw;
    aic_patch_info info;
    NTSTATUS status;
    ULONG i;
    int rc;

    AicCrumb(Ctx, "loader: identify");
    rc = aic_identify(&Ctx->Aic);
    PutChip(Ctx);
    if (rc != AIC_OK) {
        AicCrumb(Ctx, "loader: identify failed");
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    RtlZeroMemory(&fw, sizeof(fw));
    status = AicLoadFile(Ctx, L"fw_patch_table_8800d80_u02.bin", AIC_FW_MAX_BYTES, &mem[0], &fw.PatchTable);
    if (NT_SUCCESS(status)) {
        status = AicLoadFile(Ctx, L"fw_adid_8800d80_u02.bin", AIC_FW_MAX_BYTES, &mem[1], &fw.Adid);
    }
    if (NT_SUCCESS(status)) {
        status = AicLoadFile(Ctx, L"fw_patch_8800d80_u02.bin", AIC_FW_MAX_BYTES, &mem[2], &fw.Patch);
    }
    if (NT_SUCCESS(status)) {
        status = AicLoadFile(Ctx, Ctx->Aic.ChipIsH ? L"fmacfw_8800d80_h_u02.bin" : L"fmacfw_8800d80_u02.bin",
                             AIC_FW_MAX_BYTES, &mem[3], &fw.Fmac);
    }
    if (NT_SUCCESS(status) && aic_fw_parse_table(&fw.PatchTable, &info) == AIC_OK) {
        for (i = 0; i < info.ExtCount && NT_SUCCESS(status); i++) {
            if (info.ExtId[i] < AIC_MAX_EXT_PATCH && fw.Ext[info.ExtId[i]].Data == NULL) {
                status = AicLoadFile(Ctx, ext[info.ExtId[i]], AIC_FW_MAX_BYTES, &mem[4 + info.ExtId[i]],
                                     &fw.Ext[info.ExtId[i]]);
            }
        }
    }
    if (!NT_SUCCESS(status)) {
        AicPutULong(Ctx, L"FwFileStatus", (ULONG)status);
        AicCrumb(Ctx, "loader: firmware files missing (run install-firmware.ps1)");
        goto out;
    }

    AicCrumb(Ctx, "loader: download");
    rc = aic_fw_download(&Ctx->Aic, &fw, &info);
    AicPutULong(Ctx, L"PatchSections", info.Sections);
    AicPutULong(Ctx, L"PatchWrites", info.Writes);
    AicPutString(Ctx, L"PatchVersion", info.Version);
    AicPutULong(Ctx, L"FwVersionWord", Ctx->Aic.FwVersionWord);
    if (rc != AIC_OK) {
        AicPutString(Ctx, L"FwState", "download failed");
        AicCrumb(Ctx, "loader: download failed");
        status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto out;
    }
    PutBoot(L"FirmwareLoaded", 1);
    AicPutString(Ctx, L"FwState", "started");
    AicCrumb(Ctx, "loader: firmware started, waiting for PID_8D81");
out:
    for (i = 0; i < RTL_NUMBER_OF(mem); i++) {
        if (mem[i]) {
            WdfObjectDelete(mem[i]);
        }
    }
    return status;
}

static BOOLEAN
IsHex64(_In_reads_(64) const CHAR *P)
{
    ULONG i;

    for (i = 0; i < 64; i++) {
        CHAR c = P[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

static UCHAR
HexNibble(CHAR c)
{
    return (UCHAR)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
}

/* The PMK: from Pmk (REG_BINARY, 32), or from Passphrase, which is then
 * replaced by Pmk so that the passphrase does not stay in the registry. */
static BOOLEAN
GetPmk(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_reads_(SsidLen) const UCHAR *Ssid, _In_ ULONG SsidLen)
{
    CHAR pass[72];
    ULONG n;

    n = GetUtf8(Ctx, L"Passphrase", pass, sizeof(pass));
    if (n == 64 && IsHex64(pass)) {
        for (n = 0; n < 32; n++) {
            Ctx->Aic.Wpa.Pmk[n] = (UCHAR)((HexNibble(pass[2 * n]) << 4) | HexNibble(pass[2 * n + 1]));
        }
        n = 64;
    } else if (n >= 8 && n <= 63) {
        aic_wpa_pmk(pass, n, Ssid, SsidLen, Ctx->Aic.Wpa.Pmk);
    } else {
        n = 0;
    }
    RtlSecureZeroMemory(pass, sizeof(pass));
    if (n) {
        WDFKEY key;
        UNICODE_STRING name;

        /* next to the Ssid it belongs to: Parameters */
        if (NT_SUCCESS(OpenParams(KEY_WRITE, &key))) {
            RtlInitUnicodeString(&name, L"Pmk");
            (VOID)WdfRegistryAssignValue(key, &name, REG_BINARY, 32, Ctx->Aic.Wpa.Pmk);
            WdfRegistryClose(key);
        }
        DeleteValue(Ctx, L"Passphrase");
        Ctx->Aic.Wpa.HavePmk = 1;
        return TRUE;
    }
    if (GetBinary(Ctx, L"Pmk", Ctx->Aic.Wpa.Pmk, 32)) {
        Ctx->Aic.Wpa.HavePmk = 1;
        return TRUE;
    }
    return FALSE;
}

static VOID
SeedNonce(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    if (!NT_SUCCESS(BCryptGenRandom(NULL, Ctx->Aic.Wpa.Snonce, sizeof(Ctx->Aic.Wpa.Snonce),
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        LARGE_INTEGER t = KeQueryPerformanceCounter(NULL);

        RtlCopyMemory(Ctx->Aic.Wpa.Snonce, &t, sizeof(t));
        KeQuerySystemTimePrecise(&t);
        RtlCopyMemory(Ctx->Aic.Wpa.Snonce + 8, &t, sizeof(t));
    }
}

static int
ScanAndJoin(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ int Join)
{
    aic_dev *d = &Ctx->Aic;
    CHAR ssid[40], mac[18];
    ULONG ssidLen;
    int rc, idx;

    AicCrumb(Ctx, "runtime: scan");
    rc = aic_scan(d, AicGetULong(Ctx, L"ScanTimeoutMs", 8000));
    PutScan(Ctx);
    if (rc != AIC_OK || !Join) {
        return rc;
    }
    ssidLen = GetUtf8(Ctx, L"Ssid", ssid, sizeof(ssid));
    if (ssidLen == 0 || ssidLen > 32) {
        AicCrumb(Ctx, "runtime: no Ssid set");
        return AIC_E_STATE;
    }
    idx = aic_find_bss(d, (const UCHAR *)ssid, ssidLen);
    if (idx < 0) {
        AicCrumb(Ctx, "runtime: Ssid not in the scan");
        return AIC_E_NOTFOUND;
    }
    if (d->Bss[idx].Security == AIC_SEC_WPA2_PSK && !d->Wpa.HavePmk &&
        !GetPmk(Ctx, (const UCHAR *)ssid, ssidLen)) {
        AicCrumb(Ctx, "runtime: no Passphrase or Pmk");
        return AIC_E_STATE;
    }
    SeedNonce(Ctx);
    AicCrumb(Ctx, "runtime: connect");
    rc = aic_connect(d, idx, AicGetULong(Ctx, L"ConnectTimeoutMs", 15000));
    AicPutULong(Ctx, L"ConnectStatus", d->ConnectStatus);
    AicPutULong(Ctx, L"HandshakeMsg", d->Wpa.Msg);
    AicPutULong(Ctx, L"LinkFreq", d->LinkFreq);
    MacText(d->Bssid, mac);
    AicPutString(Ctx, L"LinkBssid", mac);
    AicCrumb(Ctx, rc == AIC_OK ? "runtime: link up" : "runtime: connect failed");
    return rc;
}

/*
 * In PrepareHardware (readers running): the stale-firmware check, the chip
 * ID, and from stage 2 the firmware init, so that the adapter can start
 * with the module's MAC address. Sets Ctx->FwReady.
 */
static VOID
RuntimeInit(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    aic_dev *d = &Ctx->Aic;
    CHAR mac[18];
    int rc;

    Ctx->FwReady = FALSE;
    if (GetBoot(L"FirmwareLoaded") != 1) {
        ULONG reboots = GetBoot(L"Reboots");

        /* Not started by us this boot: left running across a warm reboot
         * (or by something else). Send it back to the loader once or twice;
         * the loader instance then downloads it again. */
        if (reboots < 2) {
            PutBoot(L"Reboots", reboots + 1);
            AicCrumb(Ctx, "runtime: stale firmware, rebooting the chip to its loader");
            (VOID)aic_fw_reboot(d);
            return;
        }
        AicCrumb(Ctx, "runtime: stale firmware and reboots exhausted, carrying on");
    }
    AicCrumb(Ctx, "runtime: identify");
    rc = aic_identify(d);
    PutChip(Ctx);
    if (rc != AIC_OK || Ctx->Stage < AIC_STAGE_SCAN) {
        return;
    }
    AicCrumb(Ctx, "runtime: init");
    rc = aic_wlan_init(d);
    AicPutString(Ctx, L"FwVersion", d->FwVersion);
    MacText(d->Mac, mac);
    AicPutString(Ctx, L"MacAddress", mac);
    AicPutULong(Ctx, L"Is5gSupported", d->Is5gSupported);
    AicPutULong(Ctx, L"LmacVersion", d->LmacVersion);
    AicPutULong(Ctx, L"FwFeatures", d->Features);
    if (rc != AIC_OK) {
        AicCrumb(Ctx, "runtime: init failed");
        return;
    }
    Ctx->FwReady = TRUE;
}

/* The worker: scan, join, then keep the link and report it to Windows. */
static VOID
RuntimeStage(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    aic_dev *d = &Ctx->Aic;
    ULONG tick = 0, retry = 0;

    if (!Ctx->FwReady) {
        return;
    }
    (VOID)ScanAndJoin(Ctx, Ctx->Stage >= AIC_STAGE_CONNECT);
    PutStats(Ctx);
    if (Ctx->Stage < AIC_STAGE_CONNECT) {
        return;
    }
    AicNetUpdateLink(Ctx);
    while (!Ctx->Stopping) {
        if (!aic_poll(d)) {
            AicUsbOps.wait(Ctx, 200);
            tick++;
        }
        AicNetUpdateLink(Ctx);
        if (d->Link == AIC_LINK_DOWN && tick >= 25) {
            tick = 0;
            if (++retry <= 20) {
                AicPutULong(Ctx, L"Reconnects", retry);
                AicPutULong(Ctx, L"DisconnectReason", d->DisconnectReason);
                (VOID)ScanAndJoin(Ctx, 1);
                AicNetUpdateLink(Ctx);
            }
        }
        if (tick && (tick % 50) == 0) {
            PutStats(Ctx);
        }
    }
    (VOID)aic_disconnect(d);
    AicNetUpdateLink(Ctx);
}

static VOID
AicWorkerMain(_In_ PVOID Context)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)Context;

    if (!ctx->Runtime) {
        (VOID)LoaderStage(ctx);
    } else {
        RuntimeStage(ctx);
    }
    PutStats(ctx);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------ PnP */

static NTSTATUS
AicEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    ctx->Stage = AicGetULong(ctx, L"Stage", AIC_STAGE_OPEN);
    AicCrumb(ctx, "prepare");
    if (ctx->Ring == NULL) {
        ctx->Ring = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)AIC_RING_SLOTS * AIC_RING_SLOT_SIZE,
                                           AIC_POOL_TAG);
        ctx->TxBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, AIC_CMD_BUF_MAX + 2048, AIC_POOL_TAG);
        if (ctx->Ring == NULL || ctx->TxBuf == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    status = AicUsbPrepare(ctx);
    AicPutULong(ctx, L"OpenStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        AicCrumb(ctx, "prepare: USB setup failed");
        return status;
    }
    ctx->Aic.Ops = &AicUsbOps;
    ctx->Aic.Ctx = ctx;
    ctx->Aic.TxBuf = ctx->TxBuf;
    ctx->Aic.TxBufSize = AIC_CMD_BUF_MAX + 2048;
    aic_init(&ctx->Aic);
    AicCrumb(ctx, ctx->Runtime ? "prepare: runtime firmware (PID_8D81)" : "prepare: ROM loader (PID_8D80)");
    if (ctx->Adapter == NULL) {
        return STATUS_SUCCESS;          /* the loader service */
    }
    if (ctx->Runtime && ctx->Stage >= AIC_STAGE_FIRMWARE && NT_SUCCESS(AicUsbStartReaders(ctx))) {
        RuntimeInit(ctx);
    }
    status = AicNetStart(ctx);
    AicPutULong(ctx, L"AdapterStartStatus", (ULONG)status);
    return status;
}

static NTSTATUS
AicEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    AicUsbStopReaders(ctx);
    if (ctx->Ring) {
        ExFreePoolWithTag(ctx->Ring, AIC_POOL_TAG);
        ctx->Ring = NULL;
    }
    if (ctx->TxBuf) {
        ExFreePoolWithTag(ctx->TxBuf, AIC_POOL_TAG);
        ctx->TxBuf = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
AicEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);
    if (ctx->Stage == AIC_STAGE_OPEN) {
        return STATUS_SUCCESS;
    }
    return AicUsbStartReaders(ctx);
}

static NTSTATUS
AicEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    AicUsbStopReaders(AicGetContext(Device));
    return STATUS_SUCCESS;
}

static NTSTATUS
StartWorker(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    HANDLE h;
    NTSTATUS status;

    if (Ctx->Stage == AIC_STAGE_OPEN || Ctx->Worker != NULL) {
        return STATUS_SUCCESS;
    }
    InterlockedExchange(&Ctx->Stopping, 0);
    Ctx->Aic.Abort = 0;
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, AicWorkerMain, Ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Ctx->Worker,
                                       NULL);
    ZwClose(h);
    return status;
}

static VOID
StopWorker(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Worker == NULL) {
        return;
    }
    InterlockedExchange(&Ctx->Stopping, 1);
    Ctx->Aic.Abort = 1;
    KeSetEvent(&Ctx->RxEvent, IO_NO_INCREMENT, FALSE);
    (VOID)KeWaitForSingleObject(Ctx->Worker, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(Ctx->Worker);
    Ctx->Worker = NULL;
}

static NTSTATUS
AicEvtSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    return StartWorker(AicGetContext(Device));
}

static NTSTATUS
AicEvtSelfManagedIoRestart(_In_ WDFDEVICE Device)
{
    return StartWorker(AicGetContext(Device));
}

static NTSTATUS
AicEvtSelfManagedIoSuspend(_In_ WDFDEVICE Device)
{
    StopWorker(AicGetContext(Device));
    return STATUS_SUCCESS;
}

static VOID
AicEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    StopWorker(AicGetContext((WDFDEVICE)Object));
    AicNetFree(AicGetContext((WDFDEVICE)Object));
}

static NTSTATUS
AicEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    PAIC_DEVICE_CONTEXT ctx;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    if (!g_LoaderService) {
        status = NetDeviceInitConfig(DeviceInit);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = AicEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = AicEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = AicEvtD0Entry;
    pnp.EvtDeviceD0Exit = AicEvtD0Exit;
    pnp.EvtDeviceSelfManagedIoInit = AicEvtSelfManagedIoInit;
    pnp.EvtDeviceSelfManagedIoRestart = AicEvtSelfManagedIoRestart;
    pnp.EvtDeviceSelfManagedIoSuspend = AicEvtSelfManagedIoSuspend;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AIC_DEVICE_CONTEXT);
    attr.EvtCleanupCallback = AicEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = AicGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device = device;
    KeInitializeSpinLock(&ctx->RingLock);
    KeInitializeEvent(&ctx->RxEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&ctx->DataLock);
    if (!g_LoaderService) {
        return AicNetCreateAdapter(ctx);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR ldr[] = L"\\aic8800ldr";
    const USHORT n = (USHORT)(sizeof(ldr) - sizeof(WCHAR));
    WDF_DRIVER_CONFIG config;

    /* Which service is this: ...\Services\aic8800ldr is the loader. */
    if (RegistryPath->Length >= n) {
        UNICODE_STRING tail, want;

        tail.Buffer = (PWCH)((PUCHAR)RegistryPath->Buffer + RegistryPath->Length - n);
        tail.Length = tail.MaximumLength = n;
        RtlInitUnicodeString(&want, ldr);
        g_LoaderService = RtlEqualUnicodeString(&tail, &want, TRUE);
    }

    WDF_DRIVER_CONFIG_INIT(&config, AicEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
