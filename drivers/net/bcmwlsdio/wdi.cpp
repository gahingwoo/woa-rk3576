/*
 * wdi.cpp - bcmwlwdi.sys: the Broadcom SDIO Wi-Fi of the TaishanPi 3M
 * (BCM43456) and the ArmSoM CM5 (BCM43752) as a WDI miniport for Windows 10
 * 2004..22H2 (build 19041..19045), which has no WiFiCx. Installed by
 * bcmwlwdi.inf on SDIO function 1. See docs/dev/wifi-win10.md.
 *
 * WDI is an NDIS 6.50 miniport whose MiniportInitializeEx, MiniportHaltEx
 * and data path Microsoft's WDI layer owns: the driver gets WDI messages as
 * OIDs (OID_WDI_*: M1; completing the OID is M3), sends task completions and
 * events as status indications (NDIS_STATUS_WDI_INDICATION_*: M4), and moves
 * frames through the TAL handlers (wdital.c). The messages are the same as
 * under WiFiCx, so scan, connect, keys, radio, reset and abort are
 * wdicore.cpp, shared with bcmwlsdio.sys; this file adds what only WDI has:
 * the adapter's open and close, its capabilities as a TLV reply
 * (WDI_GET_ADAPTER_CAPABILITIES), and the station port
 * (WDI_TASK_CREATE_PORT / DELETE_PORT).
 *
 * KMDF 1.31 in miniport mode (WdfDeviceMiniportCreate): no WDF PnP, but the
 * registry, the work item and the device context the bring-up (bringup.c)
 * uses. The SD bus interface is opened on the PDO NDIS names.
 *
 * Threads: MiniportOidRequest (IRQL <= DISPATCH_LEVEL) queues the request
 * and returns NDIS_STATUS_PENDING; the worker thread (bringup.c) does the
 * bring-up when the adapter is opened, completes the open, and from then on
 * runs every request and is the only thread that talks to the card.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "wdicore.h"

#define BWL_WDI_TAG 'dWcB'

/* MaxCommandSize in the capabilities: a TASK_CONNECT carries whole beacons */
#define BWL_WDI_MAX_COMMAND 0x10000

typedef struct _BWL_WDI {
    NDIS_HANDLE Adapter;                        /* MiniportAdapterHandle */
    NDIS_WDI_OPEN_ADAPTER_COMPLETE_HANDLER OpenComplete;
    NDIS_WDI_CLOSE_ADAPTER_COMPLETE_HANDLER CloseComplete;
    volatile LONG OpenPending;                  /* the worker owes the open's completion */
    NDIS_STATUS OpenStatus;
    BOOLEAN PortCreated;
} BWL_WDI, *PBWL_WDI;

static NDIS_HANDLE g_Driver;
static WCHAR g_Params[256];                     /* Services\bcmwlwdi\Parameters */

extern "C" DRIVER_INITIALIZE DriverEntry;
static MINIPORT_UNLOAD BwlWdiUnload;
static MINIPORT_OID_REQUEST BwlWdiOidRequest;
static MINIPORT_DEVICE_PNP_EVENT_NOTIFY BwlWdiPnpEvent;
static MINIPORT_SHUTDOWN BwlWdiShutdown;
static MINIPORT_WDI_ALLOCATE_ADAPTER BwlWdiAllocateAdapter;
static MINIPORT_WDI_FREE_ADAPTER BwlWdiFreeAdapter;
static MINIPORT_WDI_OPEN_ADAPTER BwlWdiOpenAdapter;
static MINIPORT_WDI_CLOSE_ADAPTER BwlWdiCloseAdapter;
static MINIPORT_WDI_ADAPTER_HANG_DIAGNOSE BwlWdiHangDiagnose;

/* wdital.c */
EXTERN_C MINIPORT_WDI_TAL_TXRX_INITIALIZE BwlTalInitialize;
EXTERN_C MINIPORT_WDI_TAL_TXRX_DEINITIALIZE BwlTalDeinitialize;
EXTERN_C NTSTATUS BwlTalAlloc(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ NDIS_HANDLE Adapter);

/* ------------------------------------------------------------ records */

/* Before a device exists: one value in Services\bcmwlwdi\Parameters, flushed. */
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
 * The absolute path of Device Parameters, so that the bring-up's registry
 * helpers (BwlOpenDeviceKey) can open it with WdfRegistryOpenKey; a
 * miniport-mode WDFDEVICE is not a PnP device of KMDF's.
 */
