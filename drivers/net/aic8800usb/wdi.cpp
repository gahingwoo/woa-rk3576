/*
 * wdi.cpp - aic8800wdi.sys: the AIC8800D80 USB Wi-Fi of the Radxa ROCK 4D
 * as a WDI miniport for Windows 10 2004..22H2 (build 19041..19045), which
 * has no WiFiCx. Installed by aic8800wdi.inf on the WLAN interface of the
 * running firmware (USB\VID_A69C&PID_8D81&MI_02); the ROM loader face
 * (PID_8D80) is the same image as service aic8800ldr, which driver.c runs
 * as a KMDF function driver as on Windows 11. See docs/dev/aic8800.md (Windows 10).
 *
 * WDI is an NDIS 6.50 miniport whose MiniportInitializeEx, MiniportHaltEx
 * and data path Microsoft's WDI layer owns: the driver gets WDI messages as
 * OIDs (OID_WDI_*: M1; completing the OID is M3), sends task completions and
 * events as status indications (NDIS_STATUS_WDI_INDICATION_*: M4), and moves
 * frames through the TAL handlers (wdital.c). The messages are the same as
 * under WiFiCx, so scan, connect, keys, radio, reset and abort are
 * wdicore.cpp, shared with aic8800usb.sys; this file adds what only WDI has:
 * the adapter's open and close, its capabilities as a TLV reply
 * (WDI_GET_ADAPTER_CAPABILITIES), and the station port
 * (WDI_TASK_CREATE_PORT / DELETE_PORT). bcmwlsdio's wdi.cpp is the same
 * front end over SDIO.
 *
 * KMDF 1.31 in miniport mode (WdfDeviceMiniportCreate): no WDF PnP, but the
 * registry, the USB target (WdfUsbTargetDeviceCreateWithParameters on the
 * miniport's WDFDEVICE), the requests and the device context the core
 * uses.
 *
 * Threads: MiniportOidRequest (IRQL <= DISPATCH_LEVEL) queues the request
 * and returns NDIS_STATUS_PENDING; the worker thread (driver.c) opens the
 * USB device and initialises the firmware when the adapter is opened,
 * completes the open, and from then on runs every request and the core's
 * receive path.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "wdicore.h"

#define AIC_WDI_TAG 'd8cA'

/* MaxCommandSize in the capabilities: a TASK_CONNECT carries whole beacons */
#define AIC_WDI_MAX_COMMAND 0x10000

typedef struct _AIC_FE {
    NDIS_HANDLE Adapter;                        /* MiniportAdapterHandle */
    NDIS_WDI_OPEN_ADAPTER_COMPLETE_HANDLER OpenComplete;
    NDIS_WDI_CLOSE_ADAPTER_COMPLETE_HANDLER CloseComplete;
    volatile LONG OpenPending;                  /* the worker owes the open's completion */
    BOOLEAN PortCreated;
} AIC_FE, *PAIC_FE;

static NDIS_HANDLE g_Driver;
static WCHAR g_Params[256];                     /* Services\aic8800usb\Parameters */

static MINIPORT_UNLOAD AicWdiUnload;
static MINIPORT_OID_REQUEST AicWdiOidRequest;
static MINIPORT_DEVICE_PNP_EVENT_NOTIFY AicWdiPnpEvent;
static MINIPORT_SHUTDOWN AicWdiShutdown;
static MINIPORT_WDI_ALLOCATE_ADAPTER AicWdiAllocateAdapter;
static MINIPORT_WDI_FREE_ADAPTER AicWdiFreeAdapter;
static MINIPORT_WDI_OPEN_ADAPTER AicWdiOpenAdapter;
static MINIPORT_WDI_CLOSE_ADAPTER AicWdiCloseAdapter;
static MINIPORT_WDI_ADAPTER_HANG_DIAGNOSE AicWdiHangDiagnose;

/* wdital.c */
EXTERN_C MINIPORT_WDI_TAL_TXRX_INITIALIZE AicTalInitialize;
EXTERN_C MINIPORT_WDI_TAL_TXRX_DEINITIALIZE AicTalDeinitialize;
EXTERN_C NTSTATUS AicTalAlloc(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ NDIS_HANDLE Adapter);
EXTERN_C VOID AicTalService(_In_ PAIC_DEVICE_CONTEXT Ctx);

/* ------------------------------------------------------------ records */

/* Before a device exists: one value in Services\aic8800usb\Parameters, flushed. */
static VOID
ParamCrumb(_In_ PCWSTR Name, _In_opt_ PCWSTR Text, _In_ ULONG Value)
{
    UNICODE_STRING p, n;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;

    if (g_Params[0] == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitUnicodeString(&p, g_Params);
    InitializeObjectAttributes(&oa, &p, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        return;
    }
    RtlInitUnicodeString(&n, Name);
    if (Text != NULL) {
        (VOID)ZwSetValueKey(key, &n, 0, REG_SZ, (PVOID)Text, (ULONG)(wcslen(Text) + 1) * sizeof(WCHAR));
    } else {
        (VOID)ZwSetValueKey(key, &n, 0, REG_DWORD, &Value, sizeof(Value));
    }
    (VOID)ZwFlushKey(key);
    ZwClose(key);
}

static ULONG
ParamGet(_In_ PCWSTR Name, _In_ ULONG Default)
{
    UNICODE_STRING p, n;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    ULONG buf[(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG) + 3) / 4];   /* aligned */
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len = 0, r = Default;

    RtlInitUnicodeString(&p, g_Params);
    InitializeObjectAttributes(&oa, &p, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        RtlInitUnicodeString(&n, Name);
        if (NT_SUCCESS(ZwQueryValueKey(key, &n, KeyValuePartialInformation, buf, sizeof(buf), &len)) &&
            v->Type == REG_DWORD && v->DataLength == sizeof(ULONG)) {
            /* byte by byte: Data is at offset 12 of the buffer */
            r = (ULONG)v->Data[0] | ((ULONG)v->Data[1] << 8) | ((ULONG)v->Data[2] << 16) | ((ULONG)v->Data[3] << 24);
        }
        ZwClose(key);
    }
    return r;
}

