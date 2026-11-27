/*
 * netadapter.c - the network adapter of the runtime instance (PID_8D81
 * MI_02): a NetAdapterCx client that looks like an Ethernet card to
 * Windows, the way drivers/net/dwc_eqos does it, with the Wi-Fi link
 * underneath (joined by the worker thread, stage 3).
 *
 * TX: EvtPacketQueueAdvance gathers each packet's fragments into one
 * Ethernet frame, turns it into the firmware's data frame (aic_build_tx)
 * in one of AIC_TX_SLOTS preallocated USB requests and sends it
 * asynchronously on the data pipe; the packet is done as soon as it is
 * copied. When every slot is in flight the queue stops taking packets and
 * the completion of a slot asks NetAdapterCx for more: that is the TX flow
 * control.
 *
 * RX: the worker thread runs the core's receive path; each Ethernet frame
 * it produces (ops->rx_eth) goes into an aic_frameq, and
 * EvtPacketQueueAdvance copies queued frames into the RX ring's buffers
 * (system-managed, one fragment per packet).
 *
 * Frames move only from stage 4 on, and only while the link is up.
 *
 * WiFiCx mode (Mode 1, wificx.cpp): the same queues, but Windows hands
 * over and takes back native 802.11 data frames (aic_80211_to_eth on TX,
 * aic_eth_to_80211 on RX), WiFiCx creates the adapter and reports the
 * link itself, and frames move from association on (the EAPOL frames of
 * Windows' 4-way handshake come first). RX frames that arrive before the
 * association has been indicated to Windows wait in the FIFO (RxHold).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic8800usb.h"

#define AIC_LINK_SPEED (65ull * 1000 * 1000)    /* HT20 MCS 7, long GI */

typedef struct _AIC_QUEUE_CONTEXT {
    PAIC_DEVICE_CONTEXT Device;
    NET_RING *Packets;
    NET_RING *Fragments;
    NET_EXTENSION VirtualAddress;
} AIC_QUEUE_CONTEXT, *PAIC_QUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIC_QUEUE_CONTEXT, AicQueueGetContext)

static EVT_NET_ADAPTER_CREATE_TXQUEUE AicEvtCreateTxQueue;
static EVT_NET_ADAPTER_CREATE_RXQUEUE AicEvtCreateRxQueue;
static EVT_NET_ADAPTER_SET_RECEIVE_FILTER AicEvtSetReceiveFilter;
static EVT_PACKET_QUEUE_ADVANCE AicEvtTxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED AicEvtTxNotify;
static EVT_PACKET_QUEUE_CANCEL AicEvtTxCancel;
static EVT_PACKET_QUEUE_ADVANCE AicEvtRxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED AicEvtRxNotify;
static EVT_PACKET_QUEUE_CANCEL AicEvtRxCancel;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE AicTxDone;

static BOOLEAN
DataPathOpen(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Mode == AIC_MODE_WIFICX) {
        return (Ctx->Aic.Link == AIC_LINK_ASSOCIATED || Ctx->Aic.Link == AIC_LINK_UP) && !Ctx->Stopping;
    }
    return Ctx->Stage >= AIC_STAGE_ADAPTER && Ctx->Aic.Link == AIC_LINK_UP && !Ctx->Stopping;
}

/* No USB write in flight (before a pairwise key: message 4 must leave
 * unencrypted, as wpa_supplicant waits for its TX status on Linux). */
BOOLEAN
AicNetTxIdle(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    BOOLEAN idle;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    idle = Ctx->TxFreeMask == 0xFFFFFFFFu;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    return idle;
}

/* ------------------------------------------------------------------ TX */

static VOID
AicTxDone(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
          _In_ WDFCONTEXT Context)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(WdfIoTargetGetDevice(Target));
    ULONG slot = (ULONG)(ULONG_PTR)Context;
    WDF_REQUEST_REUSE_PARAMS reuse;
    NETPACKETQUEUE notify = NULL;
    KIRQL irql;

    if (!NT_SUCCESS(Params->IoStatus.Status)) {
        InterlockedIncrement((volatile LONG *)&ctx->TxErrors);
    }
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    (VOID)WdfRequestReuse(Request, &reuse);
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->TxFreeMask |= 1u << slot;
    if (ctx->TxNotify && ctx->TxQueue != NULL) {
        notify = ctx->TxQueue;
        WdfObjectReference(notify);     /* the queue may be deleted once the lock is dropped */
        ctx->TxNotify = FALSE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (notify) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(notify);
        WdfObjectDereference(notify);
    }
}

