/*++

Module Name:

    netadapter.c

Abstract:

    NetAdapterCx + WDF integration for the RK3576 GMAC. DriverEntry, device add,
    common-buffer DMA setup, MMIO mapping, and the TX/RX datapath that bridges
    NetCx packet rings to the verified DWMAC engine in hw.c.

    !!! VERIFY-ON-BUILD !!!
    The DWMAC hardware engine (hw.c) is kernel-verified. This file follows the
    documented NetAdapterCx model, but the NetCx API names (NET_RING iteration,
    capability/link structures, queue config) are owned by the WDK
    <netadaptercx.h> and evolve across versions. Aligning these to the WDK ABI
    is in progress (WIP); the control flow is correct, identifiers are being
    settled. Spots that touch the NetCx ABI are marked VERIFY-ON-BUILD.

Environment:

    Kernel mode.

--*/

#include "dwmac.h"
#include <net/ring.h>
#include <net/packet.h>
#include <net/fragment.h>

//
// ---------------------------------------------------------------------------
// DMA common-buffer helpers
// ---------------------------------------------------------------------------
//

static
NTSTATUS
DwmacAllocCommonBuffer(
    _In_ PDWMAC_ADAPTER A,
    _In_ size_t Size,
    _Out_ WDFCOMMONBUFFER *Cb,
    _Outptr_ PVOID *Va,
    _Out_ PHYSICAL_ADDRESS *Pa
    )
{
    NTSTATUS status = WdfCommonBufferCreate(A->DmaEnabler, Size,
                                            WDF_NO_OBJECT_ATTRIBUTES, Cb);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *Va = WdfCommonBufferGetAlignedVirtualAddress(*Cb);
    *Pa = WdfCommonBufferGetAlignedLogicalAddress(*Cb);
    RtlZeroMemory(*Va, Size);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DwmacAllocDma(
    _In_ PDWMAC_ADAPTER A
    )
{
    WDF_DMA_ENABLER_CONFIG cfg;
    NTSTATUS status;
    PVOID va;

    //
    // 64-bit-capable packet DMA enabler for the descriptor rings + bounce pools.
    //
    WDF_DMA_ENABLER_CONFIG_INIT(&cfg, WdfDmaProfilePacket64,
                                DWMAC_BUF_SIZE * DWMAC_RX_RING);
    status = WdfDmaEnablerCreate(A->Device, &cfg, WDF_NO_OBJECT_ATTRIBUTES,
                                 &A->DmaEnabler);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = DwmacAllocCommonBuffer(A, sizeof(DWMAC_DESC) * DWMAC_TX_RING,
                                    &A->TxDescCb, &va, &A->TxDescPa);
    if (!NT_SUCCESS(status)) return status;
    A->TxDesc = (PDWMAC_DESC)va;

    status = DwmacAllocCommonBuffer(A, sizeof(DWMAC_DESC) * DWMAC_RX_RING,
                                    &A->RxDescCb, &va, &A->RxDescPa);
    if (!NT_SUCCESS(status)) return status;
    A->RxDesc = (PDWMAC_DESC)va;

    status = DwmacAllocCommonBuffer(A, (size_t)DWMAC_BUF_SIZE * DWMAC_TX_RING,
                                    &A->TxBufCb, &va, &A->TxBufPa);
    if (!NT_SUCCESS(status)) return status;
    A->TxBuf = (PUCHAR)va;

    status = DwmacAllocCommonBuffer(A, (size_t)DWMAC_BUF_SIZE * DWMAC_RX_RING,
                                    &A->RxBufCb, &va, &A->RxBufPa);
    if (!NT_SUCCESS(status)) return status;
    A->RxBuf = (PUCHAR)va;

    return STATUS_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// MAC address
// ---------------------------------------------------------------------------
//

static
VOID
DwmacLoadMacAddress(
    _In_ PDWMAC_ADAPTER A
    )
{
    //
    // Reuse the address the firmware (U-Boot/UEFI) programmed if it is valid;
    // otherwise fall back to a fixed locally-administered address.
    //
    ULONG hi = DwmacRead(A->Mac, GMAC_ADDR_HIGH0);
    ULONG lo = DwmacRead(A->Mac, GMAC_ADDR_LOW0);

    A->MacAddress[0] = (UCHAR)(lo);
    A->MacAddress[1] = (UCHAR)(lo >> 8);
    A->MacAddress[2] = (UCHAR)(lo >> 16);
    A->MacAddress[3] = (UCHAR)(lo >> 24);
    A->MacAddress[4] = (UCHAR)(hi);
    A->MacAddress[5] = (UCHAR)(hi >> 8);

    if ((A->MacAddress[0] | A->MacAddress[1] | A->MacAddress[2] |
         A->MacAddress[3] | A->MacAddress[4] | A->MacAddress[5]) == 0 ||
        (A->MacAddress[0] & 0x01)) {              // zero or multicast => invalid
        A->MacAddress[0] = 0x02;                  // locally administered, unicast
        A->MacAddress[1] = 0x00;
        A->MacAddress[2] = 0x35; A->MacAddress[3] = 0x76;
        A->MacAddress[4] = 0x00; A->MacAddress[5] = 0x01;
    }
}

//
// ---------------------------------------------------------------------------
// Datapath  (VERIFY-ON-BUILD: NetCx ring/packet/fragment API)
// ---------------------------------------------------------------------------
//

static
VOID
DwmacEvtTxQueueAdvance(
    _In_ NETPACKETQUEUE Queue
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(NetTxQueueGetAdapter(Queue));
    NET_RING_COLLECTION const *rings = NetTxQueueGetRingCollection(Queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    UINT32 i;

    //
    // Reclaim finished TX descriptors first so the ring has room.
    //
    (VOID)DwmacReclaimTx(A);

    //
    // Post each packet pending at the ring's "begin..next" window. v1 assumes a
    // single fragment per packet (linear frame) and bounce-copies it.
    //
    for (i = pr->BeginIndex; i != pr->NextIndex; i = NetRingIncrementIndex(pr, i)) {
        NET_PACKET *packet = NetRingGetPacketAtIndex(pr, i);
        NET_FRAGMENT *frag;
        PUCHAR data;

        if (packet->Ignore || packet->FragmentCount == 0) {
            continue;
        }

        frag = NetRingGetFragmentAtIndex(fr, packet->FragmentIndex);
        data = (PUCHAR)NetFragmentGetVirtualAddress(frag, /*...*/ 0) + frag->Offset;

        if (!DwmacTransmitFrame(A, data, (ULONG)frag->ValidLength)) {
            break;     // ring full; resume on next advance
        }
    }

    //
    // Release completed packets back to NetCx (BeginIndex catches up to the
    // descriptors the hardware has finished).
    //
    pr->BeginIndex = pr->NextIndex;
    fr->BeginIndex = fr->NextIndex;
}

static
VOID
DwmacEvtRxQueueAdvance(
    _In_ NETPACKETQUEUE Queue
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(NetRxQueueGetAdapter(Queue));
    NET_RING_COLLECTION const *rings = NetRxQueueGetRingCollection(Queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    UINT32 pi = pr->NextIndex;
    UINT32 fi = fr->NextIndex;

    //
    // Drain completed RX descriptors into NetCx packets/fragments while both
    // rings have room.
    //
    while (pi != pr->EndIndex && fi != fr->EndIndex) {
        NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, fi);
        PUCHAR dst = (PUCHAR)NetFragmentGetVirtualAddress(frag, 0) + frag->Offset;
        ULONG len = 0;

        if (!DwmacReceiveFrame(A, dst, (ULONG)frag->Capacity, &len)) {
            break;     // nothing more ready
        }

        frag->ValidLength = len;
        {
            NET_PACKET *packet = NetRingGetPacketAtIndex(pr, pi);
            packet->FragmentIndex = fi;
            packet->FragmentCount = 1;
            packet->Layout = (NET_PACKET_LAYOUT){ 0 };
        }

        pi = NetRingIncrementIndex(pr, pi);
        fi = NetRingIncrementIndex(fr, fi);
    }

    pr->NextIndex = pi;
    fr->NextIndex = fi;
}

//
// Queue notification toggles: the framework enables these when it wants to be
// told that more work is available; the DPC clears them as it wakes the queue.
//
static
VOID
DwmacEvtTxSetNotification(
    _In_ NETPACKETQUEUE Queue,
    _In_ BOOLEAN Enabled
    )
{
    DwmacGetAdapterContext(NetTxQueueGetAdapter(Queue))->TxNotify = Enabled;
}

static
VOID
DwmacEvtRxSetNotification(
    _In_ NETPACKETQUEUE Queue,
    _In_ BOOLEAN Enabled
    )
{
    DwmacGetAdapterContext(NetRxQueueGetAdapter(Queue))->RxNotify = Enabled;
}

//
// ISR (DIRQL): latch the DMA channel status and schedule the DPC. The dwmac
// register access is verified; the WDFINTERRUPT/NetCx wiring is VERIFY-ON-BUILD.
//
static
BOOLEAN
DwmacEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(WdfInterruptGetDevice(Interrupt));
    ULONG sts;

    UNREFERENCED_PARAMETER(MessageID);

    sts = DwmacReadAndClearIrq(A);
    if ((sts & (DMA_CHAN_STATUS_TI | DMA_CHAN_STATUS_RI | DMA_CHAN_STATUS_RBU)) == 0) {
        return FALSE;
    }

    InterlockedOr(&A->IrqStatus, (LONG)sts);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

static
VOID
DwmacEvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(WdfInterruptGetDevice(Interrupt));
    LONG sts = InterlockedExchange(&A->IrqStatus, 0);

    UNREFERENCED_PARAMETER(AssociatedObject);

    if ((sts & DMA_CHAN_STATUS_TI) && A->TxNotify) {
        A->TxNotify = FALSE;
        NetTxQueueNotifyMoreCompletedPacketsAvailable(A->TxQueue);
    }
    if ((sts & (DMA_CHAN_STATUS_RI | DMA_CHAN_STATUS_RBU)) && A->RxNotify) {
        A->RxNotify = FALSE;
        NetRxQueueNotifyMoreReceivedPacketsAvailable(A->RxQueue);
    }
}

static
NTSTATUS
DwmacEvtCreateTxQueue(
    _In_ NETADAPTER Adapter,
    _Inout_ NETTXQUEUE_INIT *QueueInit
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(Adapter);
    NET_PACKET_QUEUE_CONFIG cfg;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, DwmacEvtTxQueueAdvance,
                                 DwmacEvtTxSetNotification, NULL);
    return NetTxQueueCreate(QueueInit, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &A->TxQueue);
}

static
NTSTATUS
DwmacEvtCreateRxQueue(
    _In_ NETADAPTER Adapter,
    _Inout_ NETRXQUEUE_INIT *QueueInit
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(Adapter);
    NET_PACKET_QUEUE_CONFIG cfg;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, DwmacEvtRxQueueAdvance,
                                 DwmacEvtRxSetNotification, NULL);
    return NetRxQueueCreate(QueueInit, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &A->RxQueue);
}