static NTSTATUS
DevKeyPath(_In_ PBWL_DEVICE_CONTEXT Ctx)
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
    info = (PKEY_NAME_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, BWL_WDI_TAG);
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
    ExFreePoolWithTag(info, BWL_WDI_TAG);
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
BwlFeReqBuffer(_In_ BWL_REQ Request, _Out_ UINT *InLen, _Out_ UINT *OutLen)
{
    PNDIS_OID_REQUEST r = (PNDIS_OID_REQUEST)Request;

    *InLen = r->DATA.METHOD_INFORMATION.InputBufferLength;
    *OutLen = r->DATA.METHOD_INFORMATION.OutputBufferLength;
    return (PWDI_MESSAGE_HEADER)r->DATA.METHOD_INFORMATION.InformationBuffer;
}

UINT16
BwlFeReqId(_In_ BWL_REQ Request)
{
    return (UINT16)(((PNDIS_OID_REQUEST)Request)->DATA.METHOD_INFORMATION.Oid & 0xFFFF);
}

/*
 * M3. The status goes into the OID's completion and the header's Status;
 * BytesWritten covers the header at least on success (Microsoft's WDI
 * contract).
 */
VOID
BwlFeReqComplete(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ NTSTATUS Status, _In_ ULONG Bytes)
{
    PBWL_WDI fe = (PBWL_WDI)W->Fe;
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
BwlFeIndicate(_In_ PBWL_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *Header,
              _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    PBWL_WDI fe = (PBWL_WDI)W->Fe;
    PNDIS_STATUS_INDICATION ind;
    PWDI_MESSAGE_HEADER h;
    SIZE_T size = sizeof(*ind) + sizeof(*h) + TlvLen;

    ind = (PNDIS_STATUS_INDICATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, size, BWL_WDI_TAG);
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
    ExFreePoolWithTag(ind, BWL_WDI_TAG);
    return TRUE;
}

/* A task's M4 with TLVs at the request's transaction. */
static VOID
TaskReply(_In_ PBWL_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *H, _In_ NDIS_STATUS Status,
          _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    BwlWifiIndicate(W, MessageId, H->PortId, H->TransactionId, H->IhvSpecificId, Status, Tlv, TlvLen);
}

/* ------------------------------------------------------ capabilities */

/* What the station can do, as bcmwlsdio's WiFiCx front end reports it
 * (wificx.cpp BwlWifiReportCapabilities), in WDI's one TLV reply. */
static VOID
GetCapabilities(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ PWDI_MESSAGE_HEADER H, _In_ UINT OutLen)
{
    static const UCHAR ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                 132, 136, 140, 144, 149, 153, 157, 161, 165 };
    static WDI_ALGO_PAIRS unicast[] = {
        { WDI_AUTH_ALGO_80211_OPEN, WDI_CIPHER_ALGO_NONE },
        { WDI_AUTH_ALGO_RSNA_PSK, WDI_CIPHER_ALGO_CCMP },
    };
    static WDI_ALGO_PAIRS mgmt[] = { { WDI_AUTH_ALGO_80211_OPEN, WDI_CIPHER_ALGO_NONE } };
    static WDI_PHY_TYPE phys24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    static WDI_PHY_TYPE phys5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT };
    static UINT32 width20 = 20;
    static UINT32 power[] = { 100 };
#define R(x) { WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE, x }
    static WDI_DATA_RATE_LIST cck[] = { R(2), R(4), R(11), R(22) };
    static WDI_DATA_RATE_LIST erp[] = { R(2), R(4), R(11), R(22), R(12), R(18), R(24), R(36), R(48), R(72),
                                        R(96), R(108) };
    static WDI_DATA_RATE_LIST ofdm[] = { R(12), R(18), R(24), R(36), R(48), R(72), R(96), R(108) };
    /* HT20 1x1 MCS 0-7, 800 ns GI, in 500 kbit/s */
    static WDI_DATA_RATE_LIST ht[] = { R(13), R(26), R(39), R(52), R(78), R(104), R(117), R(130) };
#undef R
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    const bwl_wlan *w = &ctx->Bwl.Wlan;
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
        BwlFeReqComplete(W, Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }
#pragma warning(suppress : 4291)    /* no placement delete: the constructors do not throw */
    P = new ((ULONG_PTR)0) WDI_GET_ADAPTER_CAPABILITIES_PARAMETERS;
    bands = (WDI_BAND_INFO_CONTAINER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 2 * sizeof(*bands), BWL_WDI_TAG);
    phy = (WDI_PHY_INFO_CONTAINER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 * sizeof(*phy), BWL_WDI_TAG);
    chans = (WDI_CHANNEL_MAPPING_ENTRY *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (13 + sizeof(ch5)) * sizeof(*chans),
                                                         BWL_WDI_TAG);
    if (P == NULL || bands == NULL || phy == NULL || chans == NULL) {
        ns = NDIS_STATUS_RESOURCES;
        goto out;
    }

    P->Optional.CommunicationAttributes_IsPresent = 1;
    P->CommunicationAttributes.Optional.CommunicationCapabilities_IsPresent = 1;
    P->CommunicationAttributes.CommunicationCapabilities.MaxCommandSize = BWL_WDI_MAX_COMMAND;

    {
        WDI_INTERFACE_CAPABILITIES *ic = &P->InterfaceAttributes.InterfaceCapabilities;

        ic->MTUSize = 1500;
        ic->MaxMultiCastListSize = 32;
        ic->BackFillSize = 0;
        RtlCopyMemory(ic->Address.Address, w->Mac, 6);
        ic->MaxTxRate = w->Has5g ? 433300 : 72200;
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
    (VOID)RtlStringCbCopyA(fw, sizeof(fw), ctx->FwVer[0] ? ctx->FwVer : "unknown");
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
        sc->AutoPowerSaveMode = FALSE;          /* power save is off (C_SET_PM 0) */
        sc->BSSListCachemanagement = FALSE;     /* Windows keeps the network list */
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
        /* SDIO: message based; one queue per port, flow-controlled by the TX FIFO */
        dc->InterconnectType = WDI_INTERCONNECT_MESSAGE_BASED;
        dc->MaxNumPeers = 4;
        dc->TxTargetPriorityQueueing = TRUE;
        dc->TxMaxScatterGatherElementsPerFrame = 0xFF;
        dc->TxExplicitSendCompleteFlagRequired = TRUE;
        dc->TxMinEffectiveFrameSize = 0;
        dc->TxFrameSizeGranularity = 1;
        dc->RxTxForwarding = FALSE;
        dc->RxMaxThroughput = w->Has5g ? 433 : 72;
    }

    /* the channels the CLM normally allows; the firmware's country decides */
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
    if (w->Has5g) {
        bands[1].BandCapabilities.BandID = WDI_BAND_ID_5000;
        bands[1].BandCapabilities.BandState = TRUE;
        bands[1].ValidPhyTypes.SimpleAssign(phys5, (UINT32)ARRAYSIZE(phys5));
        bands[1].ValidChannelTypes.SimpleAssign(chans + 13, (UINT32)sizeof(ch5));
        bands[1].ChannelWidthList.SimpleAssign(&width20, 1);
        nb = 2;
    }
    P->Optional.BandInfo_IsPresent = 1;
    P->BandInfo.SimpleAssign(bands, nb);

    RtlZeroMemory(phy, 4 * sizeof(*phy));
    phy[0].PhyCapabilities.PhyType = WDI_PHY_TYPE_HRDSSS;
    phy[0].DataRateList.SimpleAssign(cck, (UINT32)ARRAYSIZE(cck));
    phy[1].PhyCapabilities.PhyType = WDI_PHY_TYPE_ERP;
    phy[1].DataRateList.SimpleAssign(erp, (UINT32)ARRAYSIZE(erp));
    phy[2].PhyCapabilities.PhyType = WDI_PHY_TYPE_HT;
    phy[2].DataRateList.SimpleAssign(ht, (UINT32)ARRAYSIZE(ht));
    np = 3;
    if (w->Has5g) {
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
    BwlPutULong(ctx, L"WdiCapsStatus", (ULONG)ns);
    BwlPutULong(ctx, L"WdiCapsBytes", tlvLen);
    if (ns != NDIS_STATUS_SUCCESS) {
        BwlFeReqComplete(W, Request, ns == NDIS_STATUS_RESOURCES ? STATUS_INSUFFICIENT_RESOURCES : STATUS_UNSUCCESSFUL,
                         0);
    } else if (OutLen < sizeof(*H) + tlvLen) {
        /* WDI asks again with a buffer this big */
        r->DATA.METHOD_INFORMATION.BytesNeeded = (ULONG)(sizeof(*H) + tlvLen);
        BwlFeReqComplete(W, Request, STATUS_BUFFER_TOO_SMALL, 0);
    } else {
        RtlCopyMemory(H + 1, tlv, tlvLen);
        BwlFeReqComplete(W, Request, STATUS_SUCCESS, (ULONG)(sizeof(*H) + tlvLen));
        BwlCrumb(ctx, "wdi: capabilities reported");
    }
    if (tlv != NULL) {
        FreeGenerated(tlv);
    }
    delete P;                                   /* before what its arrays point at */
    if (chans != NULL) {
        ExFreePoolWithTag(chans, BWL_WDI_TAG);
    }
    if (phy != NULL) {
        ExFreePoolWithTag(phy, BWL_WDI_TAG);
    }
    if (bands != NULL) {
        ExFreePoolWithTag(bands, BWL_WDI_TAG);
    }
}