/* Take a free slot, or return AIC_TX_SLOTS. */
static ULONG
TxTakeSlot(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    ULONG slot = AIC_TX_SLOTS, i;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    for (i = 0; i < AIC_TX_SLOTS; i++) {
        if (Ctx->TxFreeMask & (1u << i)) {
            Ctx->TxFreeMask &= ~(1u << i);
            slot = i;
            break;
        }
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    return slot;
}

static VOID
TxGiveSlot(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ ULONG Slot)
{
    KIRQL irql;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    Ctx->TxFreeMask |= 1u << Slot;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
}

/* Send one gathered Ethernet frame. FALSE if no slot is free (try later). */
static BOOLEAN
TxSend(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_reads_(Len) const UCHAR *Eth, _In_ ULONG Len)
{
    WDFMEMORY_OFFSET range;
    ULONG slot = TxTakeSlot(Ctx);
    NTSTATUS status;
    int n;

    if (slot == AIC_TX_SLOTS) {
        return FALSE;
    }
    n = aic_build_tx(&Ctx->Aic, Eth, Len, Ctx->TxSlotBuf[slot], AIC_TX_SLOT_SIZE);
    if (n <= 0) {
        TxGiveSlot(Ctx, slot);
        Ctx->TxDrops++;
        return TRUE;
    }
    range.BufferOffset = 0;
    range.BufferLength = (size_t)n;
    status = WdfUsbTargetPipeFormatRequestForWrite(Ctx->OutPipe[0], Ctx->TxReq[slot], Ctx->TxMem[slot], &range);
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(Ctx->TxReq[slot], AicTxDone, (WDFCONTEXT)(ULONG_PTR)slot);
        if (WdfRequestSend(Ctx->TxReq[slot], WdfUsbTargetPipeGetIoTarget(Ctx->OutPipe[0]), WDF_NO_SEND_OPTIONS)) {
            Ctx->TxFrames++;
            /* an EAPOL frame counts as answered from here (SetAddCipherKeys) */
            aic_tx_note(&Ctx->Aic, Eth, Len);
            return TRUE;
        }
    }
    {
        WDF_REQUEST_REUSE_PARAMS reuse;

        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
        (VOID)WdfRequestReuse(Ctx->TxReq[slot], &reuse);
    }
    TxGiveSlot(Ctx, slot);
    Ctx->TxErrors++;
    Ctx->TxDrops++;
    return TRUE;
}