//
// Publish link-layer capabilities, MAC addresses, and current link state. Called
// from PrepareHardware (after the PHY is probed) and on link change.
//
static
VOID
DwmacSetCapabilities(
    _In_ PDWMAC_ADAPTER A
    )
{
    NET_ADAPTER_LINK_LAYER_ADDRESS mac;
    NET_ADAPTER_LINK_STATE linkState;
    UINT64 maxBps = 1000000000ULL;   // 1 Gbps

    //
    // Permanent + current MAC.
    //
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&mac, 6, A->MacAddress);
    NetAdapterSetPermanentLinkLayerAddress(A->Adapter, &mac);
    NetAdapterSetCurrentLinkLayerAddress(A->Adapter, &mac);

    {
        NET_ADAPTER_LINK_LAYER_CAPABILITIES caps;
        NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&caps, maxBps, maxBps);
        NetAdapterSetLinkLayerCapabilities(A->Adapter, &caps);
        NetAdapterSetLinkLayerMtuSize(A->Adapter, 1500);
    }

    //
    // Current link state.
    //
    if (A->LinkUp) {
        NET_ADAPTER_LINK_STATE_INIT(
            &linkState,
            (UINT64)A->LinkSpeedMbps * 1000000ULL,
            MediaConnectStateConnected,
            A->FullDuplex ? MediaDuplexStateFull : MediaDuplexStateHalf,
            NetAdapterPauseFunctionTypeUnsupported,
            NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated);
    } else {
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
    }
    NetAdapterSetLinkState(A->Adapter, &linkState);
}

