/*
 * wdicore.cpp - the WDI messages of the AIC8800 station, shared by the
 * WiFiCx front end (wificx.cpp, aic8800usb.sys, Windows 11) and the WDI
 * miniport (wdi.cpp, aic8800wdi.sys, Windows 10); see wdicore.h and
 * docs/dev/wificx.md.
 *
 * Each command is a WDI_MESSAGE_HEADER and TLVs, which Microsoft's TLV
 * library (WificxTLVGenParse.lib or TLVGeneratorParser.lib) parses and
 * generates at the version the OS speaks. A property message is done with
 * its completion (M3); a task gets its M3 when accepted and later its
 * completion indication (M4) with the same transaction ID, the M3 always
 * first.
 *
 * Threads: the front end (IRQL <= DISPATCH_LEVEL) only queues the request
 * (AicWifiQueue) and wakes the worker thread (driver.c), which runs
 * AicWifiRun: one request at a time, each completed exactly once, and in
 * between the core's receive path (scan results, association, disconnect)
 * and the task timeouts. Every call into the core is made from that one
 * thread.
 *
 * What runs where:
 *   - Windows keeps the network list (BSSListCachemanagement FALSE), so a
 *     scan sends up every beacon and probe response the firmware hands
 *     over (WDI_INDICATION_BSS_ENTRY_LIST, then SCAN_COMPLETE).
 *   - TASK_CONNECT: the first candidate the firmware can join (open, or
 *     WPA2-PSK with CCMP) gets SM_CONNECT with our RSN element; then
 *     ASSOCIATION_RESULT (with the association request body, whose RSN
 *     element Windows puts into message 2, and the AP's beacon body),
 *     LINK_STATE_CHANGE and CONNECT_COMPLETE.
 *   - Windows runs the 4-way handshake over the data path (EAPOL as
 *     ordinary 802.11 data frames) and hands the keys down with
 *     SET_ADD_CIPHER_KEYS; the pairwise key opens the firmware's port.
 *     The driver's own supplicant (aic_wpa.c) is not used in this mode.
 * Station only: no Wi-Fi Direct, no hotspot, no WPA3, no PMF, no roaming
 * offload, CCMP only (pairwise and group).
 *
 * The structure follows Microsoft's WiFiCx sample and what rtw89-win
 * (github.com/Meari-Prototype/rtw89-win, BSD-3-Clause) observed of WiFiCx
 * on hardware; its observed-behavior.md IDs are cited where a detail comes
 * from there. No code is copied from either.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "wdicore.h"

/* the core's cipher numbers are not WDI's: the front end maps them */
C_ASSERT(AIC_MAC_CIPHER_CCMP == 2);

/* ------------------------------------------- the TLV library's memory */

/* The TLV library (WificxTLVGenParse.lib or TLVGeneratorParser.lib)
 * allocates through these (arrayofelements.hpp);
 * AllocationContext is TLV_CONTEXT.AllocationContext, always 0 here. */
void * __cdecl
operator new(size_t Size, ULONG_PTR AllocationContext) throw()
{
    UNREFERENCED_PARAMETER(AllocationContext);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size != 0 ? Size : 1, AIC_WIFI_TAG);
}

void __cdecl
operator delete(void *P) noexcept
{
    if (P != NULL) {
        ExFreePoolWithTag(P, AIC_WIFI_TAG);
    }
}

void __cdecl
operator delete[](void *P) noexcept
{
    if (P != NULL) {
        ExFreePoolWithTag(P, AIC_WIFI_TAG);
    }
}

void __cdecl
operator delete(void *P, size_t Size) noexcept
{
    UNREFERENCED_PARAMETER(Size);
    if (P != NULL) {
        ExFreePoolWithTag(P, AIC_WIFI_TAG);
    }
}

void __cdecl
operator delete[](void *P, size_t Size) noexcept
{
    UNREFERENCED_PARAMETER(Size);
    if (P != NULL) {
        ExFreePoolWithTag(P, AIC_WIFI_TAG);
    }
}

/* ------------------------------------------------------------ helpers */

ULONG
AicWifiNowMs(VOID)
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

#define NowMs AicWifiNowMs

/* An indication: the WDI header, then the TLVs; the front end hands both
 * to the OS, which copies them during the call. */