static VOID
AicEvtTxAdvance(_In_ NETPACKETQUEUE Queue)
{
    PAIC_QUEUE_CONTEXT q = AicQueueGetContext(Queue);
    PAIC_DEVICE_CONTEXT ctx = q->Device;
    NET_RING *pr = q->Packets, *fr = q->Fragments;
    UINT32 i = pr->BeginIndex;
    BOOLEAN open = DataPathOpen(ctx);

    while (i != pr->EndIndex) {
        NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, i);

        if (!pkt->Ignore && open) {
            ULONG len = 0, k;
            UINT32 f = pkt->FragmentIndex;
            BOOLEAN fits = TRUE;

            for (k = 0; k < pkt->FragmentCount; k++) {
                NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, f);
                const UCHAR *va = (const UCHAR *)NetExtensionGetFragmentVirtualAddress(&q->VirtualAddress, f)
                                      ->VirtualAddress + frag->Offset;

                if (len + frag->ValidLength > sizeof(ctx->TxGather)) {
                    fits = FALSE;
                    break;
                }
                RtlCopyMemory(ctx->TxGather + len, va, (SIZE_T)frag->ValidLength);
                len += (ULONG)frag->ValidLength;
                f = NetRingIncrementIndex(fr, f);
            }
            if (fits && ctx->Mode == AIC_MODE_WIFICX) {
                /* a native 802.11 data frame from Windows */
                int n = aic_80211_to_eth(ctx->TxGather, len, ctx->TxEth, (u32)sizeof(ctx->TxEth));

                if (n > 0) {
                    RtlCopyMemory(ctx->TxGather, ctx->TxEth, (SIZE_T)n);
                    len = (ULONG)n;
                } else {
                    len = 0;    /* not a frame to send: dropped below */
                }
            }
            if (fits && len >= 14) {
                if (!TxSend(ctx, ctx->TxGather, len)) {
                    /* every slot in flight: ask to be called again */
                    KIRQL irql;

                    KeAcquireSpinLock(&ctx->DataLock, &irql);
                    if (ctx->TxFreeMask == 0) {
                        ctx->TxNotify = TRUE;
                        KeReleaseSpinLock(&ctx->DataLock, irql);
                        break;
                    }
                    KeReleaseSpinLock(&ctx->DataLock, irql);
                    continue;   /* a slot came back meanwhile */
                }
            } else {
                ctx->TxDrops++;
            }
        } else if (!pkt->Ignore) {
            ctx->TxDrops++;     /* no link: dropped, as a cable-less NIC would */
        }
        fr->BeginIndex = NetRingAdvanceIndex(fr, pkt->FragmentIndex, pkt->FragmentCount);
        i = NetRingIncrementIndex(pr, i);
    }
    pr->BeginIndex = i;
    pr->NextIndex = i;
    fr->NextIndex = fr->BeginIndex;
}

static VOID
AicEvtTxNotify(_In_ NETPACKETQUEUE Queue, _In_ BOOLEAN Enabled)
{
    PAIC_DEVICE_CONTEXT ctx = AicQueueGetContext(Queue)->Device;
    KIRQL irql;
    BOOLEAN now = FALSE;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->TxNotify = Enabled;
    if (Enabled && ctx->TxFreeMask != 0) {
        ctx->TxNotify = FALSE;
        now = TRUE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (now) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(Queue);
    }
}

static VOID
AicEvtTxCancel(_In_ NETPACKETQUEUE Queue)
{
    PAIC_QUEUE_CONTEXT q = AicQueueGetContext(Queue);

    q->Packets->BeginIndex = q->Packets->NextIndex = q->Packets->EndIndex;
    q->Fragments->BeginIndex = q->Fragments->NextIndex = q->Fragments->EndIndex;
}

static VOID
QueueCleanup(_In_ WDFOBJECT Object)
{
    PAIC_QUEUE_CONTEXT q = AicQueueGetContext((NETPACKETQUEUE)Object);
    KIRQL irql;

    KeAcquireSpinLock(&q->Device->DataLock, &irql);
    if (q->Device->TxQueue == (NETPACKETQUEUE)Object) {
        q->Device->TxQueue = NULL;
        q->Device->TxNotify = FALSE;
    }
    if (q->Device->RxQueue == (NETPACKETQUEUE)Object) {
        q->Device->RxQueue = NULL;
        q->Device->RxNotify = FALSE;
    }
    KeReleaseSpinLock(&q->Device->DataLock, irql);
}