/* ------------------------------------------------------------- ports */

/* The station port Windows asks for after the open; only one, station only. */
static VOID
CreatePort(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ PWDI_MESSAGE_HEADER H, _In_ UINT InLen)
{
    PBWL_WDI fe = (PBWL_WDI)W->Fe;
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
    BwlPutULong(W->Ctx, L"WdiCreatePortMask", mask);
    BwlPutULong(W->Ctx, L"WdiCreatePortNumber", port);
    if (parsed != NDIS_STATUS_SUCCESS) {
        BwlFeReqComplete(W, Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    if (!(mask & WDI_OPERATION_MODE_STA) || fe->PortCreated || !W->HwReady) {
        /* no Wi-Fi Direct or hotspot ports */
        BwlFeReqComplete(W, Request, !W->HwReady ? STATUS_DEVICE_NOT_READY : STATUS_NOT_SUPPORTED, 0);
        return;
    }
    fe->PortCreated = TRUE;
    W->PortId = (UINT16)port;
    W->AdapterUp = TRUE;
    BwlFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));          /* M3 */
    RtlCopyMemory(C.PortAttributes.MacAddress.Address, W->Ctx->Bwl.Wlan.Mac, 6);
    C.PortAttributes.PortNumber = (UINT16)port;
    if (GenerateWdiIndicationCreatePortCompleteFromIhv(&C, 0, &W->Tlv, &tlvLen, &tlv) != NDIS_STATUS_SUCCESS) {
        TaskReply(W, WDI_INDICATION_CREATE_PORT_COMPLETE, &h, NDIS_STATUS_FAILURE, NULL, 0);
        return;
    }
    TaskReply(W, WDI_INDICATION_CREATE_PORT_COMPLETE, &h, NDIS_STATUS_SUCCESS, tlv, tlvLen);   /* M4 */
    FreeGenerated(tlv);
    BwlPutULong(W->Ctx, L"WdiPortId", W->PortId);
    BwlCrumb(W->Ctx, "wdi: station port created");
}

static VOID
DeletePort(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ PWDI_MESSAGE_HEADER H)
{
    PBWL_WDI fe = (PBWL_WDI)W->Fe;
    WDI_MESSAGE_HEADER h = *H;

    BwlFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));          /* M3 */
    /* Windows disconnects first (TASK_DISCONNECT); the link is down here */
    fe->PortCreated = FALSE;
    W->AdapterUp = FALSE;
    TaskReply(W, WDI_INDICATION_DELETE_PORT_COMPLETE, &h, NDIS_STATUS_SUCCESS, NULL, 0);    /* M4 */
}

/* The messages only WDI has; the rest are wdicore.cpp's. */
BOOLEAN
BwlFeDispatch(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ UINT16 Id, _In_ PWDI_MESSAGE_HEADER H,
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
        BwlFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));
        TaskReply(W, Id == WDI_TASK_OPEN ? WDI_INDICATION_OPEN_COMPLETE : WDI_INDICATION_CLOSE_COMPLETE, &h,
                  NDIS_STATUS_SUCCESS, NULL, 0);
        return TRUE;
    case WDI_SET_POWER_STATE:
    case WDI_SET_AUTO_POWER_SAVE:
    case WDI_SET_RECEIVE_COALESCING:
    case WDI_SET_CLEAR_RECEIVE_COALESCING:
        /* accepted, nothing to do: no power save, no wake-up, no coalescing */
        BwlFeReqComplete(W, Request, STATUS_SUCCESS, sizeof(*H));
        return TRUE;
    default:
        return FALSE;
    }
}

