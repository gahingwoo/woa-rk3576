/*
 * wificx.cpp - the WiFiCx 1.0 front end of bcmwlsdio (Stage 3,
 * bcmwlwifi.inf): Windows' own Wi-Fi on top of the Broadcom core, for
 * Windows 11 22H2/23H2 (WiFiCx 1.0, NetAdapterCx 2.2). The design and most
 * of the code are aic8800usb's wificx.cpp (docs/dev/wificx.md), with the
 * Broadcom core's calls (bwl_wlan.c) in place of the AIC8800's.
 *
 * WiFiCx speaks WDI: each command is a WIFIREQUEST holding a
 * WDI_MESSAGE_HEADER and TLVs, which Microsoft's TLV library
 * (WificxTLVGenParse.lib) parses and generates at the version the OS
 * speaks. A property message is done with its completion (M3); a task gets
 * its M3 when accepted and later its completion indication (M4) with the
 * same transaction ID, the M3 always first.
 *
 * Threads: EvtWifiDeviceSendCommand (IRQL <= DISPATCH_LEVEL) only queues
 * the request and wakes the worker thread (driver.c), which runs
 * BwlWifiRun: one request at a time, each completed exactly once, and in
 * between the poller (BwlService: firmware events, scan results, data
 * frames), the TX FIFO, the task timeouts and the link's rate and signal.
 * The worker is the only thread that talks to the card.
 *
 * What runs where:
 *   - Windows keeps the network list (BSSListCachemanagement FALSE): a scan
 *     is one escan, and every result goes up (BSS_ENTRY_LIST, then
 *     SCAN_COMPLETE).
 *   - TASK_CONNECT: the first candidate that is open, or WPA2-PSK with
 *     CCMP, is joined with C_SET_SSID (BSSID and channel given); then
 *     ASSOCIATION_RESULT (with the association request the firmware sent,
 *     whose RSN element Windows puts into message 2, and the AP's beacon
 *     body), LINK_STATE_CHANGE and CONNECT_COMPLETE.
 *   - WPA2-PSK: by default Windows runs the 4-way handshake over the data
 *     path (sup_wpa 0, EAPOL as ordinary data frames) and hands the keys
 *     down with SET_ADD_CIPHER_KEYS ("wsec_key"). With FwSupplicant = 1
 *     and a Passphrase in the registry (WiFiCx never gives the driver the
 *     passphrase), the firmware runs it instead (sup_wpa 1,
 *     C_SET_WSEC_PMK) and the association is reported with the port
 *     already authorized; a test path for the board.
 * Station only: no Wi-Fi Direct, no hotspot, no WPA3, no PMF, no roaming
 * offload, CCMP only.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"
#include <wificx.h>
#include <TlvGeneratorParser.hpp>

#define BWL_WIFI_TAG        'wWcB'
#define BWL_WIFI_RING       32          /* WIFIREQUESTs waiting for the worker */
#define BWL_WIFI_BSS_MAX    64          /* networks per scan */
#define BWL_WIFI_BODY_BUF   0xC000      /* their frame bodies */
#define BWL_WIFI_BSS_PER_IND 16         /* entries per BSS_ENTRY_LIST */
#define BWL_WIFI_CONN_BODY  1024        /* the candidate's body, for ASSOCIATION_RESULT */
#define BWL_WIFI_TRACE      64          /* message IDs kept for WdiTrace */
#define BWL_WIFI_SCAN_MS    10000       /* a scan task ends (aborted) after this */
#define BWL_WIFI_FW_SCAN_MS 8000        /* a firmware scan still running is given up after this */
#define BWL_WIFI_LINK_MS    5000        /* rate and signal are read this often while connected */
#define BWL_WIFI_M4_WAIT_MS 200         /* a pairwise key waits this long at most for message 4 to go out */

#ifndef NDIS_STATUS_REQUEST_ABORTED
#define NDIS_STATUS_REQUEST_ABORTED ((NDIS_STATUS)0xC001000CL)
#endif

typedef struct _BWL_WIFI_BSS {
    UCHAR  Bssid[6];
    UCHAR  Channel;
    UCHAR  Band;            /* 0 2.4 GHz, 1 5 GHz */
    CHAR   Rssi;
    BOOLEAN Beacon;         /* the body is a beacon (else a probe response) */
    USHORT Len;
    ULONG  Off;             /* into Body */
} BWL_WIFI_BSS;

typedef struct _BWL_WIFI_TASK {
    BOOLEAN Active;         /* M3 sent, M4 not yet */
    BOOLEAN Aborted;
    UINT16  PortId;
    UINT32  Tid;
    UINT32  Ihv;
    ULONG   StartMs;
} BWL_WIFI_TASK;

typedef struct _BWL_WIFI {
    PBWL_DEVICE_CONTEXT Ctx;
    TLV_CONTEXT Tlv;                    /* PeerVersion: the OS's WDI version */

    KSPIN_LOCK Lock;                    /* the ring and Accepting */
    WIFIREQUEST Ring[BWL_WIFI_RING];
    ULONG RingHead, RingCount;
    BOOLEAN Accepting;                  /* between PrepareHardware and ReleaseHardware */

    BOOLEAN HwReady;                    /* firmware up, capabilities reported */
    BOOLEAN RadioOn;
    BOOLEAN AdapterUp;
    UINT16  PortId;                     /* the station adapter's */

    BWL_WIFI_TASK Scan;
    BOOLEAN FwScanning;                 /* escan sent, its end not seen */
    ULONG   FwScanStartMs;
    ULONG   BssCount, BssSent, BodyUsed;
    BWL_WIFI_BSS Bss[BWL_WIFI_BSS_MAX];

    BWL_WIFI_TASK Conn;
    BOOLEAN Connected;                  /* ASSOCIATION_RESULT (success) sent, no DISASSOCIATION since */
    BOOLEAN ConnFwSup;                  /* this connection: the firmware's supplicant */
    UCHAR   ConnBssid[6];
    UCHAR   ConnChannel, ConnBand;
    CHAR    ConnRssi;
    USHORT  ConnBodyLen;
    UCHAR   ConnBody[BWL_WIFI_CONN_BODY];
    ULONG   ConnTimeoutMs;
    ULONG   LastLinkMs;
    ULONG   ReportedRateKbps;
    LONG    ReportedRssi;

    /* settings */
    ULONG   FwSupMode;                  /* FwSupplicant */
    CHAR    Passphrase[68];
    CHAR    FwSupSsid[36];

    /* records (Device Parameters) */
    ULONG Msgs, Unsupported, Indications, IndicationErrors, Keys, KeyErrors, ScanTasks, ConnectTasks;
    ULONG BssIndicated, Disassociations, LastConnectStatus, LastKeyStatus, Refused, LinkUpdates;
    ULONG ServiceErrors, ServiceErrorRun;
    ULONG KeyEapolWaitMs;               /* the last pairwise key: how long message 4 took to go out */
    u32   EapolRx0, EapolTx0;           /* the EAPOL counters at this connection's start or last pairwise key */
    LONG  LastServiceError;
    USHORT Trace[BWL_WIFI_TRACE];
    ULONG TraceCount;
    ULONG LastRecordMs;

    UCHAR Body[BWL_WIFI_BODY_BUF];
} BWL_WIFI, *PBWL_WIFI;

static EVT_WIFI_DEVICE_SEND_COMMAND BwlEvtWifiSendCommand;
static EVT_WIFI_DEVICE_CREATE_ADAPTER BwlEvtWifiCreateAdapter;
static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE BwlEvtWifiCreateWifiDirectDevice;

/* ------------------------------------------- the TLV library's memory */

/* WificxTLVGenParse.lib allocates through these (arrayofelements.hpp);
 * AllocationContext is TLV_CONTEXT.AllocationContext, always 0 here. */
void * __cdecl
operator new(size_t Size, ULONG_PTR AllocationContext) throw()
{
    UNREFERENCED_PARAMETER(AllocationContext);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size != 0 ? Size : 1, BWL_WIFI_TAG);
}

void __cdecl
operator delete(void *P) noexcept
{
    if (P != NULL) {
        ExFreePoolWithTag(P, BWL_WIFI_TAG);
    }
}

void __cdecl
operator delete[](void *P) noexcept
{
    if (P != NULL) {
        ExFreePoolWithTag(P, BWL_WIFI_TAG);
    }
}

void __cdecl
operator delete(void *P, size_t Size) noexcept
{
    UNREFERENCED_PARAMETER(Size);
    if (P != NULL) {
        ExFreePoolWithTag(P, BWL_WIFI_TAG);
    }
}

void __cdecl
operator delete[](void *P, size_t Size) noexcept
{
    UNREFERENCED_PARAMETER(Size);
    if (P != NULL) {
        ExFreePoolWithTag(P, BWL_WIFI_TAG);
    }
}