VOID
AicWifiIndicate(_In_ PAIC_WIFI W, _In_ UINT16 MessageId, _In_ UINT16 PortId, _In_ UINT32 Tid, _In_ UINT32 Ihv,
                _In_ NDIS_STATUS Status, _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    WDI_MESSAGE_HEADER h;

    RtlZeroMemory(&h, sizeof(h));
    h.PortId = PortId;
    h.Status = Status;
    h.TransactionId = Tid;
    h.IhvSpecificId = Ihv;
    if (AicFeIndicate(W, MessageId, &h, Tlv, Tlv != NULL ? TlvLen : 0)) {
        W->Indications++;
    } else {
        W->IndicationErrors++;
    }
}

#define Indicate AicWifiIndicate

/* M3 of a message: just the header. */
static VOID
Complete(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ NTSTATUS Status)
{
    AicFeReqComplete(W, Request, Status, sizeof(WDI_MESSAGE_HEADER));
}

/* A task's completion indication, header only. */
static VOID
TaskDone(_In_ PAIC_WIFI W, _Inout_ AIC_WIFI_TASK *T, _In_ UINT16 MessageId, _In_ NDIS_STATUS Status)
{
    if (!T->Active) {
        return;
    }
    T->Active = FALSE;
    Indicate(W, MessageId, T->PortId, T->Tid, T->Ihv, Status, NULL, 0);
}

static VOID
TaskStart(_Out_ AIC_WIFI_TASK *T, _In_ const WDI_MESSAGE_HEADER *H)
{
    T->Active = TRUE;
    T->Aborted = FALSE;
    T->PortId = H->PortId;
    T->Tid = H->TransactionId;
    T->Ihv = H->IhvSpecificId;
    T->StartMs = NowMs();
}

/* ------------------------------------------------------------- records */

static VOID
PutRecords(_In_ PAIC_WIFI W)
{
    PAIC_DEVICE_CONTEXT ctx = W->Ctx;
    CHAR text[AIC_WIFI_TRACE * 5 + 1];
    PCHAR p = text;
    size_t left = sizeof(text);
    ULONG i, n = W->TraceCount < AIC_WIFI_TRACE ? W->TraceCount : AIC_WIFI_TRACE;

    text[0] = 0;
    for (i = 0; i < n; i++) {
        (VOID)RtlStringCchPrintfExA(p, left, &p, &left, 0, "%u ", (unsigned)W->Trace[i]);
    }
    AicPutString(ctx, L"WdiTrace", text);
    AicPutULong(ctx, L"WdiMessages", W->Msgs);
    AicPutULong(ctx, L"WdiUnsupported", W->Unsupported);
    AicPutULong(ctx, L"WdiRefused", W->Refused);
    AicPutULong(ctx, L"WdiIndications", W->Indications);
    AicPutULong(ctx, L"WdiIndicationErrors", W->IndicationErrors);
    AicPutULong(ctx, L"WdiScanTasks", W->ScanTasks);
    AicPutULong(ctx, L"WdiBssIndicated", W->BssIndicated);
    AicPutULong(ctx, L"WdiConnectTasks", W->ConnectTasks);
    AicPutULong(ctx, L"WdiLastConnectStatus", W->LastConnectStatus);
    AicPutULong(ctx, L"WdiKeys", W->Keys);
    AicPutULong(ctx, L"WdiKeyErrors", W->KeyErrors);
    AicPutULong(ctx, L"WdiLastKeyStatus", W->LastKeyStatus);
    AicPutULong(ctx, L"KeyEapolWaitMs", W->KeyEapolWaitMs);
    AicPutULong(ctx, L"WdiDisassociations", W->Disassociations);
    AicPutULong(ctx, L"WdiRadioOn", W->RadioOn);
    AicPutULong(ctx, L"WdiConnected", W->Connected);
    AicPutStats(ctx);
}

/* ---------------------------------------------------------------- scan */

/* ops->scan_bss, on the worker thread inside the receive path: keep the
 * body for the next BSS_ENTRY_LIST (one entry per BSSID, the latest body). */
extern "C" VOID
AicWifiScanBss(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ const aic_bss *Bss, _In_reads_(Len) const UCHAR *Body,
               _In_ ULONG Len, _In_ int ProbeResp)
{
    PAIC_WIFI W = Ctx->Wifi;
    AIC_WIFI_BSS *e = NULL;
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
    if (e != NULL && e->Len >= Len) {
        e->Probe = (BOOLEAN)(ProbeResp != 0);    /* a newer body fits in the old place */
    } else {
        if (W->BodyUsed + Len > sizeof(W->Body)) {
            return;                             /* full: the rest go in the next scan */
        }
        if (e == NULL) {
            if (W->BssCount >= AIC_WIFI_BSS_MAX) {
                return;
            }
            e = &W->Bss[W->BssCount++];
            RtlCopyMemory(e->Bssid, Bss->Bssid, 6);
        }
        e->Off = W->BodyUsed;
        W->BodyUsed += Len;
        e->Probe = (BOOLEAN)(ProbeResp != 0);
    }
    RtlCopyMemory(W->Body + e->Off, Body, Len);
    e->Len = (USHORT)Len;
    e->Channel = Bss->Channel;
    e->Band = Bss->Band;
    e->Rssi = Bss->Rssi;
}