/*
 * The absolute path of Device Parameters, so that driver.c's registry
 * helpers (OpenKey) can open it with WdfRegistryOpenKey; a miniport-mode
 * WDFDEVICE is not a PnP device of KMDF's.
 */
static NTSTATUS
DevKeyPath(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    HANDLE h;
    ULONG len = 0;
    PKEY_NAME_INFORMATION info;
    NTSTATUS status;

    status = IoOpenDeviceRegistryKey(Ctx->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &h);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    (VOID)ZwQueryKey(h, KeyNameInformation, NULL, 0, &len);
    if (len == 0 || len > 4096) {
        ZwClose(h);
        return STATUS_UNSUCCESSFUL;
    }
    info = (PKEY_NAME_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, AIC_WDI_TAG);
    if (info == NULL) {
        ZwClose(h);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = ZwQueryKey(h, KeyNameInformation, info, len, &len);
    ZwClose(h);
    if (NT_SUCCESS(status) && info->NameLength != 0 && info->NameLength <= 0xFFF0) {
        /* the name moves to the front of the same buffer */
        USHORT n = (USHORT)info->NameLength;

        RtlMoveMemory(info, info->Name, n);
        Ctx->DevKeyPath.Buffer = (PWCH)info;
        Ctx->DevKeyPath.Length = n;
        Ctx->DevKeyPath.MaximumLength = n;
        return STATUS_SUCCESS;
    }
    ExFreePoolWithTag(info, AIC_WDI_TAG);
    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

/* ------------------------------------------------- requests, indications */

static NDIS_STATUS
NtToNdis(_In_ NTSTATUS Status)
{
    switch (Status) {
    case STATUS_SUCCESS:                return NDIS_STATUS_SUCCESS;
    case STATUS_NOT_SUPPORTED:          return NDIS_STATUS_NOT_SUPPORTED;
    case STATUS_INSUFFICIENT_RESOURCES: return NDIS_STATUS_RESOURCES;
    case STATUS_INVALID_PARAMETER:      return NDIS_STATUS_INVALID_DATA;
    case STATUS_DEVICE_NOT_READY:       return NDIS_STATUS_ADAPTER_NOT_READY;
    case STATUS_DEVICE_BUSY:            return NDIS_STATUS_MEDIA_BUSY;
    case STATUS_INVALID_DEVICE_STATE:   return NDIS_STATUS_INVALID_DEVICE_REQUEST;
    case STATUS_BUFFER_TOO_SMALL:       return NDIS_STATUS_BUFFER_TOO_SHORT;
    default:                            return NDIS_STATUS_FAILURE;
    }
}

static BOOLEAN
IsWdiOid(_In_ NDIS_OID Oid)
{
    return (Oid & 0xFFFF0000u) == WDI_OID_PREFIX;
}

PWDI_MESSAGE_HEADER
AicFeReqBuffer(_In_ AIC_REQ Request, _Out_ UINT *InLen, _Out_ UINT *OutLen)
{
    PNDIS_OID_REQUEST r = (PNDIS_OID_REQUEST)Request;

    *InLen = r->DATA.METHOD_INFORMATION.InputBufferLength;
    *OutLen = r->DATA.METHOD_INFORMATION.OutputBufferLength;
    return (PWDI_MESSAGE_HEADER)r->DATA.METHOD_INFORMATION.InformationBuffer;
}

UINT16
AicFeReqId(_In_ AIC_REQ Request)
{
    return (UINT16)(((PNDIS_OID_REQUEST)Request)->DATA.METHOD_INFORMATION.Oid & 0xFFFF);
}

/*
 * M3. The status goes into the OID's completion and the header's Status;
 * BytesWritten covers the header at least on success (Microsoft's WDI
 * contract).
 */
VOID
AicFeReqComplete(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ NTSTATUS Status, _In_ ULONG Bytes)
{
    PAIC_FE fe = (PAIC_FE)W->Fe;
    PNDIS_OID_REQUEST r = (PNDIS_OID_REQUEST)Request;
    NDIS_STATUS ns = NtToNdis(Status);
    PWDI_MESSAGE_HEADER h = (PWDI_MESSAGE_HEADER)r->DATA.METHOD_INFORMATION.InformationBuffer;
    ULONG out = r->DATA.METHOD_INFORMATION.OutputBufferLength;

    if (h != NULL && out >= sizeof(*h)) {
        h->Status = ns;
        if (ns == NDIS_STATUS_SUCCESS && Bytes < sizeof(*h)) {
            Bytes = sizeof(*h);
        }
    } else {
        Bytes = 0;
    }
    r->DATA.METHOD_INFORMATION.BytesWritten = Bytes <= out ? Bytes : out;
    r->DATA.METHOD_INFORMATION.BytesRead = r->DATA.METHOD_INFORMATION.InputBufferLength;
    NdisMOidRequestComplete(fe->Adapter, r, ns);
}

/* NDIS_STATUS_INDICATION, then the header and the TLVs, in one buffer;
 * NDIS and WDI copy what they keep during the call. */
BOOLEAN
AicFeIndicate(_In_ PAIC_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *Header,
              _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    PAIC_FE fe = (PAIC_FE)W->Fe;
    PNDIS_STATUS_INDICATION ind;
    PWDI_MESSAGE_HEADER h;
    SIZE_T size = sizeof(*ind) + sizeof(*h) + TlvLen;

    ind = (PNDIS_STATUS_INDICATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, size, AIC_WDI_TAG);
    if (ind == NULL) {
        return FALSE;
    }
    h = (PWDI_MESSAGE_HEADER)(ind + 1);
    *h = *Header;
    if (Tlv != NULL && TlvLen != 0) {
        RtlCopyMemory(h + 1, Tlv, TlvLen);
    }
    ind->Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    ind->Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    ind->Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    ind->SourceHandle = fe->Adapter;
    ind->PortNumber = 0;
    ind->StatusCode = WDI_DEFINE_INDICATION(MessageId);
    ind->StatusBuffer = h;
    ind->StatusBufferSize = (ULONG)(sizeof(*h) + TlvLen);
    NdisMIndicateStatusEx(fe->Adapter, ind);
    ExFreePoolWithTag(ind, AIC_WDI_TAG);
    return TRUE;
}

/* A task's M4 with TLVs at the request's transaction. */
static VOID
TaskReply(_In_ PAIC_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *H, _In_ NDIS_STATUS Status,
          _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    AicWifiIndicate(W, MessageId, H->PortId, H->TransactionId, H->IhvSpecificId, Status, Tlv, TlvLen);
}

/* ------------------------------------------------------ capabilities */

static VOID
FillRates(_Out_writes_(Count) WDI_DATA_RATE_LIST *List, _In_reads_(Count) const UINT16 *Rates, _In_ ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++) {
        List[i].DataRateFlag = (UINT8)(WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE);
        List[i].DataRateValue = Rates[i];
    }
}

/* What the station can do, as the WiFiCx front end reports it (wificx.cpp
 * AicWifiReportCapabilities), in WDI's one TLV reply. */
static VOID
GetCapabilities(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ PWDI_MESSAGE_HEADER H, _In_ UINT OutLen)
{
    static const UCHAR ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                 132, 136, 140, 144, 149, 153, 157, 161, 165 };
    /* the TLV structures have constructors: no aggregate initializers */
    static const UINT16 cckRates[] = { 2, 4, 11, 22 };
    static const UINT16 erpRates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    static const UINT16 ofdmRates[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    static WDI_ALGO_PAIRS unicast[2], mgmt[1];
    static WDI_PHY_TYPE phys24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    static WDI_PHY_TYPE phys5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT };
    static UINT32 width20 = 20;
    static UINT32 power[] = { 100 };
    static WDI_DATA_RATE_LIST cck[ARRAYSIZE(cckRates)], erp[ARRAYSIZE(erpRates)], ofdm[ARRAYSIZE(ofdmRates)];
    PAIC_DEVICE_CONTEXT ctx = W->Ctx;
    aic_dev *d = &ctx->Aic;
    WDI_GET_ADAPTER_CAPABILITIES_PARAMETERS *P;
    WDI_BAND_INFO_CONTAINER *bands;
    WDI_PHY_INFO_CONTAINER *phy;
    WDI_CHANNEL_MAPPING_ENTRY *chans;
    CHAR fw[64];
    ULONG i, nb, np, fwLen;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;
    NDIS_STATUS ns;
    PNDIS_OID_REQUEST r = (PNDIS_OID_REQUEST)Request;

    if (!W->HwReady) {
        AicFeReqComplete(W, Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }
    unicast[0].AuthAlgorithm = WDI_AUTH_ALGO_80211_OPEN;
    unicast[0].CipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    unicast[1].AuthAlgorithm = WDI_AUTH_ALGO_RSNA_PSK;
    unicast[1].CipherAlgorithm = WDI_CIPHER_ALGO_CCMP;
    mgmt[0].AuthAlgorithm = WDI_AUTH_ALGO_80211_OPEN;
    mgmt[0].CipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    FillRates(cck, cckRates, ARRAYSIZE(cckRates));
    FillRates(erp, erpRates, ARRAYSIZE(erpRates));
    FillRates(ofdm, ofdmRates, ARRAYSIZE(ofdmRates));
#pragma warning(suppress : 4291)    /* no placement delete: the constructors do not throw */
    P = new ((ULONG_PTR)0) WDI_GET_ADAPTER_CAPABILITIES_PARAMETERS;
    bands = (WDI_BAND_INFO_CONTAINER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 2 * sizeof(*bands), AIC_WDI_TAG);
    phy = (WDI_PHY_INFO_CONTAINER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 * sizeof(*phy), AIC_WDI_TAG);
    chans = (WDI_CHANNEL_MAPPING_ENTRY *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (13 + sizeof(ch5)) * sizeof(*chans),
                                                         AIC_WDI_TAG);
    if (P == NULL || bands == NULL || phy == NULL || chans == NULL) {
        ns = NDIS_STATUS_RESOURCES;
        goto out;
    }

    P->Optional.CommunicationAttributes_IsPresent = 1;
    P->CommunicationAttributes.Optional.CommunicationCapabilities_IsPresent = 1;
    P->CommunicationAttributes.CommunicationCapabilities.MaxCommandSize = AIC_WDI_MAX_COMMAND;

    {
        WDI_INTERFACE_CAPABILITIES *ic = &P->InterfaceAttributes.InterfaceCapabilities;

        ic->MTUSize = 1500;
        ic->MaxMultiCastListSize = 32;
        ic->BackFillSize = 0;
        RtlCopyMemory(ic->Address.Address, d->Mac, 6);
        ic->MaxTxRate = 72200;                  /* kbit/s: HT20 MCS 7, one stream, what ME_CONFIG sets up */
        ic->MaxRxRate = ic->MaxTxRate;
        ic->HardwareRadioState = TRUE;
        ic->SoftwareRadioState = W->RadioOn;
        ic->SupportPLR = FALSE;
        ic->SupportFLR = FALSE;
        ic->ActionFramesSupported = FALSE;
        ic->NumRxStreams = 1;
        ic->NumTxStreams = 1;
        ic->NumChannels = 1;
        ic->BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
        ic->SupportsNonWdiOidRequests = FALSE;
    }
    /* zero-terminated */
    (VOID)RtlStringCbCopyA(fw, sizeof(fw), d->FwVersion[0] ? d->FwVersion : "unknown");
    fwLen = (ULONG)strlen(fw) + 1;
    P->InterfaceAttributes.FirmwareVersion.SimpleAssign(fw, fwLen);

    {
        WDI_STATION_CAPABILITIES *sc = &P->StationAttributes.StationCapabilities;

        sc->ScanSSIDListSize = 1;
        sc->DesiredBSSIDListSize = 8;
        sc->DesiredSSIDListSize = 1;
        sc->PrivacyExemptionListSize = 8;
        sc->KeyMappingTableSize = 4;
        sc->DefaultKeyTableSize = 4;
        sc->WEPKeyValueMaxLength = 0;           /* no WEP */
        sc->MaxNumPerSTA = 4;
        sc->SupportedQOSFlags = 0;
        sc->HostFIPSModeImplemented = 0;
        sc->MFPCapable = 0;
        sc->AutoPowerSaveMode = FALSE;          /* power save is off (ME_CONFIG) */
        sc->BSSListCachemanagement = FALSE;     /* Windows keeps the network list (SCAN-2) */
        sc->ConnectBSSSelectionOverride = FALSE;
        sc->uMaxNetworkOffloadListSize = 0;
    }
    P->StationAttributes.Optional.UnicastAlgorithms_IsPresent = 1;
    P->StationAttributes.UnicastAlgorithms.SimpleAssign(unicast, (UINT32)ARRAYSIZE(unicast));
    P->StationAttributes.Optional.MulticastDataAlgorithms_IsPresent = 1;
    P->StationAttributes.MulticastDataAlgorithms.SimpleAssign(unicast, (UINT32)ARRAYSIZE(unicast));
    P->StationAttributes.Optional.MulticastManagementAlgorithms_IsPresent = 1;
    P->StationAttributes.MulticastManagementAlgorithms.SimpleAssign(mgmt, (UINT32)ARRAYSIZE(mgmt));

    {
        WDI_DATAPATH_CAPABILITIES *dc = &P->DatapathAttributes.DataPathCapabilities;

        P->Optional.DatapathAttributes_IsPresent = 1;
        P->DatapathAttributes.Optional.DataPathCapabilities_IsPresent = 1;
        /* USB: message based; one queue per port, flow-controlled by the TX slots */
        dc->InterconnectType = WDI_INTERCONNECT_MESSAGE_BASED;
        dc->MaxNumPeers = 4;
        dc->TxTargetPriorityQueueing = TRUE;
        dc->TxMaxScatterGatherElementsPerFrame = 0xFF;
        dc->TxExplicitSendCompleteFlagRequired = TRUE;
        dc->TxMinEffectiveFrameSize = 0;
        dc->TxFrameSizeGranularity = 1;
        dc->RxTxForwarding = FALSE;
        dc->RxMaxThroughput = 72;
    }

    /* the channels ME_CHAN_CONFIG gives the firmware (aic_wlan.c) */
    for (i = 0; i < 13; i++) {
        chans[i].ChannelNumber = i + 1;
        chans[i].ChannelCenterFrequency = 2412 + 5 * i;
    }
    for (i = 0; i < sizeof(ch5); i++) {
        chans[13 + i].ChannelNumber = ch5[i];
        chans[13 + i].ChannelCenterFrequency = 5000 + 5u * ch5[i];
    }
    RtlZeroMemory(bands, 2 * sizeof(*bands));
    bands[0].BandCapabilities.BandID = WDI_BAND_ID_2400;
    bands[0].BandCapabilities.BandState = TRUE;
    bands[0].ValidPhyTypes.SimpleAssign(phys24, (UINT32)ARRAYSIZE(phys24));
    bands[0].ValidChannelTypes.SimpleAssign(chans, 13);
    bands[0].ChannelWidthList.SimpleAssign(&width20, 1);
    nb = 1;
    if (d->Is5gSupported) {
        bands[1].BandCapabilities.BandID = WDI_BAND_ID_5000;
        bands[1].BandCapabilities.BandState = TRUE;
        bands[1].ValidPhyTypes.SimpleAssign(phys5, (UINT32)ARRAYSIZE(phys5));
        bands[1].ValidChannelTypes.SimpleAssign(chans + 13, (UINT32)sizeof(ch5));
        bands[1].ChannelWidthList.SimpleAssign(&width20, 1);
        nb = 2;
    }
    P->Optional.BandInfo_IsPresent = 1;
    P->BandInfo.SimpleAssign(bands, nb);

    /* the rate lists as the WiFiCx front end gives them (HT: the ERP list) */
    RtlZeroMemory(phy, 4 * sizeof(*phy));
    phy[0].PhyCapabilities.PhyType = WDI_PHY_TYPE_HRDSSS;
    phy[0].DataRateList.SimpleAssign(cck, (UINT32)ARRAYSIZE(cck));
    phy[1].PhyCapabilities.PhyType = WDI_PHY_TYPE_ERP;
    phy[1].DataRateList.SimpleAssign(erp, (UINT32)ARRAYSIZE(erp));
    phy[2].PhyCapabilities.PhyType = WDI_PHY_TYPE_HT;
    phy[2].DataRateList.SimpleAssign(erp, (UINT32)ARRAYSIZE(erp));
    np = 3;
    if (d->Is5gSupported) {
        phy[3].PhyCapabilities.PhyType = WDI_PHY_TYPE_OFDM;
        phy[3].DataRateList.SimpleAssign(ofdm, (UINT32)ARRAYSIZE(ofdm));
        np = 4;
    }
    for (i = 0; i < np; i++) {
        phy[i].PhyCapabilities.MPDUMaxLength = 2346;
        phy[i].TxPowerLevelList.SimpleAssign(power, (UINT32)ARRAYSIZE(power));
    }
    P->Optional.PhyInfo_IsPresent = 1;
    P->PhyInfo.SimpleAssign(phy, np);

    /* no wake-on-LAN */
    P->Optional.PmCapabilities_IsPresent = 1;
    P->PmCapabilities.MinMagicPacketWakeUp = NdisDeviceStateUnspecified;
    P->PmCapabilities.MinPatternWakeUp = NdisDeviceStateUnspecified;
    P->PmCapabilities.MinLinkChangeWakeUp = NdisDeviceStateUnspecified;

    ns = GenerateWdiGetAdapterCapabilitiesFromIhv(P, 0, &W->Tlv, &tlvLen, &tlv);
out:
    AicPutULong(ctx, L"WdiCapsStatus", (ULONG)ns);
    AicPutULong(ctx, L"WdiCapsBytes", tlvLen);
    if (ns != NDIS_STATUS_SUCCESS) {
        AicFeReqComplete(W, Request, ns == NDIS_STATUS_RESOURCES ? STATUS_INSUFFICIENT_RESOURCES : STATUS_UNSUCCESSFUL,
                         0);
    } else if (OutLen < sizeof(*H) + tlvLen) {
        /* WDI asks again with a buffer this big */
        r->DATA.METHOD_INFORMATION.BytesNeeded = (ULONG)(sizeof(*H) + tlvLen);
        AicFeReqComplete(W, Request, STATUS_BUFFER_TOO_SMALL, 0);
    } else {
        RtlCopyMemory(H + 1, tlv, tlvLen);
        AicFeReqComplete(W, Request, STATUS_SUCCESS, (ULONG)(sizeof(*H) + tlvLen));
        AicCrumb(ctx, "wdi: capabilities reported");
    }
    if (tlv != NULL) {
        FreeGenerated(tlv);
    }
    delete P;                                   /* before what its arrays point at */
    if (chans != NULL) {
        ExFreePoolWithTag(chans, AIC_WDI_TAG);
    }
    if (phy != NULL) {
        ExFreePoolWithTag(phy, AIC_WDI_TAG);
    }
    if (bands != NULL) {
        ExFreePoolWithTag(bands, AIC_WDI_TAG);
    }
}

/* ------------------------------------------------------------- ports */

/* The station port Windows asks for after the open; only one, station only. */
static VOID
CreatePort(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ PWDI_MESSAGE_HEADER H, _In_ UINT InLen)
{
    PAIC_FE fe = (PAIC_FE)W->Fe;
    WDI_TASK_CREATE_PORT_PARAMETERS P;
    WDI_INDICATION_CREATE_PORT_COMPLETE_PARAMETERS C;
    WDI_MESSAGE_HEADER h = *H;
    NDIS_STATUS parsed;
    UINT16 mask = 0;
    UINT32 port = 0;
    UINT8 *tlv = NULL;
    ULONG tlvLen = 0;

    parsed = ParseWdiTaskCreatePortToIhv((ULONG)(InLen - sizeof(*H)), (const UINT8 *)(H + 1), &W->Tlv, &P);
    if (parsed == NDIS_STATUS_SUCCESS) {
        mask = P.CreatePortParameters.OpModeMask;
        port = P.CreatePortParameters.NdisPortNumber;
    }
    CleanupParsedWdiTaskCreatePortToIhv(&P);
    AicPutULong(W->Ctx, L"WdiCreatePortMask", mask);
    AicPutULong(W->Ctx, L"WdiCreatePortNumber", port);
    if (parsed != NDIS_STATUS_SUCCESS) {
        AicFeReqComplete(W, Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    if (!(mask & WDI_OPERATION_MODE_STA) || fe->PortCreated || !W->HwReady) {
        /* no Wi-Fi Direct or hotspot ports */
        AicFeReqComplete(W, Request, !W->HwReady ? STATUS_DEVICE_NOT_READY : STATUS_NOT_SUPPORTED, 0);
        return;
    }
    fe->PortCreated = TRUE;
    W->PortId = (UINT16)port;
    W->AdapterUp = TRUE;
    AicFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));          /* M3 */
    RtlCopyMemory(C.PortAttributes.MacAddress.Address, W->Ctx->Aic.Mac, 6);
    C.PortAttributes.PortNumber = (UINT16)port;
    if (GenerateWdiIndicationCreatePortCompleteFromIhv(&C, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
        TaskReply(W, WDI_INDICATION_CREATE_PORT_COMPLETE, &h, NDIS_STATUS_FAILURE, NULL, 0);
        return;
    }
    TaskReply(W, WDI_INDICATION_CREATE_PORT_COMPLETE, &h, NDIS_STATUS_SUCCESS, tlv, tlvLen);   /* M4 */
    FreeGenerated(tlv);
    AicPutULong(W->Ctx, L"WdiPortId", W->PortId);
    AicCrumb(W->Ctx, "wdi: station port created");
}

static VOID
DeletePort(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ PWDI_MESSAGE_HEADER H)
{
    PAIC_FE fe = (PAIC_FE)W->Fe;
    WDI_MESSAGE_HEADER h = *H;

    AicFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));          /* M3 */
    /* Windows disconnects first (TASK_DISCONNECT); the link is down here */
    fe->PortCreated = FALSE;
    W->AdapterUp = FALSE;
    TaskReply(W, WDI_INDICATION_DELETE_PORT_COMPLETE, &h, NDIS_STATUS_SUCCESS, NULL, 0);    /* M4 */
}