/* ------------------------------------------------------------ helpers */

static ULONG
NowMs(VOID)
{
    return (ULONG)(KeQueryInterruptTime() / 10000);
}

static WDI_BAND_ID
WdiBand(UCHAR Band)
{
    return Band == 0 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
}

/* RSSI to WDI's link quality, 0..100 over -100..-50 dBm */
static UINT32
LinkQuality(LONG Rssi)
{
    LONG q = 2 * (Rssi + 100);

    return (UINT32)(q < 0 ? 0 : q > 100 ? 100 : q);
}

static NTSTATUS
NdisToNt(NDIS_STATUS Status)
{
    if (Status == NDIS_STATUS_SUCCESS) {
        return STATUS_SUCCESS;
    }
    if (Status == NDIS_STATUS_RESOURCES) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_INVALID_PARAMETER;
}

static VOID
Complete(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(W);
    WifiRequestComplete(Request, Status, sizeof(WDI_MESSAGE_HEADER));
}

/* An indication: the WDI header, then the TLVs, in a WDFMEMORY that WiFiCx
 * copies during the call. */
static VOID
Indicate(_In_ PBWL_WIFI W, _In_ UINT16 MessageId, _In_ UINT16 PortId, _In_ UINT32 Tid, _In_ UINT32 Ihv,
         _In_ NDIS_STATUS Status, _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDFMEMORY mem;
    PWDI_MESSAGE_HEADER h;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = W->Ctx->Device;
    if (!NT_SUCCESS(WdfMemoryCreate(&attr, NonPagedPoolNx, BWL_WIFI_TAG, sizeof(*h) + TlvLen, &mem, (PVOID *)&h))) {
        W->IndicationErrors++;
        return;
    }
    RtlZeroMemory(h, sizeof(*h));
    h->PortId = PortId;
    h->Status = Status;
    h->TransactionId = Tid;
    h->IhvSpecificId = Ihv;
    if (Tlv != NULL && TlvLen != 0) {
        RtlCopyMemory(h + 1, Tlv, TlvLen);
    }
    WifiDeviceReceiveIndication(W->Ctx->Device, MessageId, mem);
    WdfObjectDelete(mem);
    W->Indications++;
}

/* A task's completion indication, header only. */
static VOID
TaskDone(_In_ PBWL_WIFI W, _Inout_ BWL_WIFI_TASK *T, _In_ UINT16 MessageId, _In_ NDIS_STATUS Status)
{
    if (!T->Active) {
        return;
    }
    T->Active = FALSE;
    Indicate(W, MessageId, T->PortId, T->Tid, T->Ihv, Status, NULL, 0);
}

static VOID
TaskStart(_Out_ BWL_WIFI_TASK *T, _In_ const WDI_MESSAGE_HEADER *H)
{
    T->Active = TRUE;
    T->Aborted = FALSE;
    T->PortId = H->PortId;
    T->Tid = H->TransactionId;
    T->Ihv = H->IhvSpecificId;
    T->StartMs = NowMs();
}

/* One poll of the card and what the TX FIFO holds. Frames read plus 1 if
 * anything was sent; 0 when idle. */
static int
Service(_In_ PBWL_WIFI W)
{
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    int r, n = 0;

    if (!ctx->FwReady) {
        return 0;
    }
    r = BwlService(&ctx->Bwl);
    if (r < 0) {
        W->ServiceErrors++;
        W->ServiceErrorRun++;
        W->LastServiceError = r;
        if (W->ServiceErrorRun > 20) {
            BwlDelayMs(100);            /* the card does not answer: do not spin on it */
        }
    } else {
        W->ServiceErrorRun = 0;
        n = r;
    }
    if (BwlNetTxPump(ctx)) {
        n++;
    }
    return n;
}

/* ------------------------------------------------------------- records */

static VOID
PutRecords(_In_ PBWL_WIFI W)
{
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    const bwl_wlan *w = &ctx->Bwl.Wlan;
    CHAR text[BWL_WIFI_TRACE * 5 + 1];
    PCHAR p = text;
    size_t left = sizeof(text);
    ULONG i, n = W->TraceCount < BWL_WIFI_TRACE ? W->TraceCount : BWL_WIFI_TRACE;

    text[0] = 0;
    for (i = 0; i < n; i++) {
        (VOID)RtlStringCchPrintfExA(p, left, &p, &left, 0, "%u ", (unsigned)W->Trace[i]);
    }
    BwlPutString(ctx, L"WdiTrace", text);
    BwlPutULong(ctx, L"WdiMessages", W->Msgs);
    BwlPutULong(ctx, L"WdiUnsupported", W->Unsupported);
    BwlPutULong(ctx, L"WdiRefused", W->Refused);
    BwlPutULong(ctx, L"WdiIndications", W->Indications);
    BwlPutULong(ctx, L"WdiIndicationErrors", W->IndicationErrors);
    BwlPutULong(ctx, L"WdiScanTasks", W->ScanTasks);
    BwlPutULong(ctx, L"WdiBssIndicated", W->BssIndicated);
    BwlPutULong(ctx, L"WdiConnectTasks", W->ConnectTasks);
    BwlPutULong(ctx, L"WdiLastConnectStatus", W->LastConnectStatus);
    BwlPutULong(ctx, L"WdiKeys", W->Keys);
    BwlPutULong(ctx, L"WdiKeyErrors", W->KeyErrors);
    BwlPutULong(ctx, L"WdiLastKeyStatus", W->LastKeyStatus);
    BwlPutULong(ctx, L"KeyEapolWaitMs", W->KeyEapolWaitMs);
    BwlPutULong(ctx, L"WdiDisassociations", W->Disassociations);
    BwlPutULong(ctx, L"WdiRadioOn", W->RadioOn);
    BwlPutULong(ctx, L"WdiConnected", W->Connected);
    BwlPutULong(ctx, L"WdiLinkUpdates", W->LinkUpdates);
    BwlPutULong(ctx, L"ServiceErrors", W->ServiceErrors);
    BwlPutULong(ctx, L"LastServiceError", (ULONG)W->LastServiceError);
    BwlPutULong(ctx, L"LinkState", (ULONG)w->Link);
    BwlPutULong(ctx, L"FwSupplicantUsed", W->ConnFwSup);
    BwlPutULong(ctx, L"ScanResults", w->ScanResults);
    BwlPutULong(ctx, L"ScanStatus", w->ScanStatus);
    BwlPutULong(ctx, L"ConnStatus", w->ConnStatus);
    BwlPutULong(ctx, L"ConnEvent", w->ConnEvent);
    BwlPutULong(ctx, L"DisconnectReason", w->DisconnectReason);
    BwlPutULong(ctx, L"DisconnectEvent", w->DisconnectEvent);
    BwlPutULong(ctx, L"LinkRateKbps", w->RateKbps);
    BwlPutULong(ctx, L"LinkRssi", (ULONG)w->Rssi);
    BwlPutULong(ctx, L"RxData", w->RxData);
    BwlPutULong(ctx, L"RxEapol", w->RxEapol);
    BwlPutULong(ctx, L"TxData", w->TxData);
    BwlPutULong(ctx, L"TxDataDrops", w->TxDataDrops);
    BwlPutULong(ctx, L"TxEapol", w->TxEapol);
    BwlPutULong(ctx, L"FwKeys", w->Keys);
    BwlPutULong(ctx, L"FlowHold", w->FlowHold);
    BwlPutULong(ctx, L"WlanFwStatus", w->LastFwStatus);
    BwlPutULong(ctx, L"NetTxFrames", ctx->TxFrames);
    BwlPutULong(ctx, L"NetTxDrops", ctx->TxDrops);
    BwlPutULong(ctx, L"NetTxErrors", ctx->TxErrors);
    BwlPutULong(ctx, L"NetRxFrames", ctx->RxFrames);
    BwlPutULong(ctx, L"NetRxDrops", ctx->RxDrops);
    BwlPutCounters(ctx);
}

/* ---------------------------------------------------------------- scan */

/* BwlNetOps.scan_bss, on the worker thread inside the poller: keep the
 * body for the next BSS_ENTRY_LIST (one entry per BSSID, the latest body). */