/*
 * The worker's first job: the bring-up MiniportWdiOpenAdapter asked for
 * (seconds: firmware download), then the open's completion. Without
 * firmware the open fails and Windows gets no adapter this start; FwState
 * and LastStep in Device Parameters say why.
 */
VOID
BwlFeWorkerStart(_In_ PBWL_WIFI W)
{
    PBWL_WDI fe = (PBWL_WDI)W->Fe;
    PBWL_DEVICE_CONTEXT ctx = W->Ctx;
    NDIS_STATUS ns = NDIS_STATUS_ADAPTER_NOT_READY;
    NTSTATUS status;
    UCHAR fn = 0xFF;

    if (InterlockedExchange(&fe->OpenPending, 0) == 0) {
        return;
    }
    BwlCrumb(ctx, "wdi: open");
    status = BwlSdOpen(ctx);
    BwlPutULong(ctx, L"SdOpenStatus", (ULONG)status);
    if (NT_SUCCESS(status) && NT_SUCCESS(BwlSdGetProperty(ctx, SDP_FUNCTION_NUMBER, &fn, sizeof(fn)))) {
        ctx->Function = fn;
    }
    if (!NT_SUCCESS(status)) {
        BwlCrumb(ctx, "wdi: SDBUS open failed");
    } else if (ctx->Function != 1) {
        /* bcmwlwdi.inf binds FN_1 only; the card's other functions are not ours */
        BwlCrumb(ctx, "wdi: not SDIO function 1");
    } else {
        ctx->FwReady = BwlNetworkUp(ctx);
        W->HwReady = ctx->FwReady;
        if (ctx->FwReady) {
            ns = NDIS_STATUS_SUCCESS;
        }
    }
    fe->OpenStatus = ns;
    BwlPutULong(ctx, L"WdiOpenStatus", (ULONG)ns);
    BwlCrumb(ctx, ns == NDIS_STATUS_SUCCESS ? "wdi: open complete" : "wdi: open failed");
    fe->OpenComplete(fe->Adapter, ns);
}

/* --------------------------------------------------- control handlers */

static NDIS_STATUS
BwlWdiAllocateAdapter(_In_ NDIS_HANDLE NdisMiniportHandle, _In_ NDIS_HANDLE MiniportDriverContext,
                      _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters,
                      _In_ PNDIS_WDI_INIT_PARAMETERS NdisWdiInitParameters,
                      _Inout_ PNDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES RegistrationAttributes)
{
    WDF_OBJECT_ATTRIBUTES attr;
    PDEVICE_OBJECT pdo = NULL, fdo = NULL, next = NULL;
    PBWL_DEVICE_CONTEXT ctx;
    WDFDEVICE device;
    PBWL_WIFI W;
    PBWL_WDI fe;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(MiniportDriverContext);
    UNREFERENCED_PARAMETER(MiniportInitParameters);

    ParamCrumb(L"LastStep", L"AllocateAdapter", 0);
    ParamCrumb(L"OsWdiVersion", NULL, NdisWdiInitParameters->WdiVersion);
    NdisMGetDeviceProperty(NdisMiniportHandle, &pdo, &fdo, &next, NULL, NULL);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BWL_DEVICE_CONTEXT);
    status = WdfDeviceMiniportCreate(WdfGetDriver(), &attr, fdo, next, pdo, &device);
    ParamCrumb(L"MiniportCreateStatus", NULL, (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return NDIS_STATUS_FAILURE;
    }
    ctx = BwlGetContext(device);
    ctx->Device = device;
    ctx->Pdo = pdo;
    ctx->NextDevice = next;
    ctx->Function = 0xFF;
    ctx->AddFunction = 0xFF;
    ctx->WifiMode = TRUE;
    KeInitializeEvent(&ctx->WakeEvent, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&ctx->DataLock);
    ctx->RxHold = TRUE;
    status = DevKeyPath(ctx);
    ParamCrumb(L"DevKeyStatus", NULL, (ULONG)status);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return NDIS_STATUS_FAILURE;
    }
    BwlCrumb(ctx, "wdi: allocate adapter");
    BwlPutULong(ctx, L"OsWdiVersion", NdisWdiInitParameters->WdiVersion);
    BwlPutULong(ctx, L"ActiveMode", 1);

    /* Every CMD53 goes through this buffer: non-paged, for the MDLs */
    ctx->Bounce = (PUCHAR)BwlBufAlloc(POOL_FLAG_NON_PAGED, BWL_BOUNCE_MIN, "bounce");
    W = BwlWifiAlloc(ctx);
    fe = (PBWL_WDI)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BWL_WDI), BWL_WDI_TAG);
    status = BwlTalAlloc(ctx, NdisMiniportHandle);
    if (ctx->Bounce == NULL || W == NULL || fe == NULL || !NT_SUCCESS(status)) {
        if (fe != NULL) {
            ExFreePoolWithTag(fe, BWL_WDI_TAG);
        }
        BwlWdiFreeAdapter(ctx);
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
    BwlWifiAccept(W);

    RtlZeroMemory(RegistrationAttributes, sizeof(*RegistrationAttributes));
    RegistrationAttributes->Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    RegistrationAttributes->Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2;
    RegistrationAttributes->Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2;
    RegistrationAttributes->MiniportAdapterContext = ctx;
    RegistrationAttributes->AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK |
                                             NDIS_MINIPORT_ATTRIBUTES_NDIS_WDM |
                                             NDIS_MINIPORT_ATTRIBUTES_NO_PAUSE_ON_SUSPEND;
    RegistrationAttributes->InterfaceType = NdisInterfacePNPBus;
    BwlCrumb(ctx, "wdi: adapter allocated");
    return NDIS_STATUS_SUCCESS;
}