/* The messages only WDI has; the rest are wdicore.cpp's. */
BOOLEAN
AicFeDispatch(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ UINT16 Id, _In_ PWDI_MESSAGE_HEADER H,
              _In_ UINT InLen, _In_ UINT OutLen)
{
    WDI_MESSAGE_HEADER h = *H;

    switch (Id) {
    case WDI_GET_ADAPTER_CAPABILITIES:
        GetCapabilities(W, Request, H, OutLen);
        return TRUE;
    case WDI_TASK_CREATE_PORT:
        CreatePort(W, Request, H, InLen);
        return TRUE;
    case WDI_TASK_DELETE_PORT:
        DeletePort(W, Request, H);
        return TRUE;
    case WDI_TASK_OPEN:
    case WDI_TASK_CLOSE:
        /* MiniportWdiOpenAdapter / CloseAdapter do the work; should the
         * task come as well, it is done at once */
        AicFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));
        TaskReply(W, Id == WDI_TASK_OPEN ? WDI_INDICATION_OPEN_COMPLETE : WDI_INDICATION_CLOSE_COMPLETE, &h,
                  NDIS_STATUS_SUCCESS, NULL, 0);
        return TRUE;
    case WDI_SET_POWER_STATE:
    case WDI_SET_AUTO_POWER_SAVE:
    case WDI_SET_RECEIVE_COALESCING:
    case WDI_SET_CLEAR_RECEIVE_COALESCING:
        /* accepted, nothing to do: no power save, no wake-up, no coalescing */
        AicFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));
        return TRUE;
    default:
        return FALSE;
    }
}

