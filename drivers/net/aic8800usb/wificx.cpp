/*
 * wificx.cpp - the WiFiCx 1.0 front end of aic8800usb (Mode 1, aic8800wifi.inf):
 * Windows' own Wi-Fi on top of the same core, for Windows 11 22H2/23H2
 * (WiFiCx 1.0 is the only version build 22621 has; NetAdapterCx 2.2).
 *
 * WiFiCx speaks WDI: each command is a WIFIREQUEST holding a
 * WDI_MESSAGE_HEADER and TLVs, which Microsoft's TLV library
 * (WificxTLVGenParse.lib, TlvGeneratorParser.hpp) parses and generates at
 * the version the OS speaks (WifiDeviceGetOsWdiVersion). A property
 * message is done with its completion (M3); a task gets its M3 when
 * accepted and later its completion indication (M4) with the same
 * transaction ID, the M3 always first. docs/dev/wificx.md has the model.
 *
 * Threads: EvtWifiDeviceSendCommand (IRQL <= DISPATCH_LEVEL) only queues
 * the request and wakes the worker thread (driver.c), which runs
 * AicWifiRun: one request at a time, each completed exactly once, and in
 * between the core's receive path (scan results, association, disconnect)
 * and the task timeouts. Every call into the core is made from that one
 * thread, as in Ethernet mode.
 *
 * The messages themselves are wdicore.cpp's, shared with the Windows 10
 * WDI miniport (wdi.cpp); this file keeps what only WiFiCx has: the
 * request and indication objects, the capabilities and the station
 * adapter.
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
 *   - The adapter is the station port; WiFiCx reports its link state.
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

static EVT_WIFI_DEVICE_SEND_COMMAND AicEvtWifiSendCommand;
static EVT_WIFI_DEVICE_CREATE_ADAPTER AicEvtWifiCreateAdapter;
static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE AicEvtWifiCreateWifiDirectDevice;

/* ------------------------------------------- requests, indications */

PWDI_MESSAGE_HEADER
AicFeReqBuffer(_In_ AIC_REQ Request, _Out_ UINT *InLen, _Out_ UINT *OutLen)
{
    return (PWDI_MESSAGE_HEADER)WifiRequestGetInOutBuffer((WIFIREQUEST)Request, InLen, OutLen);
}

UINT16
AicFeReqId(_In_ AIC_REQ Request)
{
    return WifiRequestGetMessageId((WIFIREQUEST)Request);
}

VOID
AicFeReqComplete(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ NTSTATUS Status, _In_ ULONG Bytes)
{
    UNREFERENCED_PARAMETER(W);
    WifiRequestComplete((WIFIREQUEST)Request, Status, Bytes);
}

/* The WDI header, then the TLVs, in a WDFMEMORY that WiFiCx copies during
 * the call. */