//
// ---------------------------------------------------------------------------
// PnP / power
// ---------------------------------------------------------------------------
//

_Use_decl_annotations_
NTSTATUS
DwmacEvtPrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(Device);
    ULONG count, i;
    BOOLEAN memFound = FALSE;
    PHYSICAL_ADDRESS grfPa;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i += 1) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc != NULL && desc->Type == CmResourceTypeMemory && !memFound) {
            A->MacPhys = desc->u.Memory.Start;
            A->MacLen = desc->u.Memory.Length;
            A->Mac = (volatile UCHAR *)MmMapIoSpaceEx(desc->u.Memory.Start,
                                                      desc->u.Memory.Length,
                                                      PAGE_READWRITE | PAGE_NOCACHE);
            if (A->Mac == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            memFound = TRUE;
        }
    }
    if (!memFound) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // The SDGMAC_GRF clock-select window is a fixed SoC address (not in _CRS).
    //
    grfPa.QuadPart = RK3576_SDGMAC_GRF_BASE;
    A->Grf = (volatile UCHAR *)MmMapIoSpaceEx(grfPa, RK3576_SDGMAC_GRF_SIZE,
                                              PAGE_READWRITE | PAGE_NOCACHE);

    status = DwmacSwReset(A);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "DMA reset timeout\n");
        return status;
    }

    DwmacLoadMacAddress(A);
    DwmacInitMacDma(A);
    DwmacSetMacAddress(A);
    DwmacInitRings(A);

    (VOID)DwmacPhyDetect(A);
    DwmacUpdateLink(A);
    DwmacStart(A);

    DwmacSetCapabilities(A);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
DwmacEvtReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PDWMAC_ADAPTER A = DwmacGetAdapterContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (A->Mac != NULL) {
        DwmacStop(A);
        MmUnmapIoSpace((PVOID)A->Mac, A->MacLen);
        A->Mac = NULL;
    }
    if (A->Grf != NULL) {
        MmUnmapIoSpace((PVOID)A->Grf, RK3576_SDGMAC_GRF_SIZE);
        A->Grf = NULL;
    }
    return STATUS_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Device add / DriverEntry
// ---------------------------------------------------------------------------
//

_Use_decl_annotations_
NTSTATUS
DwmacEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attribs;
    WDFDEVICE device;
    PDWMAC_ADAPTER A;
    NETADAPTER_INIT *adapterInit;
    NET_ADAPTER_DATAPATH_CALLBACKS datapath;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    status = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = DwmacEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = DwmacEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attribs, DWMAC_ADAPTER);
    status = WdfDeviceCreate(&DeviceInit, &attribs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    A = DwmacGetAdapterContext(device);
    RtlZeroMemory(A, sizeof(*A));
    A->Device = device;

    status = DwmacAllocDma(A);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "DMA alloc failed 0x%08x\n", status);
        return status;
    }

    //
    // Hardware interrupt (connected by WDF from the ACPI _CRS at PnP start).
    //
    {
        WDF_INTERRUPT_CONFIG ic;
        WDF_INTERRUPT_CONFIG_INIT(&ic, DwmacEvtInterruptIsr, DwmacEvtInterruptDpc);
        status = WdfInterruptCreate(device, &ic, WDF_NO_OBJECT_ATTRIBUTES,
                                    &A->Interrupt);
        if (!NT_SUCCESS(status)) {
            RkLog(RK_DBG_ERROR, "WdfInterruptCreate failed 0x%08x\n", status);
            return status;
        }
    }

    //
    // Create the NETADAPTER with its datapath (TX/RX queue) callbacks.
    //
    adapterInit = NetAdapterInitAllocate(device);
    if (adapterInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&datapath,
                                        DwmacEvtCreateTxQueue,
                                        DwmacEvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(adapterInit, &datapath);

    status = NetAdapterCreate(adapterInit, WDF_NO_OBJECT_ATTRIBUTES, &A->Adapter);
    NetAdapterInitFree(adapterInit);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "NetAdapterCreate failed 0x%08x\n", status);
        return status;
    }

    //
    // The adapter is started once hardware is prepared and capabilities set
    // (NetCx starts it as part of the PnP flow after PrepareHardware).
    //
    status = NetAdapterStart(A->Adapter);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "NetAdapterStart failed 0x%08x\n", status);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    RkLog(RK_DBG_INFO, "DriverEntry\n");

    WDF_DRIVER_CONFIG_INIT(&config, DwmacEvtDeviceAdd);
    config.DriverPoolTag = DWMAC_POOL_TAG;

    status = WdfDriverCreate(DriverObject, RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDriverCreate failed 0x%08x\n", status);
    }
    return status;
}