/*
 * The worker's first job: what PrepareHardware does on Windows 11 (the USB
 * configuration, the readers, the stale-firmware check, the firmware init:
 * seconds), then the open's completion. Without firmware the open fails
 * and Windows gets no adapter this start; FwState and LastStep in Device
 * Parameters say why.
 */
VOID
AicFeWorkerStart(_In_ PAIC_WIFI W)
{
    PAIC_FE fe = (PAIC_FE)W->Fe;
    PAIC_DEVICE_CONTEXT ctx = W->Ctx;
    NDIS_STATUS ns = NDIS_STATUS_ADAPTER_NOT_READY;
    NTSTATUS status;
    ULONG i;

    if (InterlockedExchange(&fe->OpenPending, 0) == 0) {
        return;
    }
    AicCrumb(ctx, "wdi: open");
    status = AicUsbPrepare(ctx);
    AicPutULong(ctx, L"OpenStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        AicCrumb(ctx, "wdi: USB setup failed");
    } else if (!ctx->Runtime) {
        /* aic8800wdi.inf binds PID_8D81 MI_02 only */
        AicCrumb(ctx, "wdi: not the runtime firmware (PID_8D81)");
    } else {
        /* the out pipes too: no PnP starts the targets of a miniport */
        for (i = 0; i < ctx->OutPipes; i++) {
            (VOID)WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(ctx->OutPipe[i]));
        }
        ctx->Aic.Ops = &AicUsbOps;
        ctx->Aic.Ctx = ctx;
        ctx->Aic.TxBuf = ctx->TxBuf;
        ctx->Aic.TxBufSize = AIC_CMD_BUF_MAX + 2048;
        aic_init(&ctx->Aic);
        AicCrumb(ctx, "wdi: runtime firmware (PID_8D81)");
        if (NT_SUCCESS(AicUsbStartReaders(ctx))) {
            AicRuntimeInit(ctx);
        }
        /* Windows' supplicant gets the EAPOL frames; aic_wpa.c stays idle */
        ctx->Aic.HostAuth = 1;
        W->HwReady = ctx->FwReady;
        if (ctx->FwReady) {
            ns = NDIS_STATUS_SUCCESS;
        } else {
            AicCrumb(ctx, "wdi: firmware not running (Stage 2 or more needed)");
        }
    }
    AicPutULong(ctx, L"WdiOpenStatus", (ULONG)ns);
    AicCrumb(ctx, ns == NDIS_STATUS_SUCCESS ? "wdi: open complete" : "wdi: open failed");
    fe->OpenComplete(fe->Adapter, ns);
}