static NTSTATUS
AicEvtCreateTxQueue(_In_ NETADAPTER Adapter, _Inout_ NETTXQUEUE_INIT *Init)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(*AicAdapterGetDevice(Adapter));
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NET_EXTENSION_QUERY query;
    const NET_RING_COLLECTION *rings;
    NETPACKETQUEUE queue;
    PAIC_QUEUE_CONTEXT q;
    NTSTATUS status;
    KIRQL irql;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, AicEvtTxAdvance, AicEvtTxNotify, AicEvtTxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AIC_QUEUE_CONTEXT);
    attr.EvtCleanupCallback = QueueCleanup;
    status = NetTxQueueCreate(Init, &attr, &cfg, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    q = AicQueueGetContext(queue);
    q->Device = ctx;
    rings = NetTxQueueGetRingCollection(queue);
    q->Packets = NetRingCollectionGetPacketRing(rings);
    q->Fragments = NetRingCollectionGetFragmentRing(rings);
    NET_EXTENSION_QUERY_INIT(&query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetTxQueueGetExtension(queue, &query, &q->VirtualAddress);
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->TxQueue = queue;
    KeReleaseSpinLock(&ctx->DataLock, irql);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ RX */

/* ops->rx_eth: called by the core on the worker thread. */
VOID
AicNetRxFrame(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_reads_(Len) const UCHAR *Frame, _In_ ULONG Len)
{
    NETPACKETQUEUE notify = NULL;
    KIRQL irql;

    if (!DataPathOpen(Ctx) || Ctx->RxQ == NULL) {
        Ctx->RxDrops++;
        return;
    }
    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (Ctx->RxQueue == NULL || !aic_frameq_push(Ctx->RxQ, Frame, Len)) {
        Ctx->RxDrops++;
    } else if (Ctx->RxNotify && !Ctx->RxHold) {
        notify = Ctx->RxQueue;
        WdfObjectReference(notify);
        Ctx->RxNotify = FALSE;
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (notify) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(notify);
        WdfObjectDereference(notify);
    }
}

/* WiFiCx: the association is indicated; let the frames that waited go up. */
VOID
AicNetRxRelease(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    NETPACKETQUEUE notify = NULL;
    KIRQL irql;
    UINT32 len;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    Ctx->RxHold = FALSE;
    if (Ctx->RxNotify && Ctx->RxQueue && Ctx->RxQ && aic_frameq_peek(Ctx->RxQ, &len) != NULL) {
        notify = Ctx->RxQueue;
        WdfObjectReference(notify);
        Ctx->RxNotify = FALSE;
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (notify) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(notify);
        WdfObjectDereference(notify);
    }
}

static VOID
AicEvtRxAdvance(_In_ NETPACKETQUEUE Queue)
{
    PAIC_QUEUE_CONTEXT q = AicQueueGetContext(Queue);
    PAIC_DEVICE_CONTEXT ctx = q->Device;
    NET_RING *pr = q->Packets, *fr = q->Fragments;
    UINT32 pi = pr->BeginIndex, fi = fr->BeginIndex;
    KIRQL irql;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    while (pi != pr->EndIndex && fi != fr->EndIndex && !ctx->RxHold) {
        NET_PACKET *pkt;
        NET_FRAGMENT *frag;
        const UCHAR *data;
        PUCHAR va;
        UINT32 len = 0;

        data = ctx->RxQ ? aic_frameq_peek(ctx->RxQ, &len) : NULL;
        if (data == NULL) {
            break;
        }
        pkt = NetRingGetPacketAtIndex(pr, pi);
        frag = NetRingGetFragmentAtIndex(fr, fi);
        va = (PUCHAR)NetExtensionGetFragmentVirtualAddress(&q->VirtualAddress, fi)->VirtualAddress;
        if (ctx->Mode == AIC_MODE_WIFICX) {
            /* to the native 802.11 data frame Windows wants */
            int n = aic_eth_to_80211(ctx->Aic.Bssid, data, len, va, (u32)frag->Capacity);

            aic_frameq_pop(ctx->RxQ);
            if (n <= 0) {
                ctx->RxDrops++;
                continue;
            }
            len = (UINT32)n;
        } else {
            if (len > frag->Capacity) {
                aic_frameq_pop(ctx->RxQ);
                ctx->RxDrops++;
                continue;
            }
            RtlCopyMemory(va, data, len);
            aic_frameq_pop(ctx->RxQ);
        }
        frag->Offset = 0;
        frag->ValidLength = len;
        pkt->FragmentIndex = fi;
        pkt->FragmentCount = 1;
        RtlZeroMemory(&pkt->Layout, sizeof(pkt->Layout));
        if (ctx->Mode == AIC_MODE_WIFICX) {
            pkt->Layout.Layer2Type = NetPacketLayer2TypeIeee80211;
            pkt->Layout.Layer2HeaderLength = AIC_80211_HDR;
        } else {
            pkt->Layout.Layer2Type = NetPacketLayer2TypeEthernet;
        }
        ctx->RxFrames++;
        pi = NetRingIncrementIndex(pr, pi);
        fi = NetRingIncrementIndex(fr, fi);
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    pr->BeginIndex = pi;
    fr->BeginIndex = fi;
    pr->NextIndex = pi;
    fr->NextIndex = fi;
}

static VOID
AicEvtRxNotify(_In_ NETPACKETQUEUE Queue, _In_ BOOLEAN Enabled)
{
    PAIC_DEVICE_CONTEXT ctx = AicQueueGetContext(Queue)->Device;
    KIRQL irql;
    UINT32 len;
    BOOLEAN now = FALSE;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->RxNotify = Enabled;
    if (Enabled && ctx->RxQ && !ctx->RxHold && aic_frameq_peek(ctx->RxQ, &len) != NULL) {
        ctx->RxNotify = FALSE;
        now = TRUE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (now) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
    }
}

static VOID
AicEvtRxCancel(_In_ NETPACKETQUEUE Queue)
{
    PAIC_QUEUE_CONTEXT q = AicQueueGetContext(Queue);
    NET_RING *pr = q->Packets;
    UINT32 i;

    for (i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i)) {
        NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
    }
    pr->BeginIndex = pr->NextIndex = pr->EndIndex;
    q->Fragments->BeginIndex = q->Fragments->NextIndex = q->Fragments->EndIndex;
}

static NTSTATUS
AicEvtCreateRxQueue(_In_ NETADAPTER Adapter, _Inout_ NETRXQUEUE_INIT *Init)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(*AicAdapterGetDevice(Adapter));
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NET_EXTENSION_QUERY query;
    const NET_RING_COLLECTION *rings;
    NETPACKETQUEUE queue;
    PAIC_QUEUE_CONTEXT q;
    NTSTATUS status;
    KIRQL irql;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, AicEvtRxAdvance, AicEvtRxNotify, AicEvtRxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AIC_QUEUE_CONTEXT);
    attr.EvtCleanupCallback = QueueCleanup;
    status = NetRxQueueCreate(Init, &attr, &cfg, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    q = AicQueueGetContext(queue);
    q->Device = ctx;
    rings = NetRxQueueGetRingCollection(queue);
    q->Packets = NetRingCollectionGetPacketRing(rings);
    q->Fragments = NetRingCollectionGetFragmentRing(rings);
    NET_EXTENSION_QUERY_INIT(&query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetRxQueueGetExtension(queue, &query, &q->VirtualAddress);
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->RxQueue = queue;
    if (ctx->RxQ) {
        ctx->RxQ->Head = ctx->RxQ->Tail = 0;    /* nothing stale from before */
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    return STATUS_SUCCESS;
}

static VOID
AicEvtSetReceiveFilter(_In_ NETADAPTER Adapter, _In_ NETRECEIVEFILTER Filter)
{
    /* The firmware filters by address on its own; everything it hands up
     * is for this station or broadcast/multicast. */
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Filter);
}

/* -------------------------------------------------------------- adapter */

NTSTATUS
AicNetCreateAdapter(_In_ PAIC_DEVICE_CONTEXT Ctx, _Inout_opt_ NETADAPTER_INIT *Init)
{
    NETADAPTER_INIT *init = Init;
    NET_ADAPTER_DATAPATH_CALLBACKS cb;
    WDF_OBJECT_ATTRIBUTES attr;
    NETADAPTER adapter;
    NTSTATUS status;
    ULONG i;

    if (init == NULL) {
        init = NetAdapterInitAllocate(Ctx->Device);
        if (init == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&cb, AicEvtCreateTxQueue, AicEvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(init, &cb);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AIC_ADAPTER_CONTEXT);
    status = NetAdapterCreate(init, &attr, &adapter);
    if (Init == NULL) {
        NetAdapterInitFree(init);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *AicAdapterGetDevice(adapter) = Ctx->Device;
    Ctx->Adapter = adapter;

    /* once per device: WiFiCx may ask for the adapter again after a stop */
    if (Ctx->RxQ != NULL) {
        return STATUS_SUCCESS;
    }
    Ctx->RxQ = (aic_frameq *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(aic_frameq), AIC_POOL_TAG);
    if (Ctx->RxQ == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < AIC_TX_SLOTS; i++) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Ctx->Device;
        status = WdfRequestCreate(&attr, NULL, &Ctx->TxReq[i]);
        if (NT_SUCCESS(status)) {
            status = WdfMemoryCreate(&attr, NonPagedPoolNx, AIC_POOL_TAG, AIC_TX_SLOT_SIZE, &Ctx->TxMem[i],
                                     (PVOID *)&Ctx->TxSlotBuf[i]);
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    C_ASSERT(AIC_TX_SLOTS == 32);   /* one bit per slot in TxFreeMask */
    Ctx->TxFreeMask = 0xFFFFFFFFu;
    return STATUS_SUCCESS;
}

VOID
AicNetFree(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    if (Ctx->RxQ) {
        ExFreePoolWithTag(Ctx->RxQ, AIC_POOL_TAG);
        Ctx->RxQ = NULL;
    }
}

/* In PrepareHardware, after the firmware init (or without it below stage
 * 2: then the MAC is a placeholder and the link never comes up). */
NTSTATUS
AicNetStart(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    NET_ADAPTER_LINK_LAYER_ADDRESS addr;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES link;
    NET_ADAPTER_TX_CAPABILITIES tx;
    NET_ADAPTER_RX_CAPABILITIES rx;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES filter;
    NET_ADAPTER_LINK_STATE state;
    UCHAR mac[6];

    RtlCopyMemory(mac, Ctx->Aic.Mac, 6);
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
        mac[0] = 0x02; mac[1] = 0xA1; mac[2] = 0xC8; mac[3] = 0x00; mac[4] = 0x00; mac[5] = 0x01;
    }
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&addr, 6, mac);
    NetAdapterSetPermanentLinkLayerAddress(Ctx->Adapter, &addr);
    NetAdapterSetCurrentLinkLayerAddress(Ctx->Adapter, &addr);

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&link, AIC_LINK_SPEED, AIC_LINK_SPEED);
    NetAdapterSetLinkLayerCapabilities(Ctx->Adapter, &link);
    NetAdapterSetLinkLayerMtuSize(Ctx->Adapter, 1500);

    NET_ADAPTER_TX_CAPABILITIES_INIT(&tx, 1);
    /* an 802.11 frame is AIC_80211_OVERHEAD longer than its Ethernet frame */
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rx, Ctx->Mode == AIC_MODE_WIFICX ? 2048 : 1536, 1);
    NetAdapterSetDataPathCapabilities(Ctx->Adapter, &tx, &rx);

    /* As dwc_eqos notes, tcpip does not reliably bind unless everything is
     * claimed. */
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&filter, AicEvtSetReceiveFilter);
    filter.MaximumMulticastAddresses = 0;
    filter.SupportedPacketFilters = NetPacketFilterFlagDirected | NetPacketFilterFlagAllMulticast |
                                    NetPacketFilterFlagBroadcast | NetPacketFilterFlagPromiscuous;
    NetAdapterSetReceiveFilterCapabilities(Ctx->Adapter, &filter);

    Ctx->LinkReported = FALSE;
    if (Ctx->Mode != AIC_MODE_WIFICX) {
        /* WiFiCx reports the link from the association indications */
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&state);
        NetAdapterSetLinkState(Ctx->Adapter, &state);
    }
    return NetAdapterStart(Ctx->Adapter);
}

/* From the worker: report the Wi-Fi link to Windows when it changes. */
VOID
AicNetUpdateLink(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    NET_ADAPTER_LINK_STATE state;
    BOOLEAN up = DataPathOpen(Ctx);

    if (Ctx->Adapter == NULL || up == Ctx->LinkReported || Ctx->Mode == AIC_MODE_WIFICX) {
        return;
    }
    if (up) {
        NET_ADAPTER_LINK_STATE_INIT(&state, AIC_LINK_SPEED, MediaConnectStateConnected, MediaDuplexStateFull,
                                    NetAdapterPauseFunctionTypeUnsupported, NetAdapterAutoNegotiationFlagNone);
    } else {
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&state);
    }
    NetAdapterSetLinkState(Ctx->Adapter, &state);
    Ctx->LinkReported = up;
}