/* Quick: start the worker, which does the bring-up and completes the open. */
static NDIS_STATUS
BwlWdiOpenAdapter(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)MiniportAdapterContext;
    PBWL_WDI fe = (PBWL_WDI)ctx->Wifi->Fe;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(MiniportInitParameters);
    BwlCrumb(ctx, "wdi: open adapter");
    InterlockedExchange(&ctx->Stopping, 0);
    /* the INF sets Stage 3; a lower Stage stops the bring-up early (and the open fails) */
    ctx->Stage = BwlGetULong(ctx, L"Stage", BWL_STAGE_NETWORK);
    ctx->MaxBlocks = BwlGetULong(ctx, L"MaxBlocks", 32);
    ctx->F2BlockMode = BwlGetULong(ctx, L"F2BlockMode", 0);
    ctx->Verify = BwlGetULong(ctx, L"Verify", 1);
    ctx->PollMs = BwlGetULong(ctx, L"PollMs", 2);
    ctx->IdlePollMs = BwlGetULong(ctx, L"IdlePollMs", 10);
    BwlPutULong(ctx, L"StageSeen", ctx->Stage);
    InterlockedExchange(&fe->OpenPending, 1);
    status = BwlStartWorker(ctx);
    BwlPutULong(ctx, L"WorkerStartStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&fe->OpenPending, 0);
        return NDIS_STATUS_RESOURCES;
    }
    return NDIS_STATUS_SUCCESS;
}

/* The worker stops (running tasks end, waiting requests fail), then the chip. */
static NDIS_STATUS
BwlWdiCloseAdapter(_In_ NDIS_HANDLE MiniportAdapterContext)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)MiniportAdapterContext;
    PBWL_WDI fe = (PBWL_WDI)ctx->Wifi->Fe;

    BwlCrumb(ctx, "wdi: close adapter");
    BwlStopWorker(ctx);
    BwlWifiReleaseHardware(ctx);
    BwlStopChip(ctx);
    ctx->FwReady = FALSE;
    ctx->Bwl.FwRunning = 0;
    BwlSdClose(ctx);
    fe->PortCreated = FALSE;
    BwlCrumb(ctx, "wdi: closed");
    fe->CloseComplete(fe->Adapter, NDIS_STATUS_SUCCESS);
    return NDIS_STATUS_SUCCESS;
}

static VOID
BwlWdiFreeAdapter(_In_ NDIS_HANDLE MiniportAdapterContext)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)MiniportAdapterContext;
    PVOID fe = ctx->Wifi != NULL ? ctx->Wifi->Fe : NULL;

    BWL_LOG("FN%u teardown: free adapter", ctx->Function);
    BwlStopWorker(ctx);
    BwlWifiCleanup(ctx);                        /* fails what still waits, through fe */
    if (fe != NULL) {
        ExFreePoolWithTag(fe, BWL_WDI_TAG);
    }
    BwlNetFree(ctx);
    BwlSdClose(ctx);
    if (ctx->Bounce != NULL) {
        BwlBufFree(NULL, ctx->Bounce, "free adapter: bounce");
        ctx->Bounce = NULL;
    }
    if (ctx->DevKeyPath.Buffer != NULL) {
        ExFreePoolWithTag(ctx->DevKeyPath.Buffer, BWL_WDI_TAG);
        ctx->DevKeyPath.Buffer = NULL;
        ctx->DevKeyPath.Length = 0;
    }
    WdfObjectDelete(ctx->Device);               /* the context goes with it */
}