VOID
AicFeService(_In_ PAIC_WIFI W)
{
    AicTalService(W->Ctx);
}

/* --------------------------------------------------- control handlers */

static NDIS_STATUS
AicWdiAllocateAdapter(_In_ NDIS_HANDLE NdisMiniportHandle, _In_ NDIS_HANDLE MiniportDriverContext,
                      _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters,
                      _In_ PNDIS_WDI_INIT_PARAMETERS NdisWdiInitParameters,
                      _Inout_ PNDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES RegistrationAttributes)
{
    WDF_OBJECT_ATTRIBUTES attr;
    PDEVICE_OBJECT pdo = NULL, fdo = NULL, next = NULL;
    PAIC_DEVICE_CONTEXT ctx;
    WDFDEVICE device;
    PAIC_WIFI W;
    PAIC_FE fe;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(MiniportInitParameters);

    ParamCrumb(L"LastStep", L"AllocateAdapter", 0);
    ParamCrumb(L"OsWdiVersion", NULL, NdisWdiInitParameters->WdiVersion);
    NdisMGetDeviceProperty(NdisMiniportHandle, &pdo, &fdo, &next, NULL, NULL);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AIC_DEVICE_CONTEXT);
    status = WdfDeviceMiniportCreate(WdfGetDriver(), &attr, fdo, next, pdo, &device);
    ParamCrumb(L"MiniportCreateStatus", NULL, (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return NDIS_STATUS_FAILURE;
    }
    ctx = AicGetContext(device);
    ctx->Device = device;
    ctx->Pdo = pdo;
    ctx->NextDevice = next;
    KeInitializeSpinLock(&ctx->RingLock);
    KeInitializeEvent(&ctx->RxEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&ctx->DataLock);
    ctx->Mode = AIC_MODE_WIFICX;                /* Windows' own Wi-Fi: wdicore.cpp */
    ctx->Runtime = TRUE;                        /* PID_8D81; AicUsbPrepare checks */
    status = DevKeyPath(ctx);
    ParamCrumb(L"DevKeyStatus", NULL, (ULONG)status);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return NDIS_STATUS_FAILURE;
    }
    AicCrumb(ctx, "wdi: allocate adapter");
    AicPutULong(ctx, L"OsWdiVersion", NdisWdiInitParameters->WdiVersion);
    AicPutULong(ctx, L"ActiveMode", AIC_MODE_WIFICX);

    ctx->Ring = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)AIC_RING_SLOTS * AIC_RING_SLOT_SIZE, AIC_POOL_TAG);
    ctx->TxBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, AIC_CMD_BUF_MAX + 2048, AIC_POOL_TAG);
    W = AicWifiAlloc(ctx);
    fe = (PAIC_FE)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(AIC_FE), AIC_WDI_TAG);
    status = AicTalAlloc(ctx, NdisMiniportHandle);
    if (ctx->Ring == NULL || ctx->TxBuf == NULL || W == NULL || fe == NULL || !NT_SUCCESS(status)) {
        if (fe != NULL) {
            ExFreePoolWithTag(fe, AIC_WDI_TAG);
        }
        AicWdiFreeAdapter(ctx);
        return NDIS_STATUS_RESOURCES;
    }
    fe->Adapter = NdisMiniportHandle;
    fe->OpenComplete = NdisWdiInitParameters->OpenAdapterCompleteHandler;
    fe->CloseComplete = NdisWdiInitParameters->CloseAdapterCompleteHandler;
    W->Fe = fe;
    /* parse and generate TLVs at the OS's version */
    W->Tlv.AllocationContext = 0;
    W->Tlv.PeerVersion = NdisWdiInitParameters->WdiVersion;
    /* requests may come from here on; they wait for the worker */
    AicWifiAccept(W);

    RtlZeroMemory(RegistrationAttributes, sizeof(*RegistrationAttributes));
    RegistrationAttributes->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    RegistrationAttributes->Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2;
    RegistrationAttributes->Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2;
    RegistrationAttributes->MiniportAdapterContext = ctx;
    RegistrationAttributes->AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK |
                                             NDIS_MINIPORT_ATTRIBUTES_NDIS_WDM |
                                             NDIS_MINIPORT_ATTRIBUTES_NO_PAUSE_ON_SUSPEND;
    RegistrationAttributes->InterfaceType = NdisInterfacePNPBus;
    AicCrumb(ctx, "wdi: adapter allocated");
    return NDIS_STATUS_SUCCESS;
}