BOOLEAN
AicFeIndicate(_In_ PAIC_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *Header,
              _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDFMEMORY mem;
    PWDI_MESSAGE_HEADER h;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = W->Ctx->Device;
    if (!NT_SUCCESS(WdfMemoryCreate(&attr, NonPagedPoolNx, AIC_WIFI_TAG, sizeof(*h) + TlvLen, &mem, (PVOID *)&h))) {
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

/* WiFiCx has no messages of its own beyond the core's: capabilities and
 * the station port go through WiFiCx calls instead. */
BOOLEAN
AicFeDispatch(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ UINT16 Id, _In_ PWDI_MESSAGE_HEADER H,
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

/* The firmware init ran in PrepareHardware; nothing more here. */
VOID
AicFeWorkerStart(_In_ PAIC_WIFI W)
{
    UNREFERENCED_PARAMETER(W);
}

/* NetAdapterCx drives the queues itself. */
VOID
AicFeService(_In_ PAIC_WIFI W)
{
    UNREFERENCED_PARAMETER(W);
}

/* IRQL <= DISPATCH_LEVEL: queue it and wake the worker, nothing else. */
static VOID
AicEvtWifiSendCommand(_In_ WDFDEVICE Device, _In_ WIFIREQUEST SendRequest)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(Device);
    PAIC_WIFI W = ctx->Wifi;
    NTSTATUS fail = STATUS_SUCCESS;

    if (W == NULL) {
        WifiRequestComplete(SendRequest, STATUS_DEVICE_NOT_READY, sizeof(WDI_MESSAGE_HEADER));
        return;
    }
    if (!AicWifiQueue(W, (AIC_REQ)SendRequest, &fail)) {
        WifiRequestComplete(SendRequest, fail, sizeof(WDI_MESSAGE_HEADER));
    }
}

/* ------------------------------------------------------- device, adapter */

extern "C" NTSTATUS
AicWifiPreDeviceCreate(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    return WifiDeviceInitConfig(DeviceInit);
}

extern "C" NTSTATUS
AicWifiPostDeviceCreate(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    WIFI_DEVICE_CONFIG cfg;
    PAIC_WIFI W;
    ULONG wdi;
    NTSTATUS status;

    W = AicWifiAlloc(Ctx);              /* radio on, RX held */
    if (W == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* the WDI version this driver was built for; Parameters\WdiVersion
     * overrides it on the board if WiFiCx 1.0 refuses the newer number */
    wdi = AicGetULong(Ctx, L"WdiVersion", 0);
    WIFI_DEVICE_CONFIG_INIT(&cfg, wdi != 0 ? wdi : WDI_VERSION_LATEST, AicEvtWifiSendCommand,
                            AicEvtWifiCreateAdapter, AicEvtWifiCreateWifiDirectDevice);
    status = WifiDeviceInitialize(Ctx->Device, &cfg);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* parse and generate TLVs at the OS's version (INIT-1) */
    W->Tlv.AllocationContext = 0;
    W->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(Ctx->Device);
    AicPutULong(Ctx, L"OsWdiVersion", W->Tlv.PeerVersion);
    AicPutULong(Ctx, L"DriverWdiVersion", wdi != 0 ? wdi : WDI_VERSION_LATEST);
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

/* PrepareHardware, after the firmware init: what Windows may ask for. */
extern "C" NTSTATUS
AicWifiReportCapabilities(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    static const UINT16 cck[] = { 2, 4, 11, 22 };
    static const UINT16 erp[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    static const UINT16 ofdm[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    static const UCHAR ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                 132, 136, 140, 144, 149, 153, 157, 161, 165 };
    static UINT32 width20 = 20;
    static WDI_PHY_TYPE phys24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    static WDI_PHY_TYPE phys5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT };
    PAIC_WIFI W = Ctx->Wifi;
    aic_dev *d = &Ctx->Aic;
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
    W->HwReady = FALSE;
    /* Windows' supplicant gets the EAPOL frames; aic_wpa.c stays idle */
    d->HostAuth = 1;
    if (!Ctx->FwReady) {
        /* no firmware (Stage below 2, a missing file, a chip sent back to
         * its loader): Windows gets no Wi-Fi device this start */
        AicCrumb(Ctx, "wificx: firmware not running (Stage 2 or more needed)");
        return STATUS_DEVICE_NOT_READY;
    }

    /* the structures as the WDK's WiFiCx 1.0 header has them; the same
     * size as the 22621 WDK's (its 1.0 header has no _INIT functions) */
    RtlZeroMemory(&dev, sizeof(dev));
    dev.Size = sizeof(dev);
    dev.HardwareRadioState = TRUE;
    dev.SoftwareRadioState = W->RadioOn;
    for (i = 0; i < sizeof(dev.FirmwareVersion) - 1 && i < sizeof(d->FwVersion) && d->FwVersion[i]; i++) {
        dev.FirmwareVersion[i] = (UCHAR)d->FwVersion[i];
    }
    dev.NumRxStreams = 1;
    dev.NumTxStreams = 1;
    dev.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
    status = WifiDeviceSetDeviceCapabilities(Ctx->Device, &dev);
    AicPutULong(Ctx, L"WifiDeviceCapsStatus", (ULONG)status);
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
    sta.AutoPowerSaveMode = FALSE;              /* power save is off (ME_CONFIG) */
    sta.BSSListCachemanagement = FALSE;         /* Windows keeps the network list (SCAN-2) */
    sta.ConnectBSSSelectionOverride = FALSE;
    sta.NumSupportedUnicastAlgorithms = 2;
    sta.UnicastAlgorithmsList = unicast;
    sta.NumSupportedMulticastDataAlgorithms = 2;
    sta.MulticastDataAlgorithmsList = unicast;
    sta.NumSupportedMulticastMgmtAlgorithms = 1;
    sta.MulticastMgmtAlgorithmsList = mgmt;
    status = WifiDeviceSetStationCapabilities(Ctx->Device, &sta);
    AicPutULong(Ctx, L"WifiStationCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* the channels ME_CHAN_CONFIG gives the firmware (aic_wlan.c) */
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
    if (d->Is5gSupported) {
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
    AicPutULong(Ctx, L"WifiBandCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* WIFI_PHY_INFO holds 126 rates each: too big for the stack */
    phys = (WIFI_PHY_INFO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 * sizeof(WIFI_PHY_INFO), AIC_WIFI_TAG);
    if (phys == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    FillPhy(&phys[0], WDI_PHY_TYPE_HRDSSS, cck, (ULONG)ARRAYSIZE(cck));
    FillPhy(&phys[1], WDI_PHY_TYPE_ERP, erp, (ULONG)ARRAYSIZE(erp));
    FillPhy(&phys[2], WDI_PHY_TYPE_HT, erp, (ULONG)ARRAYSIZE(erp));
    n = 3;
    if (d->Is5gSupported) {
        FillPhy(&phys[3], WDI_PHY_TYPE_OFDM, ofdm, (ULONG)ARRAYSIZE(ofdm));
        n = 4;
    }
    RtlZeroMemory(&phyCaps, sizeof(phyCaps));
    phyCaps.Size = sizeof(phyCaps);
    phyCaps.NumPhyTypes = n;
    phyCaps.PhyInfoList = phys;
    status = WifiDeviceSetPhyCapabilities(Ctx->Device, &phyCaps);
    ExFreePoolWithTag(phys, AIC_WIFI_TAG);      /* WiFiCx keeps its own copy (INIT-2) */
    AicPutULong(Ctx, L"WifiPhyCapsStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    W->HwReady = TRUE;
    /* from here WDI requests queue for the worker, which PrepareHardware
     * starts next; before, or on a failure above, they are refused at once */
    AicWifiAccept(W);
    AicCrumb(Ctx, "wificx: capabilities reported");
    return STATUS_SUCCESS;
}

/* WiFiCx asks for the station port after PrepareHardware. PASSIVE_LEVEL. */
static NTSTATUS
AicEvtWifiCreateAdapter(_In_ WDFDEVICE Device, _Inout_ NETADAPTER_INIT *AdapterInit)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(Device);
    PAIC_WIFI W = ctx->Wifi;
    NTSTATUS status;

    if (W == NULL || WifiAdapterInitGetType(AdapterInit) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        return STATUS_NOT_SUPPORTED;            /* no Wi-Fi Direct roles */
    }
    if (!W->HwReady) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = AicNetCreateAdapter(ctx, AdapterInit);
    if (NT_SUCCESS(status)) {
        status = WifiAdapterInitialize(ctx->Adapter);
    }
    if (NT_SUCCESS(status)) {
        status = AicNetStart(ctx);              /* the module's address, the queues; no link state */
    }
    AicPutULong(ctx, L"AdapterStartStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    W->PortId = (UINT16)WifiAdapterGetPortId(ctx->Adapter);
    W->AdapterUp = TRUE;
    AicPutULong(ctx, L"WdiPortId", W->PortId);
    AicCrumb(ctx, "wificx: station adapter started");
    return STATUS_SUCCESS;
}

static NTSTATUS
AicEvtWifiCreateWifiDirectDevice(_In_ WDFDEVICE Device, _Inout_ WIFIDIRECT_DEVICE_INIT *WifiDirectDeviceInit)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(WifiDirectDeviceInit);
    return STATUS_NOT_SUPPORTED;                /* no Wi-Fi Direct capabilities are reported */
}