extern "C" VOID
BwlWifiScanBss(_In_ void *C, _In_ const bwl_bss *Bss, _In_reads_(Len) const u8 *Body, _In_ u32 Len)
{
    PBWL_WIFI W = ((PBWL_DEVICE_CONTEXT)C)->Wifi;
    BWL_WIFI_BSS *e = NULL;
    ULONG i;

    if (W == NULL || !W->Scan.Active || W->Scan.Aborted || Len < 12 || Len > 2304 || Bss->Channel == 0) {
        return;
    }
    for (i = W->BssSent; i < W->BssCount; i++) {
        if (RtlEqualMemory(W->Bss[i].Bssid, Bss->Bssid, 6)) {
            e = &W->Bss[i];
            break;
        }
    }
    if (e == NULL || e->Len < Len) {
        if (W->BodyUsed + Len > sizeof(W->Body)) {
            return;                             /* full: the rest go in the next scan */
        }
        if (e == NULL) {
            if (W->BssCount >= BWL_WIFI_BSS_MAX) {
                return;
            }
            e = &W->Bss[W->BssCount++];
            RtlCopyMemory(e->Bssid, Bss->Bssid, 6);
        }
        e->Off = W->BodyUsed;
        W->BodyUsed += Len;
    }
    RtlCopyMemory(W->Body + e->Off, Body, Len);
    e->Len = (USHORT)Len;
    e->Beacon = Bss->Beacon != 0;
    e->Channel = Bss->Channel;
    e->Band = Bss->Band;
    e->Rssi = (CHAR)(Bss->Rssi < -127 ? -127 : Bss->Rssi > 0 ? 0 : Bss->Rssi);
}

/* The entries collected and not yet sent, BWL_WIFI_BSS_PER_IND at a time
 * (unsolicited: TransactionId 0; the bodies from the Timestamp on). */