/* Quick: start the worker, which does the bring-up and completes the open. */
static NDIS_STATUS
AicWdiOpenAdapter(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)MiniportAdapterContext;
    PAIC_FE fe = (PAIC_FE)ctx->Wifi->Fe;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(MiniportInitParameters);
    AicCrumb(ctx, "wdi: open adapter");
    InterlockedExchange(&ctx->Stopping, 0);
    ctx->Stage = AicGetULong(ctx, L"Stage", AIC_STAGE_OPEN);
    AicPutULong(ctx, L"StageSeen", ctx->Stage);
    if (ctx->Stage < AIC_STAGE_SCAN) {
        /* as on Windows 11: the Wi-Fi adapter needs the firmware init
         * (Stage 2 or more; aic-bringup.ps1 -Stage 2) */
        AicCrumb(ctx, "wdi: Stage below 2, no adapter");
        fe->OpenComplete(fe->Adapter, NDIS_STATUS_ADAPTER_NOT_READY);
        return NDIS_STATUS_SUCCESS;
    }
    InterlockedExchange(&fe->OpenPending, 1);
    status = AicStartWorker(ctx);
    AicPutULong(ctx, L"WorkerStartStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&fe->OpenPending, 0);
        fe->OpenComplete(fe->Adapter, NDIS_STATUS_RESOURCES);
    }
    return NDIS_STATUS_SUCCESS;
}