static NDIS_STATUS
BwlWdiHangDiagnose(_In_ NDIS_HANDLE MiniportDriverContext, _In_ eDiagnoseLevel DiagnoseLevel, _In_ UINT32 BufferSize,
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
BwlWdiOidRequest(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ PNDIS_OID_REQUEST OidRequest)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)MiniportAdapterContext;
    PBWL_WIFI W = ctx->Wifi;
    NTSTATUS fail;

    if (OidRequest->RequestType != NdisRequestMethod || !IsWdiOid(OidRequest->DATA.METHOD_INFORMATION.Oid)) {
        return NDIS_STATUS_NOT_SUPPORTED;
    }
    if (W == NULL || OidRequest->DATA.METHOD_INFORMATION.InformationBuffer == NULL ||
        OidRequest->DATA.METHOD_INFORMATION.InputBufferLength < sizeof(WDI_MESSAGE_HEADER)) {
        return NDIS_STATUS_INVALID_DATA;
    }
    if (!BwlWifiQueue(W, OidRequest, &fail)) {
        return NtToNdis(fail);
    }
    return NDIS_STATUS_PENDING;
}

static VOID
BwlWdiPnpEvent(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)MiniportAdapterContext;

    if (NetDevicePnPEvent->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved) {
        /* no more CMD52/CMD53, not even the CPU halt */
        BWL_LOG("FN%u teardown: surprise removal", ctx->Function);
        InterlockedExchange(&ctx->Gone, 1);
    }
}

static VOID
BwlWdiShutdown(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ NDIS_SHUTDOWN_ACTION ShutdownAction)
{
    /* may run at high IRQL: leave the card as it is */
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ShutdownAction);
}

/* ------------------------------------------------------------- driver */

static VOID
BwlWdiUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_Driver != NULL) {
        NdisMDeregisterWdiMiniportDriver(g_Driver);
        g_Driver = NULL;
    }
}

/*
 * KMDF first, without its dispatch routines (NDIS owns the IRPs), then the
 * WDI registration, which installs Microsoft's WDI layer above this driver.
 * Breadcrumbs in Services\bcmwlwdi\Parameters (PreEntry, LastStep, ...).
 */
extern "C" NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
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
    mc.UnloadHandler = BwlWdiUnload;
    mc.OidRequestHandler = BwlWdiOidRequest;
    mc.DevicePnPEventNotifyHandler = BwlWdiPnpEvent;
    mc.ShutdownHandlerEx = BwlWdiShutdown;

    RtlZeroMemory(&wc, sizeof(wc));
    wc.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_WDI_CHARACTERISTICS;
    wc.Header.Revision = NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS_REVISION_1;
    wc.Header.Size = NDIS_SIZEOF_MINIPORT_WDI_CHARACTERISTICS_REVISION_1;
    /* the WDI version this driver speaks; Parameters\WdiVersion overrides it on the board */
    wdi = ParamGet(L"WdiVersion", 0);
    wc.WdiVersion = wdi != 0 ? wdi : WDI_VERSION_LATEST;
    wc.AllocateAdapterHandler = BwlWdiAllocateAdapter;
    wc.FreeAdapterHandler = BwlWdiFreeAdapter;
    wc.OpenAdapterHandler = BwlWdiOpenAdapter;
    wc.CloseAdapterHandler = BwlWdiCloseAdapter;
    wc.HangDiagnoseHandler = BwlWdiHangDiagnose;
    wc.TalTxRxInitializeHandler = BwlTalInitialize;
    wc.TalTxRxDeinitializeHandler = BwlTalDeinitialize;
    ParamCrumb(L"DriverWdiVersion", NULL, wc.WdiVersion);

    ns = NdisMRegisterWdiMiniportDriver(DriverObject, RegistryPath, NULL, &mc, &wc, &g_Driver);
    ParamCrumb(L"RegisterStatus", NULL, (ULONG)ns);
    return ns == NDIS_STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
