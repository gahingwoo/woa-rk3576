/*
 * wificx.cpp - the WiFiCx 1.0 front end of bcmwlsdio (Stage 3,
 * bcmwlwifi.inf): Windows' own Wi-Fi on top of the Broadcom core, for
 * Windows 11 22H2/23H2 (WiFiCx 1.0, NetAdapterCx 2.1). The WDI messages
 * themselves are wdicore.cpp, shared with the Windows 10 WDI miniport
 * (wdi.cpp); this file moves them between WiFiCx and the core, reports the
 * capabilities and creates the station adapter (netadapter.c).
 *
 * EvtWifiDeviceSendCommand (IRQL <= DISPATCH_LEVEL) only queues the
 * request for the worker thread (BwlWifiQueue).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "wdicore.h"

static EVT_WIFI_DEVICE_SEND_COMMAND BwlEvtWifiSendCommand;
static EVT_WIFI_DEVICE_CREATE_ADAPTER BwlEvtWifiCreateAdapter;
static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE BwlEvtWifiCreateWifiDirectDevice;

/* ------------------------------------------------- requests, indications */

PWDI_MESSAGE_HEADER
BwlFeReqBuffer(_In_ BWL_REQ Request, _Out_ UINT *InLen, _Out_ UINT *OutLen)
{
    return (PWDI_MESSAGE_HEADER)WifiRequestGetInOutBuffer((WIFIREQUEST)Request, InLen, OutLen);
}

UINT16
BwlFeReqId(_In_ BWL_REQ Request)
{
    return WifiRequestGetMessageId((WIFIREQUEST)Request);
}

VOID
BwlFeReqComplete(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ NTSTATUS Status, _In_ ULONG Bytes)
{
    UNREFERENCED_PARAMETER(W);
    WifiRequestComplete((WIFIREQUEST)Request, Status, Bytes);
}

/* The header and the TLVs in a WDFMEMORY, which WiFiCx copies during the call. */
BOOLEAN
BwlFeIndicate(_In_ PBWL_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *Header,
              _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDFMEMORY mem;
    PWDI_MESSAGE_HEADER h;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = W->Ctx->Device;
    if (!NT_SUCCESS(WdfMemoryCreate(&attr, NonPagedPoolNx, BWL_WIFI_TAG, sizeof(*h) + TlvLen, &mem, (PVOID *)&h))) {
        return FALSE;
    }
    *h = *Header;
    if (Tlv != NULL && TlvLen != 0) {
        RtlCopyMemory(h + 1, Tlv, TlvLen);
    }
    WifiDeviceReceiveIndication(W->Ctx->Device, MessageId, mem);
    WdfObjectDelete(mem);
    return TRUE;
}

/* WiFiCx makes the ports itself (EvtWifiDeviceCreateAdapter) and has the
 * capabilities from BwlWifiReportCapabilities: nothing extra. */
BOOLEAN
BwlFeDispatch(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ UINT16 Id, _In_ PWDI_MESSAGE_HEADER H,
              _In_ UINT InLen, _In_ UINT OutLen)
{
    UNREFERENCED_PARAMETER(W);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Id);
    UNREFERENCED_PARAMETER(H);
    UNREFERENCED_PARAMETER(InLen);
    UNREFERENCED_PARAMETER(OutLen);
    return FALSE;
}

/* The firmware is up before the worker starts (PrepareHardware). */
VOID
BwlFeWorkerStart(_In_ PBWL_WIFI W)
{
    UNREFERENCED_PARAMETER(W);
}

/* IRQL <= DISPATCH_LEVEL: queue it and wake the worker, nothing else. */
static VOID
BwlEvtWifiSendCommand(_In_ WDFDEVICE Device, _In_ WIFIREQUEST SendRequest)
{
    PBWL_WIFI W = BwlGetContext(Device)->Wifi;
    NTSTATUS fail;

    if (W == NULL) {
        WifiRequestComplete(SendRequest, STATUS_DEVICE_NOT_READY, sizeof(WDI_MESSAGE_HEADER));
        return;
    }
    if (!BwlWifiQueue(W, SendRequest, &fail)) {
        WifiRequestComplete(SendRequest, fail, sizeof(WDI_MESSAGE_HEADER));
    }
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

    W = BwlWifiAlloc(Ctx);
    if (W == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

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

    if (W == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    BwlWifiAccept(W);
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