static VOID
IndicateBss(_In_ PBWL_WIFI W)
{
    while (W->BssSent < W->BssCount) {
        WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS P;
        ULONG n = W->BssCount - W->BssSent, i;
        UINT8 *tlv = NULL;
        ULONG tlvLen = 0;

        if (n > BWL_WIFI_BSS_PER_IND) {
            n = BWL_WIFI_BSS_PER_IND;
        }
        if (P.DeviceDescriptor.AllocateElements(n, 0) != NDIS_STATUS_SUCCESS) {
            W->IndicationErrors++;
            return;
        }
        P.Optional.DeviceDescriptor_IsPresent = 1;
        for (i = 0; i < n; i++) {
            const BWL_WIFI_BSS *b = &W->Bss[W->BssSent + i];
            WDI_BSS_ENTRY_CONTAINER *e = &P.DeviceDescriptor.pElements[i];

            RtlCopyMemory(e->BSSID.Address, b->Bssid, 6);
            if (!b->Beacon) {
                e->ProbeResponseFrame.SimpleAssign(W->Body + b->Off, b->Len);
                e->Optional.ProbeResponseFrame_IsPresent = 1;
            } else {
                e->BeaconFrame.SimpleAssign(W->Body + b->Off, b->Len);
                e->Optional.BeaconFrame_IsPresent = 1;
            }
            e->SignalInfo.RSSI = b->Rssi;
            e->SignalInfo.LinkQuality = LinkQuality(b->Rssi);
            e->ChannelInfo.ChannelNumber = b->Channel;
            e->ChannelInfo.BandId = WdiBand(b->Band);
        }
        if (GenerateWdiIndicationBssEntryListFromIhv(&P, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
            W->IndicationErrors++;
            return;
        }
        Indicate(W, WDI_INDICATION_BSS_ENTRY_LIST, W->Scan.PortId, WDI_TRANSACTION_ID_UNSOLICIT, W->Scan.Ihv,
                 NDIS_STATUS_SUCCESS, tlv, tlvLen);
        FreeGenerated(tlv);
        W->BssSent += n;
        W->BssIndicated += n;
    }   /* P frees its entry array; the bodies are not its own (SimpleAssign) */
}

/* The end of the scan task: what was heard, then SCAN_COMPLETE (also when
 * aborted, with NDIS_STATUS_REQUEST_ABORTED). */
static VOID
ScanFinish(_In_ PBWL_WIFI W, _In_ NDIS_STATUS Status)
{
    if (!W->Scan.Active) {
        return;
    }
    IndicateBss(W);
    TaskDone(W, &W->Scan, WDI_INDICATION_SCAN_COMPLETE, Status);
}

/* The firmware takes one scan at a time: wait for one still running. */
static BOOLEAN
WaitFwScan(_In_ PBWL_WIFI W)
{
    bwl_wlan *w = &W->Ctx->Bwl.Wlan;

    while (W->FwScanning && w->Scanning && !W->Ctx->WorkerStop) {
        if (NowMs() - W->FwScanStartMs > BWL_WIFI_FW_SCAN_MS) {
            break;
        }
        if (Service(W) == 0) {
            BwlDelayMs(2);
        }
    }
    W->FwScanning = FALSE;
    w->Events &= ~BWL_EV_SCAN_DONE;
    return !W->Ctx->WorkerStop;
}

static VOID
TaskScan(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H)
{
    /* Parameters not used: the firmware scans every channel with the
     * wildcard SSID, and Windows keeps what it hears (the SSID list only
     * matters for hidden networks, not supported yet). */
    if (!W->HwReady || !W->RadioOn || W->Scan.Active) {
        Complete(W, Request, W->Scan.Active ? STATUS_DEVICE_BUSY : STATUS_DEVICE_NOT_READY);
        return;
    }
    if (!WaitFwScan(W)) {
        Complete(W, Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    W->ScanTasks++;
    W->BssCount = W->BssSent = W->BodyUsed = 0;
    TaskStart(&W->Scan, H);
    if (BwlScanStart(&W->Ctx->Bwl) != BWL_OK) {
        W->Scan.Active = FALSE;
        Complete(W, Request, STATUS_UNSUCCESSFUL);      /* no M4 after a failed M3 */
        return;
    }
    W->FwScanning = TRUE;
    W->FwScanStartMs = NowMs();
    Complete(W, Request, STATUS_SUCCESS);               /* M3; M4 when the firmware is done */
}

/* ------------------------------------------------------------- connect */

/* The AP announces WMM (vendor element 00:50:F2, type 2) in its body. */
static BOOLEAN
HasWmm(_In_reads_(Len) const UCHAR *Body, _In_ ULONG Len)
{
    ULONG i = 12;

    while (i + 2 <= Len && i + 2 + Body[i + 1] <= Len) {
        if (Body[i] == 221 && Body[i + 1] >= 4 && Body[i + 2] == 0x00 && Body[i + 3] == 0x50 &&
            Body[i + 4] == 0xF2 && Body[i + 5] == 0x02) {
            return TRUE;
        }
        i += 2u + Body[i + 1];
    }
    return FALSE;
}

static VOID
IndicateAssociation(_In_ PBWL_WIFI W, _In_ BOOLEAN Success)
{
    bwl_dev *d = &W->Ctx->Bwl;
    WDI_INDICATION_ASSOCIATION_RESULT_LIST P;
    WDI_ASSOCIATION_RESULT_CONTAINER *e;
    WDI_PHY_TYPE phy;
    UCHAR req[BWL_ASSOC_IE_MAX + 8], rsp[BWL_ASSOC_IE_MAX + 8];
    ULONG reqLen = 0, rspLen = 0;
    BOOLEAN rsn = d->Wlan.Target.Security == BWL_SEC_WPA2_PSK;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    if (P.AssociationResults.AllocateElements(1, 0) != NDIS_STATUS_SUCCESS) {
        W->IndicationErrors++;
        return;
    }
    e = &P.AssociationResults.pElements[0];
    RtlCopyMemory(e->BSSID.Address, Success ? d->Wlan.Bssid : W->ConnBssid, 6);
    e->AssociationResultParameters.AssociationStatus =
        Success ? WDI_ASSOC_STATUS_SUCCESS : WDI_ASSOC_STATUS_ASSOC_FAILED_BY_PEER;
    e->AssociationResultParameters.StatusCode = (UINT16)(Success ? 0 : d->Wlan.ConnStatus);
    e->AssociationResultParameters.ReAssociation = FALSE;
    e->AssociationResultParameters.AuthAlgorithm = rsn ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
    e->AssociationResultParameters.UnicastCipherAlgorithm = rsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    e->AssociationResultParameters.MulticastDataCipherAlgorithm = rsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    e->AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    /* Windows authorizes the port after its own handshake; with the
     * firmware's supplicant the keys are in already */
    e->AssociationResultParameters.PortAuthorized = Success && W->ConnFwSup;
    e->AssociationResultParameters.WMMQoSEnabled = HasWmm(W->ConnBody, W->ConnBodyLen);
    e->AssociationResultParameters.DSInfo = WDI_DS_UNKNOWN;
    e->AssociationResultParameters.BandID = WdiBand(W->ConnBand);
    e->AssociationResultParameters.IHVAssociationStatus = Success ? 0 : d->Wlan.ConnStatus;
    if (Success) {
        /* the request's RSN element goes into message 2, the beacon's is
         * checked against message 3 */
        reqLen = BwlAssocReqBody(d, req, (u32)sizeof(req));
        rspLen = BwlAssocRspBody(d, rsp, (u32)sizeof(rsp));
        if (reqLen != 0) {
            e->AssociationRequestFrame.SimpleAssign(req, reqLen);
            e->Optional.AssociationRequestFrame_IsPresent = 1;
        }
        if (rspLen != 0) {
            e->AssociationResponseFrame.SimpleAssign(rsp, rspLen);
            e->Optional.AssociationResponseFrame_IsPresent = 1;
        }
        if (W->ConnBodyLen != 0) {
            e->BeaconProbeResponse.SimpleAssign(W->ConnBody, W->ConnBodyLen);
            e->Optional.BeaconProbeResponse_IsPresent = 1;
        }
    }
    /* what the capabilities offer; the firmware may use more (VHT on 5 GHz) */
    phy = WDI_PHY_TYPE_HT;
    e->ActivePhyTypeList.SimpleAssign(&phy, 1);
    if (GenerateWdiIndicationAssociationResultFromIhv(&P, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
        W->IndicationErrors++;
        return;
    }
    Indicate(W, WDI_INDICATION_ASSOCIATION_RESULT, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT, W->Conn.Ihv,
             NDIS_STATUS_SUCCESS, tlv, tlvLen);
    FreeGenerated(tlv);
}

/* Rate (kbit/s) and signal of the link: the firmware's once read, else the scan's. */
static VOID
IndicateLinkState(_In_ PBWL_WIFI W)
{
    bwl_dev *d = &W->Ctx->Bwl;
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS P;
    WDI_LINK_INFO_CONTAINER link;
    LONG rssi = d->Wlan.RateKbps != 0 ? d->Wlan.Rssi : W->ConnRssi;
    UINT32 rate = d->Wlan.RateKbps != 0 ? d->Wlan.RateKbps : 65000;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    RtlCopyMemory(P.LinkStateChangeParameters.PeerMACAddress.Address, d->Wlan.Bssid, 6);
    P.LinkStateChangeParameters.TxLinkSpeed = rate;
    P.LinkStateChangeParameters.RxLinkSpeed = rate;
    P.LinkStateChangeParameters.LinkQuality = (UINT8)LinkQuality(rssi);
    RtlZeroMemory(&link, sizeof(link));
    link.LinkID = 0;
    RtlCopyMemory(link.LocalLinkMACAddress.Address, d->Wlan.Mac, 6);
    RtlCopyMemory(link.PeerLinkMACAddress.Address, d->Wlan.Bssid, 6);
    link.ChannelNumber = W->ConnChannel;
    link.BandId = WdiBand(W->ConnBand);
    link.RSSI = rssi;
    link.Bandwidth = 20;
    P.LinkInfo.SimpleAssign(&link, 1);  /* generated only for peers that know it */
    if (GenerateWdiIndicationLinkStateChangeFromIhv(&P, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
        W->IndicationErrors++;
        return;
    }
    Indicate(W, WDI_INDICATION_LINK_STATE_CHANGE, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT, W->Conn.Ihv,
             NDIS_STATUS_SUCCESS, tlv, tlvLen);
    FreeGenerated(tlv);
    W->ReportedRateKbps = rate;
    W->ReportedRssi = rssi;
    W->LinkUpdates++;
}

static VOID
IndicateDisassociation(_In_ PBWL_WIFI W, _In_ WDI_ASSOC_STATUS Reason)
{
    WDI_INDICATION_DISASSOCIATION_PARAMETERS P;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    if (!W->Connected) {
        return;
    }
    W->Connected = FALSE;
    W->Disassociations++;
    RtlCopyMemory(P.DisconnectIndicationParameters.MacAddress.Address, W->ConnBssid, 6);
    P.DisconnectIndicationParameters.DisassociationWABIReason = Reason;
    if (GenerateWdiIndicationDisassociationFromIhv(&P, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
        W->IndicationErrors++;
        return;
    }
    Indicate(W, WDI_INDICATION_DISASSOCIATION, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT, W->Conn.Ihv,
             NDIS_STATUS_SUCCESS, tlv, tlvLen);
    FreeGenerated(tlv);
}

/* Leave the network (the firmware drops the keys with it). */
static VOID
Leave(_In_ PBWL_WIFI W, _In_ WDI_ASSOC_STATUS Reason)
{
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    bwl_wlan *w = &ctx->Bwl.Wlan;
    ULONG i;

    if (w->Link != BWL_LINK_DOWN && ctx->FwReady) {
        (VOID)BwlDisconnect(&ctx->Bwl, 3);      /* 3: deauthenticated because the station is leaving */
        /* the firmware's own events for this come now: take them, or they
         * would end the next connection */
        for (i = 0; i < 5 && !ctx->WorkerStop; i++) {
            (VOID)Service(W);
            BwlDelayMs(5);
        }
    }
    w->Link = BWL_LINK_DOWN;
    w->Events &= ~(BWL_EV_ASSOC | BWL_EV_KEYED | BWL_EV_DISCONNECT);
    BwlNetFlush(ctx);
    IndicateDisassociation(W, Reason);
}

static VOID
ConnectFinish(_In_ PBWL_WIFI W, _In_ NDIS_STATUS Status)
{
    W->LastConnectStatus = (ULONG)Status;
    TaskDone(W, &W->Conn, WDI_INDICATION_CONNECT_COMPLETE, Status);
}

static BOOLEAN
Listed(_In_ const WDI_ADDRESS_LIST_CONTAINER *List, _In_ const UCHAR *Bssid, _In_ BOOLEAN BroadcastIsAny)
{
    static const UCHAR bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    UINT32 i;

    for (i = 0; i < List->ElementCount; i++) {
        if (RtlEqualMemory(List->pElements[i].Address, Bssid, 6) ||
            (BroadcastIsAny && RtlEqualMemory(List->pElements[i].Address, bcast, 6))) {
            return TRUE;
        }
    }
    return FALSE;
}

/* The firmware's supplicant for this network: asked for, a passphrase, the
 * SSID it is meant for (or any), and a firmware that has one. */
static BOOLEAN
UseFwSup(_In_ PBWL_WIFI W, _In_ const bwl_bss *B)
{
    ULONG n = (ULONG)strlen(W->FwSupSsid);

    if (W->FwSupMode == 0 || B->Security != BWL_SEC_WPA2_PSK || strlen(W->Passphrase) < 8 ||
        !W->Ctx->Bwl.Wlan.FwSupCapable) {
        return FALSE;
    }
    return n == 0 || (n == B->SsidLen && RtlEqualMemory(W->FwSupSsid, B->Ssid, n));
}

static VOID
TaskConnect(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
{
    bwl_dev *d = &W->Ctx->Bwl;
    WDI_TASK_CONNECT_PARAMETERS P;
    const WDI_CONNECT_PARAMETERS_CONTAINER *cp;
    BOOLEAN open = FALSE, psk = FALSE, ccmp = FALSE, none = FALSE, found = FALSE;
    bwl_bss target;
    UINT32 i;
    NDIS_STATUS parsed;

    RtlZeroMemory(&target, sizeof(target));
    parsed = ParseWdiTaskConnectToIhv((ULONG)(InLen - sizeof(*H)), (const UINT8 *)(H + 1), &W->Tlv, &P);
    if (parsed != NDIS_STATUS_SUCCESS) {
        CleanupParsedWdiTaskConnectToIhv(&P);
        WifiRequestComplete(Request, NdisToNt(parsed), 0);
        return;
    }
    cp = &P.ConnectParameters;
    for (i = 0; i < cp->AuthenticationAlgorithms.ElementCount; i++) {
        open = open || cp->AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_80211_OPEN;
        psk = psk || cp->AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_RSNA_PSK;
    }
    for (i = 0; i < cp->UnicastCipherAlgorithms.ElementCount; i++) {
        ccmp = ccmp || cp->UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP;
        none = none || cp->UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_NONE;
    }
    open = open && none;
    psk = psk && ccmp;
    W->ConnBodyLen = 0;
    for (i = 0; (open || psk) && !found && i < P.PreferredBSSEntryList.ElementCount; i++) {
        const WDI_CONNECT_BSS_ENTRY_CONTAINER *b = &P.PreferredBSSEntryList.pElements[i];
        const UINT8 *body = NULL;
        UINT32 len = 0, ch = b->ChannelInfo.ChannelNumber;
        unsigned band;

        if (b->Optional.ProbeResponseFrame_IsPresent && b->ProbeResponseFrame.ElementCount != 0) {
            body = b->ProbeResponseFrame.pElements;
            len = b->ProbeResponseFrame.ElementCount;
        } else if (b->Optional.BeaconFrame_IsPresent && b->BeaconFrame.ElementCount != 0) {
            body = b->BeaconFrame.pElements;
            len = b->BeaconFrame.ElementCount;
        }
        if (body == NULL || len > BWL_WIFI_CONN_BODY || ch == 0 || ch > 196) {
            continue;
        }
        /* Wlansvc names the broadcast address as "any" */
        if (cp->Optional.AllowedBSSIDs_IsPresent && cp->AllowedBSSIDs.ElementCount != 0 &&
            !Listed(&cp->AllowedBSSIDs, b->BSSID.Address, TRUE)) {
            continue;
        }
        if (cp->Optional.DisallowedBSSIDs_IsPresent && Listed(&cp->DisallowedBSSIDs, b->BSSID.Address, FALSE)) {
            continue;
        }
        if (b->ChannelInfo.BandId == WDI_BAND_ID_2400) {
            band = 0;
        } else if (b->ChannelInfo.BandId == WDI_BAND_ID_5000 && d->Wlan.Has5g) {
            band = 1;
        } else {
            continue;
        }
        /* the network as Windows hands it back */
        if (BwlBssFromBody(b->BSSID.Address, ch, band, (int)b->SignalInfo.RSSI, body, len, &target) != BWL_OK) {
            continue;
        }
        if (!((open && target.Security == BWL_SEC_OPEN) || (psk && target.Security == BWL_SEC_WPA2_PSK))) {
            continue;
        }
        found = TRUE;
        RtlCopyMemory(W->ConnBody, body, len);
        W->ConnBodyLen = (USHORT)len;
        W->ConnChannel = (UCHAR)ch;
        W->ConnBand = (UCHAR)band;
        W->ConnRssi = (CHAR)(target.Rssi < -127 ? -127 : target.Rssi > 0 ? 0 : target.Rssi);
    }
    CleanupParsedWdiTaskConnectToIhv(&P);

    if (!W->HwReady || !W->RadioOn || W->Conn.Active) {
        Complete(W, Request, W->Conn.Active ? STATUS_DEVICE_BUSY : STATUS_DEVICE_NOT_READY);
        return;
    }
    if (!found) {
        /* nothing this driver can join: WPA3, 802.1X, TKIP, WEP */
        Complete(W, Request, STATUS_NOT_SUPPORTED);
        return;
    }
    W->ConnectTasks++;
    /* a scan in the firmware would hold the join back */
    if (W->Scan.Active) {
        W->Scan.Aborted = TRUE;
        ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    if (!WaitFwScan(W)) {
        Complete(W, Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    /* a roam (RoamRequest) or a new network: leave the old one first */
    if (d->Wlan.Link != BWL_LINK_DOWN || W->Connected) {
        Leave(W, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
    }
    BwlNetFlush(W->Ctx);
    TaskStart(&W->Conn, H);
    W->EapolRx0 = d->Wlan.RxEapol;      /* an earlier attempt's unanswered message 1 does not count */
    W->EapolTx0 = d->Wlan.TxEapol;
    RtlCopyMemory(W->ConnBssid, target.Bssid, 6);
    W->ConnFwSup = UseFwSup(W, &target);
    d->Wlan.RateKbps = 0;
    if (BwlConnect(d, &target, W->ConnFwSup ? W->Passphrase : NULL) != BWL_OK) {
        W->Conn.Active = FALSE;
        W->LastConnectStatus = (ULONG)STATUS_UNSUCCESSFUL;
        Complete(W, Request, STATUS_UNSUCCESSFUL);      /* no M4 after a failed M3 */
        return;
    }
    Complete(W, Request, STATUS_SUCCESS);               /* M3; the rest from ConnEvents */
}

/* Association, disconnection, the connect timeout and the link's rate,
 * between requests. */
static VOID
ConnEvents(_In_ PBWL_WIFI W)
{
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    bwl_wlan *w = &ctx->Bwl.Wlan;

    if (W->Conn.Active && (w->Events & BWL_EV_ASSOC)) {
        w->Events &= ~(BWL_EV_ASSOC | BWL_EV_KEYED);
        if (w->ConnStatus == 0 && w->Link >= BWL_LINK_ASSOCIATED) {
            RtlCopyMemory(W->ConnBssid, w->Bssid, 6);
            IndicateAssociation(W, TRUE);
            W->Connected = TRUE;
            IndicateLinkState(W);
            ConnectFinish(W, NDIS_STATUS_SUCCESS);
            W->LastLinkMs = NowMs();
            /* message 1 may be waiting already */
            BwlNetRxRelease(ctx);
        } else {
            IndicateAssociation(W, FALSE);
            ConnectFinish(W, NDIS_STATUS_FAILURE);
            BwlNetFlush(ctx);
        }
    }
    if (W->Conn.Active && NowMs() - W->Conn.StartMs > W->ConnTimeoutMs) {
        Leave(W, WDI_ASSOC_STATUS_ASSOC_EXCHANGE_FAILURE);
        IndicateAssociation(W, FALSE);
        ConnectFinish(W, NDIS_STATUS_FAILURE);
    }
    if (w->Events & BWL_EV_DISCONNECT) {
        w->Events &= ~BWL_EV_DISCONNECT;
        BwlNetFlush(ctx);
        if (W->Conn.Active) {
            IndicateAssociation(W, FALSE);
            ConnectFinish(W, NDIS_STATUS_FAILURE);
        }
        /* Wlansvc reconnects (TASK_CONNECT with RoamRequest) */
        IndicateDisassociation(W, w->DisconnectByPeer ? WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED
                                                      : WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN);
    }
    /* the firmware's rate and signal, now and then; up to Windows when they change */
    if (W->Connected && w->Link >= BWL_LINK_ASSOCIATED && NowMs() - W->LastLinkMs > BWL_WIFI_LINK_MS) {
        W->LastLinkMs = NowMs();
        if (BwlLinkInfo(&ctx->Bwl) == BWL_OK &&
            (w->RateKbps != W->ReportedRateKbps || w->Rssi - W->ReportedRssi > 3 || W->ReportedRssi - w->Rssi > 3)) {
            IndicateLinkState(W);
        }
    }
}

static VOID
TaskDisconnect(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H)
{
    WDI_MESSAGE_HEADER h = *H;

    Complete(W, Request, STATUS_SUCCESS);               /* M3 */
    if (W->Conn.Active) {
        ConnectFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    Leave(W, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
    Indicate(W, WDI_INDICATION_DISCONNECT_COMPLETE, h.PortId, h.TransactionId, h.IhvSpecificId,
             NDIS_STATUS_SUCCESS, NULL, 0);             /* M4 */
}

/* --------------------------------------------------------------- keys */

static VOID
SetAddCipherKeys(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
{
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS P;
    NTSTATUS status = STATUS_SUCCESS;
    NDIS_STATUS parsed;
    UINT32 i;

    parsed = ParseWdiSetAddCipherKeysToIhv((ULONG)(InLen - sizeof(*H)), (const UINT8 *)(H + 1), &W->Tlv, &P);
    if (parsed != NDIS_STATUS_SUCCESS) {
        CleanupParsedWdiSetAddCipherKeysToIhv(&P);
        Complete(W, Request, NdisToNt(parsed));
        return;
    }
    for (i = 0; i < P.SetCipherKey.ElementCount && NT_SUCCESS(status); i++) {
        const WDI_SET_ADD_CIPHER_KEYS_CONTAINER *k = &P.SetCipherKey.pElements[i];
        BOOLEAN pairwise = k->CipherKeyTypeInfo.KeyType == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY;
        UINT8 id = k->Optional.CipherKeyID_IsPresent ? (UINT8)(k->CipherKeyID.CipherKeyID & 3) : 0;
        int rc;

        if (k->CipherKeyTypeInfo.CipherAlgorithm != WDI_CIPHER_ALGO_CCMP || !k->Optional.CCMPKey_IsPresent ||
            k->CCMPKey.ElementCount != 16 ||
            (!pairwise && k->CipherKeyTypeInfo.KeyType != WDI_CIPHER_KEY_TYPE_GROUP_KEY)) {
            status = STATUS_NOT_SUPPORTED;      /* CCMP pairwise and group only */
            break;
        }
        if (W->ConnFwSup) {
            continue;                           /* the firmware has its own keys */
        }
        if (pairwise) {
            /* Message 4 must leave unencrypted before the key is in
             * (brcmfmac waits for its pending 802.1X frames the same way).
             * Windows sent it just before this, but it may still be in
             * NetAdapterCx's TX ring, not yet in the TX FIFO. Each EAPOL
             * frame from the AP (M1, M3, a group key message) gets one
             * answer, so wait until as many went out as came in. */
            ULONG waited = 0;

            while (ctx->Bwl.Wlan.TxEapol - W->EapolTx0 < ctx->Bwl.Wlan.RxEapol - W->EapolRx0 &&
                   waited < BWL_WIFI_M4_WAIT_MS && !ctx->WorkerStop) {
                (VOID)BwlNetTxPump(ctx);
                (VOID)BwlService(&ctx->Bwl);
                BwlDelayMs(1);
                waited++;
            }
            W->KeyEapolWaitMs = waited;
            BwlNetTxDrain(ctx, 50);
            BwlDelayMs(10);
            /* a later rekey counts from here */
            W->EapolRx0 = ctx->Bwl.Wlan.RxEapol;
            W->EapolTx0 = ctx->Bwl.Wlan.TxEapol;
        }
        rc = BwlSetKey(&ctx->Bwl, pairwise, id, k->CCMPKey.pElements, 16);
        W->Keys++;
        if (rc != BWL_OK) {
            W->KeyErrors++;
            status = rc == BWL_E_STATE ? STATUS_INVALID_DEVICE_STATE : STATUS_UNSUCCESSFUL;
        }
    }
    CleanupParsedWdiSetAddCipherKeysToIhv(&P);
    W->LastKeyStatus = (ULONG)status;
    Complete(W, Request, status);
}

/* ------------------------------------------------- radio, reset, abort */

static VOID
TaskSetRadioState(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
{
    WDI_SET_RADIO_STATE_PARAMETERS P;
    WDI_INDICATION_RADIO_STATUS_PARAMETERS R;
    WDI_MESSAGE_HEADER h = *H;
    NDIS_STATUS parsed;
    BOOLEAN on;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    parsed = ParseWdiTaskSetRadioStateToIhv((ULONG)(InLen - sizeof(*H)), (const UINT8 *)(H + 1), &W->Tlv, &P);
    on = parsed == NDIS_STATUS_SUCCESS && P.SoftwareRadioState != 0;
    CleanupParsedWdiTaskSetRadioStateToIhv(&P);
    if (parsed != NDIS_STATUS_SUCCESS) {
        WifiRequestComplete(Request, NdisToNt(parsed), 0);
        return;
    }
    Complete(W, Request, STATUS_SUCCESS);               /* M3 */
    if (!on) {
        /* the radio stays on in the firmware: off means not connected and
         * no scans */
        if (W->Scan.Active) {
            W->Scan.Aborted = TRUE;
            ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
        }
        if (W->Conn.Active) {
            ConnectFinish(W, NDIS_STATUS_REQUEST_ABORTED);
        }
        Leave(W, WDI_ASSOC_STATUS_RADIO_OFF);
    }
    if (on != W->RadioOn) {
        W->RadioOn = on;
        R.RadioState.HardwareState = TRUE;
        R.RadioState.SoftwareState = on;
        if (GenerateWdiIndicationRadioStatusFromIhv(&R, 0, &W->Tlv, &tlvLen, &tlv) == NDIS_STATUS_SUCCESS) {
            Indicate(W, WDI_INDICATION_RADIO_STATUS, h.PortId, WDI_TRANSACTION_ID_UNSOLICIT, h.IhvSpecificId,
                     NDIS_STATUS_SUCCESS, tlv, tlvLen);
            FreeGenerated(tlv);
        } else {
            W->IndicationErrors++;
        }
    }
    Indicate(W, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, h.PortId, h.TransactionId, h.IhvSpecificId,
             NDIS_STATUS_SUCCESS, NULL, 0);             /* M4 */
}

static VOID
TaskDot11Reset(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H)
{
    WDI_MESSAGE_HEADER h = *H;

    /* The parameters (default MIB, a MAC address) change nothing here: the
     * address stays the chip's. */
    if (!W->HwReady) {
        Complete(W, Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    Complete(W, Request, STATUS_SUCCESS);               /* M3 */
    if (W->Scan.Active) {
        W->Scan.Aborted = TRUE;
        ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    if (W->Conn.Active) {
        ConnectFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    Leave(W, WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET);
    Indicate(W, WDI_INDICATION_DOT11_RESET_COMPLETE, h.PortId, h.TransactionId, h.IhvSpecificId,
             NDIS_STATUS_SUCCESS, NULL, 0);             /* M4 */
}

static VOID
AbortTask(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
{
    WDI_TASK_ABORT_PARAMETERS P;
    NDIS_STATUS parsed;
    NDIS_OID oid;
    UINT32 tid;

    parsed = ParseWdiAbortTaskToIhv((ULONG)(InLen - sizeof(*H)), (const UINT8 *)(H + 1), &W->Tlv, &P);
    oid = P.CancelParameters.OriginalTaskOID;
    tid = P.CancelParameters.OriginalTransactionId;
    CleanupParsedWdiAbortTaskToIhv(&P);
    if (parsed != NDIS_STATUS_SUCCESS) {
        Complete(W, Request, STATUS_INVALID_PARAMETER);
        return;
    }
    Complete(W, Request, STATUS_SUCCESS);
    /* the bare message ID, or the OID form */
    if (W->Scan.Active && tid == W->Scan.Tid && (oid == WDI_TASK_SCAN || oid == WDI_DEFINE_OID(WDI_TASK_SCAN))) {
        /* the firmware finishes its scan; nothing more goes up from it */
        W->Scan.Aborted = TRUE;
        ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    } else if (W->Conn.Active && tid == W->Conn.Tid &&
               (oid == WDI_TASK_CONNECT || oid == WDI_DEFINE_OID(WDI_TASK_CONNECT))) {
        Leave(W, WDI_ASSOC_STATUS_ABORTED);     /* not connected yet: no DISASSOCIATION */
        ConnectFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
}

/* ------------------------------------------------------------ dispatch */

static VOID
Dispatch(_In_ PBWL_WIFI W, _In_ WIFIREQUEST Request)
{
    UINT inLen = 0, outLen = 0;
    PWDI_MESSAGE_HEADER h = (PWDI_MESSAGE_HEADER)WifiRequestGetInOutBuffer(Request, &inLen, &outLen);
    UINT16 id = WifiRequestGetMessageId(Request);

    W->Msgs++;
    if (W->TraceCount < BWL_WIFI_TRACE) {
        W->Trace[W->TraceCount] = id;
    }
    W->TraceCount++;
    if (h == NULL || inLen < sizeof(WDI_MESSAGE_HEADER)) {
        WifiRequestComplete(Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    switch (id) {
    case WDI_TASK_SET_RADIO_STATE:
        TaskSetRadioState(W, Request, h, inLen);
        break;
    case WDI_TASK_DOT11_RESET:
        TaskDot11Reset(W, Request, h);
        break;
    case WDI_TASK_SCAN:
        TaskScan(W, Request, h);
        break;
    case WDI_ABORT_TASK:
        AbortTask(W, Request, h, inLen);
        break;
    case WDI_TASK_CONNECT:
        TaskConnect(W, Request, h, inLen);
        break;
    case WDI_TASK_DISCONNECT:
        TaskDisconnect(W, Request, h);
        break;
    case WDI_SET_ADD_CIPHER_KEYS:
        SetAddCipherKeys(W, Request, h, inLen);
        break;
    case WDI_SET_ADAPTER_CONFIGURATION:
    case WDI_SET_PRIVACY_EXEMPTION_LIST:
    case WDI_SET_DELETE_CIPHER_KEYS:
    case WDI_SET_DEFAULT_KEY_ID:
    case WDI_SET_CONNECTION_QUALITY:
    case WDI_SET_RECEIVE_PACKET_FILTER:
    case WDI_SET_MULTICAST_LIST:
    case WDI_SET_FLUSH_BSS_ENTRY:
        /* Accepted, nothing to do: the address stays the chip's; the
         * firmware passes EAPOL in the clear before the pairwise key and
         * drops the keys when it leaves; every group frame goes up and
         * Windows filters; the OS keeps the network list. */
        Complete(W, Request, STATUS_SUCCESS);
        break;
    default:
        /* GET_STATISTICS, SET_LOCATION_PRIVACY and the rest: not needed
         * for a station */
        W->Unsupported++;
        Complete(W, Request, STATUS_NOT_SUPPORTED);
        break;
    }
}

/* IRQL <= DISPATCH_LEVEL: queue it and wake the worker, nothing else. */
static VOID
BwlEvtWifiSendCommand(_In_ WDFDEVICE Device, _In_ WIFIREQUEST SendRequest)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);
    PBWL_WIFI W = ctx->Wifi;
    NTSTATUS fail = STATUS_SUCCESS;
    KIRQL irql;

    if (W == NULL) {
        WifiRequestComplete(SendRequest, STATUS_DEVICE_NOT_READY, sizeof(WDI_MESSAGE_HEADER));
        return;
    }
    KeAcquireSpinLock(&W->Lock, &irql);
    if (!W->Accepting) {
        fail = STATUS_DEVICE_NOT_READY;
    } else if (W->RingCount == BWL_WIFI_RING) {
        fail = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        W->Ring[(W->RingHead + W->RingCount) % BWL_WIFI_RING] = SendRequest;
        W->RingCount++;
    }
    KeReleaseSpinLock(&W->Lock, irql);
    if (!NT_SUCCESS(fail)) {
        InterlockedIncrement((volatile LONG *)&W->Refused);
        WifiRequestComplete(SendRequest, fail, sizeof(WDI_MESSAGE_HEADER));
        return;
    }
    KeSetEvent(&ctx->WakeEvent, IO_NO_INCREMENT, FALSE);
}

static WIFIREQUEST
Pop(_In_ PBWL_WIFI W)
{
    WIFIREQUEST r = NULL;
    KIRQL irql;

    KeAcquireSpinLock(&W->Lock, &irql);
    if (W->RingCount != 0) {
        r = W->Ring[W->RingHead];
        W->RingHead = (W->RingHead + 1) % BWL_WIFI_RING;
        W->RingCount--;
    }
    KeReleaseSpinLock(&W->Lock, irql);
    return r;
}

/* The worker thread (driver.c), until the device stops. */
extern "C" VOID
BwlWifiRun(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PBWL_WIFI W = Ctx->Wifi;
    bwl_wlan *w = &Ctx->Bwl.Wlan;
    ULONG lastActive = NowMs();

    if (W == NULL) {
        return;
    }
    W->ConnTimeoutMs = BwlGetULong(Ctx, L"ConnectTimeoutMs", 15000);
    W->FwSupMode = BwlGetULong(Ctx, L"FwSupplicant", 0);
    (VOID)BwlGetAscii(Ctx, L"Passphrase", W->Passphrase, sizeof(W->Passphrase));
    (VOID)BwlGetAscii(Ctx, L"FwSupSsid", W->FwSupSsid, sizeof(W->FwSupSsid));
    BwlCrumb(Ctx, "wificx: running");
    while (!Ctx->WorkerStop) {
        WIFIREQUEST r = Pop(W);

        if (r != NULL) {
            Dispatch(W, r);
        } else {
            /* polled: fast while frames move, slower when idle; a WDI
             * request or a frame to send wakes the thread at once */
            LARGE_INTEGER t;
            ULONG ms = NowMs() - lastActive < 200 ? Ctx->PollMs : Ctx->IdlePollMs;

            t.QuadPart = -10000LL * (LONGLONG)(ms ? ms : 1);
            (VOID)KeWaitForSingleObject(&Ctx->WakeEvent, Executive, KernelMode, FALSE, &t);
        }
        if (Service(W) > 0) {
            lastActive = NowMs();
        }
        if (W->FwScanning && (w->Events & BWL_EV_SCAN_DONE)) {
            w->Events &= ~BWL_EV_SCAN_DONE;
            W->FwScanning = FALSE;
            ScanFinish(W, w->ScanStatus == 0 ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
        }
        if (W->Scan.Active && NowMs() - W->Scan.StartMs > BWL_WIFI_SCAN_MS) {
            W->Scan.Aborted = TRUE;
            ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
        }
        ConnEvents(W);
        if (NowMs() - W->LastRecordMs > 5000) {
            W->LastRecordMs = NowMs();
            PutRecords(W);
        }
    }
    /* stopping (suspend, removal): running tasks end; queued requests are
     * failed by BwlWifiPause (suspend) or ReleaseHardware. The link is left
     * up for a suspend; ReleaseHardware halts the chip. */
    if (W->Scan.Active) {
        W->Scan.Aborted = TRUE;
        ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    if (W->Conn.Active) {
        ConnectFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    W->FwScanning = FALSE;
    PutRecords(W);
    BwlCrumb(Ctx, "wificx: stopped");
}

/* ------------------------------------------------------- device, adapter */

extern "C" NTSTATUS
BwlWifiPreDeviceCreate(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    return WifiDeviceInitConfig(DeviceInit);
}

extern "C" NTSTATUS
BwlWifiPostDeviceCreate(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    WIFI_DEVICE_CONFIG cfg;
    PBWL_WIFI W;
    ULONG wdi;
    NTSTATUS status;

    W = (PBWL_WIFI)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BWL_WIFI), BWL_WIFI_TAG);
    if (W == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    W->Ctx = Ctx;
    KeInitializeSpinLock(&W->Lock);
    W->RadioOn = TRUE;
    Ctx->Wifi = W;

    /* the WDI version this driver was built for; WdiVersion in Device
     * Parameters overrides it on the board if WiFiCx 1.0 refuses it */
    wdi = BwlGetULong(Ctx, L"WdiVersion", 0);
    WIFI_DEVICE_CONFIG_INIT(&cfg, wdi != 0 ? wdi : WDI_VERSION_LATEST, BwlEvtWifiSendCommand,
                            BwlEvtWifiCreateAdapter, BwlEvtWifiCreateWifiDirectDevice);
    status = WifiDeviceInitialize(Ctx->Device, &cfg);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* parse and generate TLVs at the OS's version */
    W->Tlv.AllocationContext = 0;
    W->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(Ctx->Device);
    BwlPutULong(Ctx, L"OsWdiVersion", W->Tlv.PeerVersion);
    BwlPutULong(Ctx, L"DriverWdiVersion", wdi != 0 ? wdi : WDI_VERSION_LATEST);
    return STATUS_SUCCESS;
}

/* The worker has stopped: nothing more is taken, and what waits is
 * failed, so that nobody waits for it. */
extern "C" VOID
BwlWifiPause(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PBWL_WIFI W = Ctx->Wifi;
    WIFIREQUEST r;
    KIRQL irql;

    if (W == NULL) {
        return;
    }
    KeAcquireSpinLock(&W->Lock, &irql);
    W->Accepting = FALSE;
    KeReleaseSpinLock(&W->Lock, irql);
    while ((r = Pop(W)) != NULL) {
        WifiRequestComplete(r, STATUS_DEVICE_NOT_READY, sizeof(WDI_MESSAGE_HEADER));
    }
}

/* SelfManagedIoRestart, before the worker starts again. */
extern "C" VOID
BwlWifiResume(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PBWL_WIFI W = Ctx->Wifi;
    KIRQL irql;

    if (W == NULL) {
        return;
    }
    KeAcquireSpinLock(&W->Lock, &irql);
    W->Accepting = TRUE;
    KeReleaseSpinLock(&W->Lock, irql);
}

/* ReleaseHardware (the worker has stopped). */
extern "C" VOID
BwlWifiReleaseHardware(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PBWL_WIFI W = Ctx->Wifi;

    if (W == NULL) {
        return;
    }
    BwlWifiPause(Ctx);
    W->HwReady = FALSE;
    W->Connected = FALSE;
}

extern "C" VOID
BwlWifiCleanup(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PBWL_WIFI W = Ctx->Wifi;

    if (W == NULL) {
        return;
    }
    BwlWifiReleaseHardware(Ctx);
    Ctx->Wifi = NULL;
    ExFreePoolWithTag(W, BWL_WIFI_TAG);
}

/* Legacy rates in 500 kbit/s units. */
static VOID
FillPhy(_Out_ WIFI_PHY_INFO *Info, _In_ WDI_PHY_TYPE Type, _In_reads_(Count) const UINT16 *Rates, _In_ ULONG Count)
{
    ULONG i;

    Info->PhyType = Type;
    Info->NumberDataRateEntries = Count;
    for (i = 0; i < Count; i++) {
        Info->DataRateList[i].DataRateFlag = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
        Info->DataRateList[i].DataRateValue = Rates[i];
    }
}

/* PrepareHardware, after the firmware and the station are up. */
extern "C" NTSTATUS
BwlWifiReportCapabilities(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    static const UINT16 cck[] = { 2, 4, 11, 22 };
    static const UINT16 erp[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    static const UINT16 ofdm[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    static const UCHAR ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                 132, 136, 140, 144, 149, 153, 157, 161, 165 };
    static UINT32 width20 = 20;
    static WDI_PHY_TYPE phys24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    static WDI_PHY_TYPE phys5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT };
    PBWL_WIFI W = Ctx->Wifi;
    const bwl_wlan *w = &Ctx->Bwl.Wlan;
    WIFI_DEVICE_CAPABILITIES dev;
    WIFI_STATION_CAPABILITIES sta;
    WIFI_BAND_CAPABILITIES bandCaps;
    WIFI_PHY_CAPABILITIES phyCaps;
    WIFI_BAND_INFO bands[2];
    WDI_CHANNEL_MAPPING_ENTRY chans24[13], chans5[sizeof(ch5)];
    DOT11_AUTH_CIPHER_PAIR unicast[2], mgmt[1];
    WIFI_PHY_INFO *phys;
    ULONG i, n;
    NTSTATUS status;
    KIRQL irql;

    if (W == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    KeAcquireSpinLock(&W->Lock, &irql);
    W->Accepting = TRUE;
    KeReleaseSpinLock(&W->Lock, irql);
    W->HwReady = FALSE;
    if (!Ctx->FwReady) {
        /* no firmware (a missing file, a failed step: FwState says which):
         * Windows gets no Wi-Fi device this start */
        BwlCrumb(Ctx, "wificx: firmware not running");
        return STATUS_DEVICE_NOT_READY;
    }

    /* the structures as the WDK's WiFiCx 1.0 header has them; the same
     * size as the 22621 WDK's (its 1.0 header has no _INIT functions) */
    RtlZeroMemory(&dev, sizeof(dev));
    dev.Size = sizeof(dev);
    dev.HardwareRadioState = TRUE;
    dev.SoftwareRadioState = W->RadioOn;
    for (i = 0; i < sizeof(dev.FirmwareVersion) - 1 && i < sizeof(Ctx->FwVer) && Ctx->FwVer[i]; i++) {
        dev.FirmwareVersion[i] = (UCHAR)Ctx->FwVer[i];
    }
    dev.NumRxStreams = 1;
    dev.NumTxStreams = 1;
    dev.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
    status = WifiDeviceSetDeviceCapabilities(Ctx->Device, &dev);
    BwlPutULong(Ctx, L"WifiDeviceCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* open, and WPA2-Personal with CCMP */
    unicast[0].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN;
    unicast[0].CipherAlgoId = DOT11_CIPHER_ALGO_NONE;
    unicast[1].AuthAlgoId = DOT11_AUTH_ALGO_RSNA_PSK;
    unicast[1].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;
    mgmt[0].AuthAlgoId = DOT11_AUTH_ALGO_80211_OPEN;
    mgmt[0].CipherAlgoId = DOT11_CIPHER_ALGO_NONE;
    RtlZeroMemory(&sta, sizeof(sta));
    sta.Size = sizeof(sta);
    sta.ScanSSIDListSize = 1;
    sta.DesiredSSIDListSize = 1;
    sta.PrivacyExemptionListSize = 8;
    sta.KeyMappingTableSize = 4;
    sta.DefaultKeyTableSize = 4;
    sta.WEPKeyValueMaxLength = 0;               /* no WEP */
    sta.MaxNumPerSTA = 4;
    sta.SupportedQOSFlags = 0;
    sta.HostFIPSModeImplemented = 0;
    sta.MFPCapable = 0;
    sta.AutoPowerSaveMode = FALSE;              /* power save is off (C_SET_PM 0) */
    sta.BSSListCachemanagement = FALSE;         /* Windows keeps the network list */
    sta.ConnectBSSSelectionOverride = FALSE;
    sta.NumSupportedUnicastAlgorithms = 2;
    sta.UnicastAlgorithmsList = unicast;
    sta.NumSupportedMulticastDataAlgorithms = 2;
    sta.MulticastDataAlgorithmsList = unicast;
    sta.NumSupportedMulticastMgmtAlgorithms = 1;
    sta.MulticastMgmtAlgorithmsList = mgmt;
    status = WifiDeviceSetStationCapabilities(Ctx->Device, &sta);
    BwlPutULong(Ctx, L"WifiStationCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* the channels the CLM normally allows; the firmware's country decides */
    for (i = 0; i < 13; i++) {
        chans24[i].ChannelNumber = i + 1;
        chans24[i].ChannelCenterFrequency = 2412 + 5 * i;
    }
    for (i = 0; i < sizeof(ch5); i++) {
        chans5[i].ChannelNumber = ch5[i];
        chans5[i].ChannelCenterFrequency = 5000 + 5u * ch5[i];
    }
    RtlZeroMemory(bands, sizeof(bands));
    bands[0].BandID = WDI_BAND_ID_2400;
    bands[0].BandState = TRUE;
    bands[0].NumValidPhyTypes = (ULONG)ARRAYSIZE(phys24);
    bands[0].ValidPhyTypeList = phys24;
    bands[0].NumValidChannelTypes = (ULONG)ARRAYSIZE(chans24);
    bands[0].ValidChannelTypes = chans24;
    bands[0].NumChannelWidths = 1;
    bands[0].ChannelWidthList = &width20;
    n = 1;
    if (w->Has5g) {
        bands[1].BandID = WDI_BAND_ID_5000;
        bands[1].BandState = TRUE;
        bands[1].NumValidPhyTypes = (ULONG)ARRAYSIZE(phys5);
        bands[1].ValidPhyTypeList = phys5;
        bands[1].NumValidChannelTypes = (ULONG)ARRAYSIZE(chans5);
        bands[1].ValidChannelTypes = chans5;
        bands[1].NumChannelWidths = 1;
        bands[1].ChannelWidthList = &width20;
        n = 2;
    }
    RtlZeroMemory(&bandCaps, sizeof(bandCaps));
    bandCaps.Size = sizeof(bandCaps);
    bandCaps.NumBands = n;
    bandCaps.BandInfoList = bands;
    status = WifiDeviceSetBandCapabilities(Ctx->Device, &bandCaps);
    BwlPutULong(Ctx, L"WifiBandCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* WIFI_PHY_INFO holds 126 rates each: too big for the stack */
    phys = (WIFI_PHY_INFO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 * sizeof(WIFI_PHY_INFO), BWL_WIFI_TAG);
    if (phys == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    FillPhy(&phys[0], WDI_PHY_TYPE_HRDSSS, cck, (ULONG)ARRAYSIZE(cck));
    FillPhy(&phys[1], WDI_PHY_TYPE_ERP, erp, (ULONG)ARRAYSIZE(erp));
    FillPhy(&phys[2], WDI_PHY_TYPE_HT, erp, (ULONG)ARRAYSIZE(erp));
    n = 3;
    if (w->Has5g) {
        FillPhy(&phys[3], WDI_PHY_TYPE_OFDM, ofdm, (ULONG)ARRAYSIZE(ofdm));
        n = 4;
    }
    RtlZeroMemory(&phyCaps, sizeof(phyCaps));
    phyCaps.Size = sizeof(phyCaps);
    phyCaps.NumPhyTypes = n;
    phyCaps.PhyInfoList = phys;
    status = WifiDeviceSetPhyCapabilities(Ctx->Device, &phyCaps);
    ExFreePoolWithTag(phys, BWL_WIFI_TAG);      /* WiFiCx keeps its own copy */
    BwlPutULong(Ctx, L"WifiPhyCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    W->HwReady = TRUE;
    BwlCrumb(Ctx, "wificx: capabilities reported");
    return STATUS_SUCCESS;
}

/* WiFiCx asks for the station port after PrepareHardware. PASSIVE_LEVEL. */
static NTSTATUS
BwlEvtWifiCreateAdapter(_In_ WDFDEVICE Device, _Inout_ NETADAPTER_INIT *AdapterInit)
{
    PBWL_DEVICE_CONTEXT ctx = BwlGetContext(Device);
    PBWL_WIFI W = ctx->Wifi;
    NTSTATUS status;

    if (W == NULL || WifiAdapterInitGetType(AdapterInit) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        return STATUS_NOT_SUPPORTED;            /* no Wi-Fi Direct roles */
    }
    if (!W->HwReady) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = BwlNetCreateAdapter(ctx, AdapterInit);
    if (NT_SUCCESS(status)) {
        status = WifiAdapterInitialize(ctx->Adapter);
    }
    if (NT_SUCCESS(status)) {
        status = BwlNetStart(ctx);              /* the chip's address, the queues; no link state */
    }
    BwlPutULong(ctx, L"AdapterStartStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    W->PortId = (UINT16)WifiAdapterGetPortId(ctx->Adapter);
    W->AdapterUp = TRUE;
    BwlPutULong(ctx, L"WdiPortId", W->PortId);
    BwlCrumb(ctx, "wificx: station adapter started");
    return STATUS_SUCCESS;
}

static NTSTATUS
BwlEvtWifiCreateWifiDirectDevice(_In_ WDFDEVICE Device, _Inout_ WIFIDIRECT_DEVICE_INIT *WifiDirectDeviceInit)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(WifiDirectDeviceInit);
    return STATUS_NOT_SUPPORTED;                /* no Wi-Fi Direct capabilities are reported */
}