static VOID
StopOutPipes(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    ULONG i;

    for (i = 0; i < Ctx->OutPipes; i++) {
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Ctx->OutPipe[i]), WdfIoTargetCancelSentIo);
    }
}

/* The worker stops (running tasks end, waiting requests fail), then the USB I/O. */
static NDIS_STATUS
AicWdiCloseAdapter(_In_ NDIS_HANDLE MiniportAdapterContext)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)MiniportAdapterContext;
    PAIC_FE fe = (PAIC_FE)ctx->Wifi->Fe;

    AicCrumb(ctx, "wdi: close adapter");
    AicStopWorker(ctx);
    AicWifiReleaseHardware(ctx);
    AicUsbStopReaders(ctx);
    StopOutPipes(ctx);                          /* TX writes in flight end here */
    ctx->FwReady = FALSE;
    fe->PortCreated = FALSE;
    AicPutStats(ctx);
    AicCrumb(ctx, "wdi: closed");
    fe->CloseComplete(fe->Adapter, NDIS_STATUS_SUCCESS);
    return NDIS_STATUS_SUCCESS;
}

static VOID
AicWdiFreeAdapter(_In_ NDIS_HANDLE MiniportAdapterContext)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)MiniportAdapterContext;
    PVOID fe = ctx->Wifi != NULL ? ctx->Wifi->Fe : NULL;

    AIC_LOG("teardown: free adapter");
    AicStopWorker(ctx);
    AicWifiCleanup(ctx);                        /* fails what still waits, through fe */
    if (fe != NULL) {
        ExFreePoolWithTag(fe, AIC_WDI_TAG);
    }
    AicUsbStopReaders(ctx);
    AicNetFree(ctx);
    if (ctx->Ring != NULL) {
        ExFreePoolWithTag(ctx->Ring, AIC_POOL_TAG);
        ctx->Ring = NULL;
    }
    if (ctx->TxBuf != NULL) {
        ExFreePoolWithTag(ctx->TxBuf, AIC_POOL_TAG);
        ctx->TxBuf = NULL;
    }
    if (ctx->DevKeyPath.Buffer != NULL) {
        ExFreePoolWithTag(ctx->DevKeyPath.Buffer, AIC_WDI_TAG);
        ctx->DevKeyPath.Buffer = NULL;
        ctx->DevKeyPath.Length = 0;
    }
    WdfObjectDelete(ctx->Device);               /* the USB target, the TX requests and the context go with it */
}