/* The entries collected and not yet sent, AIC_WIFI_BSS_PER_IND at a time
 * (unsolicited: TransactionId 0; the bodies from the Timestamp on, SCAN-3). */
static VOID
IndicateBss(_In_ PAIC_WIFI W)
{
    while (W->BssSent < W->BssCount) {
        WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS P;
        ULONG n = W->BssCount - W->BssSent, i;
        UINT8 *tlv = NULL;
        ULONG tlvLen = 0;

        if (n > AIC_WIFI_BSS_PER_IND) {
            n = AIC_WIFI_BSS_PER_IND;
        }
        if (P.DeviceDescriptor.AllocateElements(n, 0) != NDIS_STATUS_SUCCESS) {
            W->IndicationErrors++;
            return;
        }
        P.Optional.DeviceDescriptor_IsPresent = 1;
        for (i = 0; i < n; i++) {
            const AIC_WIFI_BSS *b = &W->Bss[W->BssSent + i];
            WDI_BSS_ENTRY_CONTAINER *e = &P.DeviceDescriptor.pElements[i];

            RtlCopyMemory(e->BSSID.Address, b->Bssid, 6);
            if (b->Probe) {
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
 * aborted, with NDIS_STATUS_REQUEST_ABORTED; SCAN-5). */
static VOID
ScanFinish(_In_ PAIC_WIFI W, _In_ NDIS_STATUS Status)
{
    if (!W->Scan.Active) {
        return;
    }
    IndicateBss(W);
    TaskDone(W, &W->Scan, WDI_INDICATION_SCAN_COMPLETE, Status);
}

/* The firmware takes one scan at a time: wait for one still running. */
static BOOLEAN
WaitFwScan(_In_ PAIC_WIFI W)
{
    aic_dev *d = &W->Ctx->Aic;

    while (W->FwScanning && !(d->Events & AIC_EV_SCAN_DONE) && !W->Ctx->Stopping) {
        if (NowMs() - W->FwScanStartMs > AIC_WIFI_FW_SCAN_MS) {
            break;
        }
        AicUsbOps.wait(W->Ctx, 50);
    }
    W->FwScanning = FALSE;
    d->Events &= (u8)~AIC_EV_SCAN_DONE;
    return !W->Ctx->Stopping;
}

static VOID
TaskScan(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H)
{
    aic_dev *d = &W->Ctx->Aic;
    int rc;

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
    rc = aic_scan_start(d);
    if (rc != AIC_OK) {
        W->Scan.Active = FALSE;
        Complete(W, Request, STATUS_UNSUCCESSFUL);      /* no M4 after a failed M3 */
        return;
    }
    W->FwScanning = TRUE;
    W->FwScanStartMs = NowMs();
    Complete(W, Request, STATUS_SUCCESS);               /* M3; M4 when the firmware is done */
}

/* ------------------------------------------------------------- connect */

static VOID
IndicateAssociation(_In_ PAIC_WIFI W, _In_ BOOLEAN Success, _In_ const UCHAR *Bssid)
{
    aic_dev *d = &W->Ctx->Aic;
    WDI_INDICATION_ASSOCIATION_RESULT_LIST P;
    WDI_ASSOCIATION_RESULT_CONTAINER *e;
    WDI_PHY_TYPE phy;
    UCHAR req[AIC_ASSOC_IE_MAX + 8], rsp[AIC_ASSOC_IE_MAX + 8];
    ULONG reqLen = 0, rspLen = 0;
    BOOLEAN rsn = d->AssocRsnIeLen != 0;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    if (P.AssociationResults.AllocateElements(1, 0) != NDIS_STATUS_SUCCESS) {
        W->IndicationErrors++;
        return;
    }
    e = &P.AssociationResults.pElements[0];
    RtlCopyMemory(e->BSSID.Address, Bssid, 6);
    e->AssociationResultParameters.AssociationStatus =
        Success ? WDI_ASSOC_STATUS_SUCCESS : WDI_ASSOC_STATUS_ASSOC_FAILED_BY_PEER;
    e->AssociationResultParameters.StatusCode = Success ? 0 : d->ConnectStatus;
    e->AssociationResultParameters.ReAssociation = FALSE;
    e->AssociationResultParameters.AuthAlgorithm = rsn ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
    e->AssociationResultParameters.UnicastCipherAlgorithm = rsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    e->AssociationResultParameters.MulticastDataCipherAlgorithm = rsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    e->AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    /* Windows authorizes the port after its handshake (CONN-5) */
    e->AssociationResultParameters.PortAuthorized = FALSE;
    e->AssociationResultParameters.WMMQoSEnabled = d->ApQos != 0;
    e->AssociationResultParameters.DSInfo = WDI_DS_UNKNOWN;
    e->AssociationResultParameters.BandID = WdiBand(d->LinkFreq != 0 && d->LinkFreq < 3000 ? 0 : 1);
    e->AssociationResultParameters.IHVAssociationStatus = Success ? 0 : d->ConnectStatus;
    if (Success) {
        /* the request's RSN element goes into message 2, the beacon's is
         * checked against message 3 (CONN-5) */
        reqLen = aic_assoc_req_body(d, req, (u32)sizeof(req));
        rspLen = aic_assoc_rsp_body(d, rsp, (u32)sizeof(rsp));
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
    /* HT20 is what ME_CONFIG sets up */
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

static VOID
IndicateLinkState(_In_ PAIC_WIFI W)
{
    aic_dev *d = &W->Ctx->Aic;
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS P;
#ifndef AIC_WDI
    WDI_LINK_INFO_CONTAINER link;
#else
    struct { UINT32 LinkID; WDI_MAC_ADDRESS LocalLinkMACAddress, PeerLinkMACAddress; UINT32 ChannelNumber;
             WDI_BAND_ID BandId; INT32 RSSI; UINT32 Bandwidth; } link;
#endif
    int i = d->CurBss < d->BssCount ? d->CurBss : -1;
    LONG rssi = i >= 0 ? d->Bss[i].Rssi : -60;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    RtlCopyMemory(P.LinkStateChangeParameters.PeerMACAddress.Address, d->Bssid, 6);
    P.LinkStateChangeParameters.TxLinkSpeed = 65000;    /* kbit/s: HT20 MCS 7, one stream */
    P.LinkStateChangeParameters.RxLinkSpeed = 65000;
    P.LinkStateChangeParameters.LinkQuality = (UINT8)LinkQuality(rssi);
    RtlZeroMemory(&link, sizeof(link));
    link.LinkID = 0;
    RtlCopyMemory(link.LocalLinkMACAddress.Address, d->Mac, 6);
    RtlCopyMemory(link.PeerLinkMACAddress.Address, d->Bssid, 6);
    link.ChannelNumber = i >= 0 ? d->Bss[i].Channel : 0;
    link.BandId = WdiBand(d->LinkFreq != 0 && d->LinkFreq < 3000 ? 0 : 1);
    link.RSSI = rssi;
    link.Bandwidth = 20;
#ifndef AIC_WDI
    P.LinkInfo.SimpleAssign(&link, 1);  /* generated only for peers that know it */
#else
    UNREFERENCED_PARAMETER(link);       /* WDI 1.x has no per-link list */
#endif
    if (GenerateWdiIndicationLinkStateChangeFromIhv(&P, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
        W->IndicationErrors++;
        return;
    }
    Indicate(W, WDI_INDICATION_LINK_STATE_CHANGE, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT, W->Conn.Ihv,
             NDIS_STATUS_SUCCESS, tlv, tlvLen);
    FreeGenerated(tlv);
}

static VOID
IndicateDisassociation(_In_ PAIC_WIFI W, _In_ WDI_ASSOC_STATUS Reason)
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

/* Leave the network (keys go with the station entry in the firmware). */
static VOID
Leave(_In_ PAIC_WIFI W, _In_ WDI_ASSOC_STATUS Reason)
{
    aic_dev *d = &W->Ctx->Aic;

    W->Ctx->RxHold = TRUE;
    if (d->Link != AIC_LINK_DOWN) {
        ULONG i;

        (VOID)aic_disconnect(d);
        /* the firmware answers with SM_DISCONNECT_IND too: take it now, or
         * it would end the next connection when it comes */
        for (i = 0; i < 10 && !(d->Events & AIC_EV_DISCONNECT) && !W->Ctx->Stopping; i++) {
            AicUsbOps.wait(W->Ctx, 20);
        }
        d->Link = AIC_LINK_DOWN;
    }
    d->Events &= (u8)~(AIC_EV_ASSOC | AIC_EV_DISCONNECT);
    IndicateDisassociation(W, Reason);
}

static VOID
ConnectFinish(_In_ PAIC_WIFI W, _In_ NDIS_STATUS Status)
{
    W->LastConnectStatus = (ULONG)Status;
    TaskDone(W, &W->Conn, WDI_INDICATION_CONNECT_COMPLETE, Status);
}

/* The candidate's security as the core sees it fits what Windows asks for. */
static BOOLEAN
Acceptable(_In_ const aic_bss *B, _In_ BOOLEAN Open, _In_ BOOLEAN Psk)
{
    return (Open && B->Security == AIC_SEC_OPEN) || (Psk && B->Security == AIC_SEC_WPA2_PSK);
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

static VOID
TaskConnect(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
{
    aic_dev *d = &W->Ctx->Aic;
    WDI_TASK_CONNECT_PARAMETERS P;
    const WDI_CONNECT_PARAMETERS_CONTAINER *cp;
    BOOLEAN open = FALSE, psk = FALSE, ccmp = FALSE, none = FALSE;
    int idx = -1;
    UINT32 i;
    NDIS_STATUS parsed;

    parsed = ParseWdiTaskConnectToIhv((ULONG)(InLen - sizeof(*H)), (const UINT8 *)(H + 1), &W->Tlv, &P);
    if (parsed != NDIS_STATUS_SUCCESS) {
        CleanupParsedWdiTaskConnectToIhv(&P);
        AicFeReqComplete(W, Request, NdisToNt(parsed), 0);
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
    for (i = 0; (open || psk) && idx < 0 && i < P.PreferredBSSEntryList.ElementCount; i++) {
        const WDI_CONNECT_BSS_ENTRY_CONTAINER *b = &P.PreferredBSSEntryList.pElements[i];
        const UINT8 *body = NULL;
        UINT32 len = 0, ch = b->ChannelInfo.ChannelNumber;
        USHORT freq;
        int k;

        if (b->Optional.ProbeResponseFrame_IsPresent && b->ProbeResponseFrame.ElementCount != 0) {
            body = b->ProbeResponseFrame.pElements;
            len = b->ProbeResponseFrame.ElementCount;
        } else if (b->Optional.BeaconFrame_IsPresent && b->BeaconFrame.ElementCount != 0) {
            body = b->BeaconFrame.pElements;
            len = b->BeaconFrame.ElementCount;
        }
        if (body == NULL || len > AIC_WIFI_CONN_BODY || ch == 0 || ch > 196) {
            continue;
        }
        /* Wlansvc names the broadcast address as "any" (CONN-2) */
        if (cp->Optional.AllowedBSSIDs_IsPresent && cp->AllowedBSSIDs.ElementCount != 0 &&
            !Listed(&cp->AllowedBSSIDs, b->BSSID.Address, TRUE)) {
            continue;
        }
        if (cp->Optional.DisallowedBSSIDs_IsPresent && Listed(&cp->DisallowedBSSIDs, b->BSSID.Address, FALSE)) {
            continue;
        }
        if (b->ChannelInfo.BandId == WDI_BAND_ID_2400) {
            freq = (USHORT)(ch == 14 ? 2484 : 2407 + 5 * ch);
        } else if (b->ChannelInfo.BandId == WDI_BAND_ID_5000 && d->Is5gSupported) {
            freq = (USHORT)(5000 + 5 * ch);
        } else {
            continue;
        }
        /* the network as Windows hands it back (our own list may have been
         * emptied by a scan since) */
        k = aic_bss_from_body(d, b->BSSID.Address, freq, (s8)b->SignalInfo.RSSI, body, len);
        if (k < 0 || !Acceptable(&d->Bss[k], open, psk)) {
            continue;
        }
        idx = k;
        RtlCopyMemory(W->ConnBody, body, len);
        W->ConnBodyLen = (USHORT)len;
    }
    CleanupParsedWdiTaskConnectToIhv(&P);

    if (!W->HwReady || !W->RadioOn || W->Conn.Active) {
        Complete(W, Request, W->Conn.Active ? STATUS_DEVICE_BUSY : STATUS_DEVICE_NOT_READY);
        return;
    }
    if (idx < 0) {
        /* nothing this driver can join: WPA3, 802.1X, TKIP-only, WEP */
        Complete(W, Request, STATUS_NOT_SUPPORTED);
        return;
    }
    W->ConnectTasks++;
    /* a scan in the firmware would hold the connect back */
    if (W->Scan.Active) {
        W->Scan.Aborted = TRUE;
        ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    if (!WaitFwScan(W)) {
        Complete(W, Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    /* a roam (RoamRequest) or a new network: leave the old one first */
    if (d->Link != AIC_LINK_DOWN || W->Connected) {
        Leave(W, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
    }
    d->HostAuth = 1;
    W->Ctx->RxHold = TRUE;
    TaskStart(&W->Conn, H);
    W->EapolRx0 = d->Stats.RxEapol;     /* an earlier attempt's unanswered message 1 does not count */
    W->EapolTx0 = d->Stats.TxEapol;
    RtlCopyMemory(W->ConnBssid, d->Bss[idx].Bssid, 6);
    if (aic_connect_start(d, idx) != AIC_OK) {
        W->Conn.Active = FALSE;
        W->LastConnectStatus = (ULONG)STATUS_UNSUCCESSFUL;
        Complete(W, Request, STATUS_UNSUCCESSFUL);      /* no M4 after a failed M3 */
        return;
    }
    Complete(W, Request, STATUS_SUCCESS);               /* M3; the rest from ConnEvents */
}

/* Association, disconnect and the connect timeout, between requests. */
static VOID
ConnEvents(_In_ PAIC_WIFI W)
{
    aic_dev *d = &W->Ctx->Aic;

    if (W->Conn.Active && (d->Events & AIC_EV_ASSOC)) {
        d->Events &= (u8)~AIC_EV_ASSOC;
        if (d->ConnectStatus == 0 && d->Link == AIC_LINK_ASSOCIATED) {
            RtlCopyMemory(W->ConnBssid, d->Bssid, 6);
            IndicateAssociation(W, TRUE, d->Bssid);
            W->Connected = TRUE;
            IndicateLinkState(W);
            ConnectFinish(W, NDIS_STATUS_SUCCESS);
            /* message 1 may be waiting already */
            AicNetRxRelease(W->Ctx);
        } else {
            IndicateAssociation(W, FALSE, W->ConnBssid);
            ConnectFinish(W, NDIS_STATUS_FAILURE);
        }
    }
    if (W->Conn.Active && NowMs() - W->Conn.StartMs > W->ConnTimeoutMs) {
        Leave(W, WDI_ASSOC_STATUS_ASSOC_EXCHANGE_FAILURE);
        IndicateAssociation(W, FALSE, W->ConnBssid);
        ConnectFinish(W, NDIS_STATUS_FAILURE);
    }
    if (d->Events & AIC_EV_DISCONNECT) {
        d->Events &= (u8)~AIC_EV_DISCONNECT;
        W->Ctx->RxHold = TRUE;
        if (W->Conn.Active) {
            IndicateAssociation(W, FALSE, W->ConnBssid);
            ConnectFinish(W, NDIS_STATUS_FAILURE);
        }
        /* Wlansvc reconnects (TASK_CONNECT with RoamRequest, CONN-7) */
        IndicateDisassociation(W, d->DisconnectReason == 0 ? WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN
                                                           : WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED);
    }
}

static VOID
TaskDisconnect(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H)
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
SetAddCipherKeys(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
{
    PAIC_DEVICE_CONTEXT ctx = W->Ctx;
    aic_dev *d = &ctx->Aic;
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
        if (pairwise) {
            /* Message 4 must leave unencrypted before the key is in.
             * Windows sent it just before this, but it may still be in
             * NetAdapterCx's TX ring, not yet sent on the data pipe (the TX
             * queue runs on its own thread). Each EAPOL frame from the AP
             * (M1, M3, a group key message) gets one answer, so wait until
             * as many went out as came in, taking the receive path
             * meanwhile; then for the USB writes to finish. */
            ULONG start = NowMs(), waited = 0;

            while (aic_eapol_owed(d, W->EapolRx0, W->EapolTx0) != 0 && !ctx->Stopping) {
                waited = NowMs() - start;
                if (waited >= AIC_WIFI_M4_WAIT_MS) {
                    break;
                }
                AicUsbOps.wait(ctx, 2);
            }
            W->KeyEapolWaitMs = NowMs() - start;
            waited = 0;
            while (!AicNetTxIdle(ctx) && waited < 50) {
                AicUsbOps.delay_ms(ctx, 5);
                waited += 5;
            }
            AicUsbOps.delay_ms(ctx, 10);
            /* a later rekey counts from here */
            W->EapolRx0 = d->Stats.RxEapol;
            W->EapolTx0 = d->Stats.TxEapol;
        }
        rc = aic_host_set_key(d, pairwise, id, k->CCMPKey.pElements, 16, AIC_MAC_CIPHER_CCMP);
        W->Keys++;
        if (rc != AIC_OK) {
            W->KeyErrors++;
            status = rc == AIC_E_STATE ? STATUS_INVALID_DEVICE_STATE : STATUS_UNSUCCESSFUL;
        }
    }
    CleanupParsedWdiSetAddCipherKeysToIhv(&P);
    W->LastKeyStatus = (ULONG)status;
    Complete(W, Request, status);
}

/* ------------------------------------------------- radio, reset, abort */

static VOID
TaskSetRadioState(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
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
        AicFeReqComplete(W, Request, NdisToNt(parsed), 0);
        return;
    }
    Complete(W, Request, STATUS_SUCCESS);               /* M3 */
    if (!on) {
        /* the firmware has no radio switch here: off means not connected
         * and no scans */
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
TaskDot11Reset(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H)
{
    WDI_MESSAGE_HEADER h = *H;

    /* The parameters (default MIB, a MAC address) change nothing here:
     * the address stays the module's, and the exemption list is not kept
     * (the firmware lets EAPOL through itself until the port opens). */
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
AbortTask(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ const WDI_MESSAGE_HEADER *H, _In_ UINT InLen)
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
    /* the bare message ID, or the OID form (SCAN-4) */
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
Dispatch(_In_ PAIC_WIFI W, _In_ AIC_REQ Request)
{
    UINT inLen = 0, outLen = 0;
    PWDI_MESSAGE_HEADER h = AicFeReqBuffer(Request, &inLen, &outLen);
    UINT16 id = AicFeReqId(Request);

    W->Msgs++;
    if (W->TraceCount < AIC_WIFI_TRACE) {
        W->Trace[W->TraceCount] = id;
    }
    W->TraceCount++;
    if (h == NULL || inLen < sizeof(WDI_MESSAGE_HEADER)) {
        AicFeReqComplete(W, Request, STATUS_INVALID_PARAMETER, 0);
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
        /* Accepted, nothing to do: the address stays the module's; the
         * firmware lets EAPOL through until the port opens and encrypts
         * everything after; keys go with the station entry on leaving;
         * every group frame goes up and Windows filters; the OS keeps the
         * network list. */
        Complete(W, Request, STATUS_SUCCESS);
        break;
    default:
        /* the front end's own messages (WDI: the adapter's capabilities,
         * the ports); GET_STATISTICS, SET_LOCATION_PRIVACY and the rest:
         * not needed for a station (SCAN-1) */
        if (AicFeDispatch(W, Request, id, h, inLen, outLen)) {
            break;
        }
        W->Unsupported++;
        Complete(W, Request, STATUS_NOT_SUPPORTED);
        break;
    }
}

BOOLEAN
AicWifiQueue(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _Out_ NTSTATUS *Fail)
{
    NTSTATUS fail = STATUS_SUCCESS;
    KIRQL irql;

    KeAcquireSpinLock(&W->Lock, &irql);
    if (!W->Accepting) {
        fail = STATUS_DEVICE_NOT_READY;
    } else if (W->RingCount == AIC_WIFI_RING) {
        fail = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        W->Ring[(W->RingHead + W->RingCount) % AIC_WIFI_RING] = Request;
        W->RingCount++;
    }
    KeReleaseSpinLock(&W->Lock, irql);
    *Fail = fail;
    if (!NT_SUCCESS(fail)) {
        InterlockedIncrement((volatile LONG *)&W->Refused);
        return FALSE;
    }
    KeSetEvent(&W->Ctx->RxEvent, IO_NO_INCREMENT, FALSE);
    return TRUE;
}

/* DeviceAdd (WiFiCx) or AllocateAdapter (WDI): the state, radio on, not
 * yet taking requests. */
PAIC_WIFI
AicWifiAlloc(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_WIFI W = (PAIC_WIFI)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(AIC_WIFI), AIC_WIFI_TAG);

    if (W == NULL) {
        return NULL;
    }
    W->Ctx = Ctx;
    KeInitializeSpinLock(&W->Lock);
    W->RadioOn = TRUE;
    Ctx->Wifi = W;
    Ctx->RxHold = TRUE;
    return W;
}

/* Start taking requests. */
VOID
AicWifiAccept(_In_ PAIC_WIFI W)
{
    KIRQL irql;

    KeAcquireSpinLock(&W->Lock, &irql);
    W->Accepting = TRUE;
    KeReleaseSpinLock(&W->Lock, irql);
}

static AIC_REQ
Pop(_In_ PAIC_WIFI W)
{
    AIC_REQ r = NULL;
    KIRQL irql;

    KeAcquireSpinLock(&W->Lock, &irql);
    if (W->RingCount != 0) {
        r = W->Ring[W->RingHead];
        W->RingHead = (W->RingHead + 1) % AIC_WIFI_RING;
        W->RingCount--;
    }
    KeReleaseSpinLock(&W->Lock, irql);
    return r;
}

/* The worker thread (driver.c RuntimeStage), until the device stops. */
extern "C" VOID
AicWifiRun(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_WIFI W = Ctx->Wifi;
    aic_dev *d = &Ctx->Aic;

    if (W == NULL) {
        return;
    }
    /* the front end's start on this thread (WDI: the bring-up and the
     * open's completion) */
    AicFeWorkerStart(W);
    W->ConnTimeoutMs = AicGetULong(Ctx, L"ConnectTimeoutMs", 15000);
    AicCrumb(Ctx, AIC_FE_NAME ": running");
    while (!Ctx->Stopping) {
        AIC_REQ r = Pop(W);

        if (r != NULL) {
            Dispatch(W, r);
        } else {
            AicUsbOps.wait(Ctx, 50);    /* the receive path; a new request wakes it */
        }
        if (W->FwScanning && (d->Events & AIC_EV_SCAN_DONE)) {
            d->Events &= (u8)~AIC_EV_SCAN_DONE;
            W->FwScanning = FALSE;
            ScanFinish(W, d->ScanStatus == 0 ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
        }
        if (W->Scan.Active && NowMs() - W->Scan.StartMs > AIC_WIFI_SCAN_MS) {
            W->Scan.Aborted = TRUE;
            ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
        }
        ConnEvents(W);
        AicFeService(W);
        if (NowMs() - W->LastRecordMs > 5000) {
            W->LastRecordMs = NowMs();
            PutRecords(W);
        }
    }
    /* stopping (suspend, removal): running tasks end; queued requests are
     * failed by AicWifiPause (suspend) or ReleaseHardware */
    if (W->Scan.Active) {
        W->Scan.Aborted = TRUE;
        ScanFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    if (W->Conn.Active) {
        ConnectFinish(W, NDIS_STATUS_REQUEST_ABORTED);
    }
    Leave(W, WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET);
    W->FwScanning = FALSE;
    PutRecords(W);
    AicCrumb(Ctx, AIC_FE_NAME ": stopped");
}

/* The worker has stopped (or never started): nothing more is taken, and
 * what waits is failed, so that nobody waits for it. */
extern "C" VOID
AicWifiPause(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_WIFI W = Ctx->Wifi;
    AIC_REQ r;
    KIRQL irql;

    if (W == NULL) {
        return;
    }
    KeAcquireSpinLock(&W->Lock, &irql);
    W->Accepting = FALSE;
    KeReleaseSpinLock(&W->Lock, irql);
    while ((r = Pop(W)) != NULL) {
        AicFeReqComplete(W, r, STATUS_DEVICE_NOT_READY, sizeof(WDI_MESSAGE_HEADER));
    }
}

/* SelfManagedIoRestart, before the worker starts again. */
extern "C" VOID
AicWifiResume(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_WIFI W = Ctx->Wifi;
    KIRQL irql;

    if (W == NULL) {
        return;
    }
    KeAcquireSpinLock(&W->Lock, &irql);
    W->Accepting = W->HwReady;
    KeReleaseSpinLock(&W->Lock, irql);
}

/* ReleaseHardware (the worker has stopped). */
extern "C" VOID
AicWifiReleaseHardware(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_WIFI W = Ctx->Wifi;

    if (W == NULL) {
        return;
    }
    AicWifiPause(Ctx);
    W->HwReady = FALSE;
    W->Connected = FALSE;
}

extern "C" VOID
AicWifiCleanup(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_WIFI W = Ctx->Wifi;

    if (W == NULL) {
        return;
    }
    AicWifiReleaseHardware(Ctx);
    Ctx->Wifi = NULL;
    ExFreePoolWithTag(W, AIC_WIFI_TAG);
}