static NDIS_STATUS
AicWdiHangDiagnose(_In_ NDIS_HANDLE MiniportDriverContext, _In_ eDiagnoseLevel DiagnoseLevel, _In_ UINT32 BufferSize,
                   _Out_writes_bytes_to_(BufferSize, *pOutputSize) UINT8 *FirmwareBlob, _Out_ UINT32 *pOutputSize)
{
    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(DiagnoseLevel);
    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(FirmwareBlob);
    *pOutputSize = 0;
    return NDIS_STATUS_NOT_SUPPORTED;
}

/* M1: queue for the worker. Not a WDI OID: not this driver's. */
static NDIS_STATUS
AicWdiOidRequest(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ PNDIS_OID_REQUEST OidRequest)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)MiniportAdapterContext;
    PAIC_WIFI W = ctx->Wifi;
    NTSTATUS fail;

    if (OidRequest->RequestType != NdisRequestMethod || !IsWdiOid(OidRequest->DATA.METHOD_INFORMATION.Oid)) {
        return NDIS_STATUS_NOT_SUPPORTED;
    }
    if (W == NULL || OidRequest->DATA.METHOD_INFORMATION.InformationBuffer == NULL ||
        OidRequest->DATA.METHOD_INFORMATION.InputBufferLength < sizeof(WDI_MESSAGE_HEADER)) {
        return NDIS_STATUS_INVALID_DATA;
    }
    if (!AicWifiQueue(W, OidRequest, &fail)) {
        return NtToNdis(fail);
    }
    return NDIS_STATUS_PENDING;
}

static VOID
AicWdiPnpEvent(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)MiniportAdapterContext;

    if (NetDevicePnPEvent->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved) {
        /* unplugged (or the chip went back to its loader): no more USB
         * writes, the worker's waits end */
        AIC_LOG("teardown: surprise removal");
        InterlockedExchange(&ctx->Stopping, 1);
        ctx->Aic.Abort = 1;
        KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);
    }
}

static VOID
AicWdiShutdown(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    /* may run at high IRQL: leave the chip as it is */
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ShutdownAction);
}

/* ------------------------------------------------------------- driver */

static VOID
AicWdiUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_Driver != NULL) {
        NdisMDeregisterWdiMiniportDriver(g_Driver);
        g_Driver = NULL;
    }
}

/*
 * From driver.c's DriverEntry, as service aic8800usb: KMDF first, without
 * its dispatch routines (NDIS owns the IRPs), then the WDI registration,
 * which installs Microsoft's WDI layer above this driver. Breadcrumbs in
 * Services\aic8800usb\Parameters (PreEntry, RegisterStatus, ...).
 */
extern "C" NTSTATUS
AicDriverEntryWdi(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS mc;
    NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS wc;
    WDF_DRIVER_CONFIG config;
    NDIS_STATUS ns;
    NTSTATUS status;
    ULONG wdi;

    if (NT_SUCCESS(RtlStringCchCopyNW(g_Params, RTL_NUMBER_OF(g_Params), RegistryPath->Buffer,
                                      RegistryPath->Length / sizeof(WCHAR)))) {
        (VOID)RtlStringCchCatW(g_Params, RTL_NUMBER_OF(g_Params), L"\\Parameters");
    }
    ParamCrumb(L"PreEntry", L"DriverEntry: WDI miniport, KMDF 1.31, NDIS 6.50", 0);
    ParamCrumb(L"NdisVersion", NULL, NdisGetVersion());

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNoDispatchOverride;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    ParamCrumb(L"WdfDriverCreateStatus", NULL, (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&mc, sizeof(mc));
    mc.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    mc.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    mc.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    mc.MajorNdisVersion = 6;
    mc.MinorNdisVersion = 50;
    mc.MajorDriverVersion = 1;
    mc.MinorDriverVersion = 0;
    mc.UnloadHandler = AicWdiUnload;
    mc.OidRequestHandler = AicWdiOidRequest;
    mc.DevicePnPEventNotifyHandler = AicWdiPnpEvent;
    mc.ShutdownHandlerEx = AicWdiShutdown;

    RtlZeroMemory(&wc, sizeof(wc));
    wc.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_WDI_CHARACTERISTICS;
    wc.Header.Revision = NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS_REVISION_1;
    wc.Header.Size = NDIS_SIZEOF_MINIPORT_WDI_CHARACTERISTICS_REVISION_1;
    /* the WDI version this driver speaks; Parameters\WdiVersion overrides it on the board */
    wdi = ParamGet(L"WdiVersion", 0);
    wc.WdiVersion = wdi != 0 ? wdi : WDI_VERSION_LATEST;
    wc.AllocateAdapterHandler = AicWdiAllocateAdapter;
    wc.FreeAdapterHandler = AicWdiFreeAdapter;
    wc.OpenAdapterHandler = AicWdiOpenAdapter;
    wc.CloseAdapterHandler = AicWdiCloseAdapter;
    wc.HangDiagnoseHandler = AicWdiHangDiagnose;
    wc.TalTxRxInitializeHandler = AicTalInitialize;
    wc.TalTxRxDeinitializeHandler = AicTalDeinitialize;
    ParamCrumb(L"DriverWdiVersion", NULL, wc.WdiVersion);

    ns = NdisMRegisterWdiMiniportDriver(DriverObject, RegistryPath, NULL, &mc, &wc, &g_Driver);
    ParamCrumb(L"RegisterStatus", NULL, (ULONG)ns);
    return ns == NDIS_STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
