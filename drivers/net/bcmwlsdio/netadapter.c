/*
 * netadapter.c - the station's network adapter (stage 3): a NetAdapterCx
 * adapter that WiFiCx creates (EvtWifiDeviceCreateAdapter), carrying
 * native 802.11 data frames, as aic8800usb's netadapter.c does in its
 * WiFiCx mode.
 *
 * TX: EvtPacketQueueAdvance gathers each packet's fragments into one
 * 802.11 data frame, turns it into Ethernet (Bwl80211ToEth) and puts it in
 * the TX FIFO; the packet is done once copied. The SDIO card is only
 * touched by the worker thread (wificx.cpp), which empties the FIFO
 * (BwlNetTxPump) as far as the firmware's credit window allows. When the
 * FIFO is full the queue stops taking packets, and the worker asks
 * NetAdapterCx for more once there is room again.
 *
 * RX: the worker's poller hands every Ethernet frame the firmware delivers
 * to BwlNetOps.rx_eth, which puts it in the RX FIFO; EvtPacketQueueAdvance
 * turns queued frames into 802.11 frames (BwlEthTo80211) in the RX ring's
 * buffers. Frames that arrive before the association has been indicated
 * to Windows (message 1 of the handshake comes right behind it) wait in
 * the FIFO (RxHold).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"

/* the most the BCM43456 does (1x1, VHT80); the actual rate goes up with the link state */
#define BWL_LINK_SPEED (433ull * 1000 * 1000)

typedef struct _BWL_QUEUE_CONTEXT {
    PBWL_DEVICE_CONTEXT Device;
    NET_RING *Packets;
    NET_RING *Fragments;
    NET_EXTENSION VirtualAddress;
} BWL_QUEUE_CONTEXT, *PBWL_QUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BWL_QUEUE_CONTEXT, BwlQueueGetContext)

static EVT_NET_ADAPTER_CREATE_TXQUEUE BwlEvtCreateTxQueue;
static EVT_NET_ADAPTER_CREATE_RXQUEUE BwlEvtCreateRxQueue;
static EVT_NET_ADAPTER_SET_RECEIVE_FILTER BwlEvtSetReceiveFilter;
static EVT_PACKET_QUEUE_ADVANCE BwlEvtTxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED BwlEvtTxNotify;
static EVT_PACKET_QUEUE_CANCEL BwlEvtTxCancel;
static EVT_PACKET_QUEUE_ADVANCE BwlEvtRxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED BwlEvtRxNotify;
static EVT_PACKET_QUEUE_CANCEL BwlEvtRxCancel;

static VOID BwlNetRxEth(_In_ void *C, _In_reads_(Len) const u8 *Eth, _In_ u32 Len);

const bwl_net_ops BwlNetOps = {
    BwlNetRxEth,
    BwlWifiScanBss,
};

/* Windows may send from association on (EAPOL first). */
static BOOLEAN
TxOpen(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    return Ctx->FwReady && Ctx->Bwl.Wlan.Link >= BWL_LINK_ASSOCIATED && Ctx->WorkerStop == 0;
}

/* ------------------------------------------------------------------ TX */

/* The 802.1D priority of a QoS data frame (its TID), else 0. */
static unsigned
FramePrio(_In_reads_(Len) const UCHAR *F, _In_ ULONG Len)
{
    return (Len >= 26 && (F[0] & 0x8C) == 0x88) ? (unsigned)(F[24] & 7) : 0u;
}

static VOID
BwlEvtTxAdvance(_In_ NETPACKETQUEUE Queue)
{
    PBWL_QUEUE_CONTEXT q = BwlQueueGetContext(Queue);
    PBWL_DEVICE_CONTEXT ctx = q->Device;
    NET_RING *pr = q->Packets, *fr = q->Fragments;
    UINT32 i = pr->BeginIndex;
    BOOLEAN open = TxOpen(ctx), queued = FALSE;
    KIRQL irql;

    while (i != pr->EndIndex) {
        NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, i);

        if (!pkt->Ignore && open) {
            ULONG len = 0, k;
            UINT32 f = pkt->FragmentIndex;
            BOOLEAN fits = TRUE;
            unsigned prio;
            int n;

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
            /* a native 802.11 data frame from Windows, to Ethernet for the firmware */
            n = fits ? Bwl80211ToEth(ctx->TxGather, len, ctx->TxEth, (u32)sizeof(ctx->TxEth)) : 0;
            prio = FramePrio(ctx->TxGather, len);
            if (n >= 14 && (u32)n <= BWL_FRAMEQ_MAX) {
                BOOLEAN ok;

                KeAcquireSpinLock(&ctx->DataLock, &irql);
                ok = ctx->TxQ != NULL && BwlFrameqCount(ctx->TxQ) < BWL_FRAMEQ_SLOTS - 1 &&
                     BwlFrameqPush(ctx->TxQ, ctx->TxEth, (u32)n, prio);
                if (!ok && ctx->TxQ != NULL) {
                    /* full: this packet stays, the worker asks for more */
                    ctx->TxNotify = TRUE;
                    KeReleaseSpinLock(&ctx->DataLock, irql);
                    break;
                }
                KeReleaseSpinLock(&ctx->DataLock, irql);
                if (ok) {
                    queued = TRUE;
                } else {
                    ctx->TxDrops++;
                }
            } else {
                ctx->TxDrops++;     /* not a frame to pass (protected, management, too long) */
            }
        } else if (!pkt->Ignore) {
            ctx->TxDrops++;         /* not associated: dropped */
        }
        fr->BeginIndex = NetRingAdvanceIndex(fr, pkt->FragmentIndex, pkt->FragmentCount);
        i = NetRingIncrementIndex(pr, i);
    }
    pr->BeginIndex = i;
    pr->NextIndex = i;
    fr->NextIndex = fr->BeginIndex;
    if (queued) {
        KeSetEvent(&ctx->WakeEvent, IO_NO_INCREMENT, FALSE);
    }
}

static VOID
BwlEvtTxNotify(_In_ NETPACKETQUEUE Queue, _In_ BOOLEAN Enabled)
{
    PBWL_DEVICE_CONTEXT ctx = BwlQueueGetContext(Queue)->Device;
    KIRQL irql;
    BOOLEAN now = FALSE;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->TxNotify = Enabled;
    if (Enabled && ctx->TxQ != NULL && BwlFrameqCount(ctx->TxQ) < BWL_FRAMEQ_SLOTS / 2) {
        ctx->TxNotify = FALSE;
        now = TRUE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (now) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(Queue);
    }
}

static VOID
BwlEvtTxCancel(_In_ NETPACKETQUEUE Queue)
{
    PBWL_QUEUE_CONTEXT q = BwlQueueGetContext(Queue);

    q->Packets->BeginIndex = q->Packets->NextIndex = q->Packets->EndIndex;
    q->Fragments->BeginIndex = q->Fragments->NextIndex = q->Fragments->EndIndex;
}

/* Worker thread: what the FIFO holds, to the firmware, while it takes it. */
BOOLEAN
BwlNetTxPump(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    NETPACKETQUEUE notify = NULL;
    BOOLEAN sent = FALSE;
    KIRQL irql;

    for (;;) {
        const u8 *frame;
        u32 len = 0;
        unsigned prio = 0;
        int r;

        KeAcquireSpinLock(&Ctx->DataLock, &irql);
        frame = Ctx->TxQ != NULL ? BwlFrameqPeek(Ctx->TxQ, &len, &prio) : NULL;
        KeReleaseSpinLock(&Ctx->DataLock, irql);
        if (frame == NULL || !BwlTxReady(&Ctx->Bwl, prio)) {
            break;
        }
        /* only this thread pops, and a push never touches the oldest
         * slot: the frame stays put while it is sent */
        r = BwlTxEth(&Ctx->Bwl, frame, len, prio);
        if (r == BWL_E_CREDIT) {
            break;
        }
        if (r == BWL_OK) {
            Ctx->TxFrames++;
            sent = TRUE;
        } else {
            Ctx->TxErrors++;
        }
        KeAcquireSpinLock(&Ctx->DataLock, &irql);
        BwlFrameqPop(Ctx->TxQ);
        KeReleaseSpinLock(&Ctx->DataLock, irql);
    }
    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (Ctx->TxNotify && Ctx->TxQueue != NULL && Ctx->TxQ != NULL &&
        BwlFrameqCount(Ctx->TxQ) < BWL_FRAMEQ_SLOTS / 2) {
        Ctx->TxNotify = FALSE;
        notify = Ctx->TxQueue;
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (notify) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(notify);
    }
    return sent;
}

VOID
BwlNetTxDrain(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ ULONG MaxMs)
{
    ULONG waited = 0;
    KIRQL irql;
    u32 n;

    for (;;) {
        (VOID)BwlNetTxPump(Ctx);
        KeAcquireSpinLock(&Ctx->DataLock, &irql);
        n = Ctx->TxQ != NULL ? BwlFrameqCount(Ctx->TxQ) : 0;
        KeReleaseSpinLock(&Ctx->DataLock, irql);
        if (n == 0 || waited >= MaxMs || Ctx->WorkerStop) {
            break;
        }
        /* the credit comes back with the next frame from the firmware */
        (VOID)BwlService(&Ctx->Bwl);
        BwlDelayMs(1);
        waited++;
    }
}

/* Both FIFOs emptied (leaving a network). */
VOID
BwlNetFlush(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    KIRQL irql;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (Ctx->TxQ) {
        Ctx->TxQ->Head = Ctx->TxQ->Tail = 0;
    }
    if (Ctx->RxQ) {
        Ctx->RxQ->Head = Ctx->RxQ->Tail = 0;
    }
    Ctx->RxHold = TRUE;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
}

static VOID
QueueCleanup(_In_ WDFOBJECT Object)
{
    PBWL_QUEUE_CONTEXT q = BwlQueueGetContext((NETPACKETQUEUE)Object);
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
BwlEvtCreateTxQueue(_In_ NETADAPTER Adapter, _Inout_ NETTXQUEUE_INIT *Init)
{
    PBWL_DEVICE_CONTEXT ctx = BwlAdapterGetContext(Adapter)->Device;
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NET_EXTENSION_QUERY query;
    const NET_RING_COLLECTION *rings;
    NETPACKETQUEUE queue;
    PBWL_QUEUE_CONTEXT q;
    NTSTATUS status;
    KIRQL irql;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, BwlEvtTxAdvance, BwlEvtTxNotify, BwlEvtTxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BWL_QUEUE_CONTEXT);
    attr.EvtCleanupCallback = QueueCleanup;
    status = NetTxQueueCreate(Init, &attr, &cfg, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    q = BwlQueueGetContext(queue);
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

/* BwlNetOps.rx_eth: the poller, on the worker thread. From the join on
 * (message 1 can come in the same batch as the join's result). */
static VOID
BwlNetRxEth(_In_ void *C, _In_reads_(Len) const u8 *Eth, _In_ u32 Len)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)C;
    NETPACKETQUEUE notify = NULL;
    KIRQL irql;

    if (ctx->Bwl.Wlan.Link == BWL_LINK_DOWN || ctx->RxQ == NULL) {
        ctx->RxDrops++;
        return;
    }
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    if (ctx->RxQueue == NULL || !BwlFrameqPush(ctx->RxQ, Eth, Len, 0)) {
        ctx->RxDrops++;
    } else if (ctx->RxNotify && !ctx->RxHold) {
        notify = ctx->RxQueue;
        ctx->RxNotify = FALSE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (notify) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(notify);
    }
}

/* The association is indicated: let the frames that waited go up. */
VOID
BwlNetRxRelease(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    NETPACKETQUEUE notify = NULL;
    KIRQL irql;
    u32 len;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    Ctx->RxHold = FALSE;
    if (Ctx->RxNotify && Ctx->RxQueue && Ctx->RxQ && BwlFrameqPeek(Ctx->RxQ, &len, NULL) != NULL) {
        notify = Ctx->RxQueue;
        Ctx->RxNotify = FALSE;
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (notify) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(notify);
    }
}

static VOID
BwlEvtRxAdvance(_In_ NETPACKETQUEUE Queue)
{
    PBWL_QUEUE_CONTEXT q = BwlQueueGetContext(Queue);
    PBWL_DEVICE_CONTEXT ctx = q->Device;
    NET_RING *pr = q->Packets, *fr = q->Fragments;
    UINT32 pi = pr->BeginIndex, fi = fr->BeginIndex;
    KIRQL irql;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    while (pi != pr->EndIndex && fi != fr->EndIndex && !ctx->RxHold) {
        NET_PACKET *pkt;
        NET_FRAGMENT *frag;
        const u8 *data;
        PUCHAR va;
        u32 len = 0;
        int n;

        data = ctx->RxQ ? BwlFrameqPeek(ctx->RxQ, &len, NULL) : NULL;
        if (data == NULL) {
            break;
        }
        pkt = NetRingGetPacketAtIndex(pr, pi);
        frag = NetRingGetFragmentAtIndex(fr, fi);
        va = (PUCHAR)NetExtensionGetFragmentVirtualAddress(&q->VirtualAddress, fi)->VirtualAddress;
        /* to the native 802.11 data frame Windows wants */
        n = BwlEthTo80211(ctx->Bwl.Wlan.Bssid, data, len, va, (u32)frag->Capacity);
        BwlFrameqPop(ctx->RxQ);
        if (n <= 0) {
            ctx->RxDrops++;
            continue;
        }
        frag->Offset = 0;
        frag->ValidLength = (UINT32)n;
        pkt->FragmentIndex = fi;
        pkt->FragmentCount = 1;
        RtlZeroMemory(&pkt->Layout, sizeof(pkt->Layout));
        pkt->Layout.Layer2Type = NetPacketLayer2TypeIeee80211;
        pkt->Layout.Layer2HeaderLength = BWL_80211_HDR;
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
BwlEvtRxNotify(_In_ NETPACKETQUEUE Queue, _In_ BOOLEAN Enabled)
{
    PBWL_DEVICE_CONTEXT ctx = BwlQueueGetContext(Queue)->Device;
    KIRQL irql;
    u32 len;
    BOOLEAN now = FALSE;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->RxNotify = Enabled;
    if (Enabled && ctx->RxQ && !ctx->RxHold && BwlFrameqPeek(ctx->RxQ, &len, NULL) != NULL) {
        ctx->RxNotify = FALSE;
        now = TRUE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (now) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
    }
}

static VOID
BwlEvtRxCancel(_In_ NETPACKETQUEUE Queue)
{
    PBWL_QUEUE_CONTEXT q = BwlQueueGetContext(Queue);
    NET_RING *pr = q->Packets;
    UINT32 i;

    for (i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i)) {
        NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
    }
    pr->BeginIndex = pr->NextIndex = pr->EndIndex;
    q->Fragments->BeginIndex = q->Fragments->NextIndex = q->Fragments->EndIndex;
}

static NTSTATUS
BwlEvtCreateRxQueue(_In_ NETADAPTER Adapter, _Inout_ NETRXQUEUE_INIT *Init)
{
    PBWL_DEVICE_CONTEXT ctx = BwlAdapterGetContext(Adapter)->Device;
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NET_EXTENSION_QUERY query;
    const NET_RING_COLLECTION *rings;
    NETPACKETQUEUE queue;
    PBWL_QUEUE_CONTEXT q;
    NTSTATUS status;
    KIRQL irql;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, BwlEvtRxAdvance, BwlEvtRxNotify, BwlEvtRxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BWL_QUEUE_CONTEXT);
    attr.EvtCleanupCallback = QueueCleanup;
    status = NetRxQueueCreate(Init, &attr, &cfg, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    q = BwlQueueGetContext(queue);
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
BwlEvtSetReceiveFilter(_In_ NETADAPTER Adapter, _In_ NETRECEIVEFILTER Filter)
{
    /* The firmware filters by address on its own; everything it hands up
     * is for this station or a group. */
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Filter);
}

/* -------------------------------------------------------------- adapter */

NTSTATUS
BwlNetCreateAdapter(_In_ PBWL_DEVICE_CONTEXT Ctx, _Inout_ NETADAPTER_INIT *Init)
{
    NET_ADAPTER_DATAPATH_CALLBACKS cb;
    WDF_OBJECT_ATTRIBUTES attr;
    NETADAPTER adapter;
    NTSTATUS status;

    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&cb, BwlEvtCreateTxQueue, BwlEvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(Init, &cb);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, BWL_ADAPTER_CONTEXT);
    status = NetAdapterCreate(Init, &attr, &adapter);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    BwlAdapterGetContext(adapter)->Device = Ctx;
    Ctx->Adapter = adapter;

    /* once per device: WiFiCx may ask for the adapter again after a stop */
    if (Ctx->RxQ == NULL) {
        Ctx->RxQ = (bwl_frameq *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(bwl_frameq), BWL_POOL_TAG);
    }
    if (Ctx->TxQ == NULL) {
        Ctx->TxQ = (bwl_frameq *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(bwl_frameq), BWL_POOL_TAG);
    }
    return Ctx->RxQ != NULL && Ctx->TxQ != NULL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

VOID
BwlNetFree(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    if (Ctx->RxQ) {
        ExFreePoolWithTag(Ctx->RxQ, BWL_POOL_TAG);
        Ctx->RxQ = NULL;
    }
    if (Ctx->TxQ) {
        ExFreePoolWithTag(Ctx->TxQ, BWL_POOL_TAG);
        Ctx->TxQ = NULL;
    }
}

/* The chip's address and the queues; WiFiCx reports the link itself. */
NTSTATUS
BwlNetStart(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    NET_ADAPTER_LINK_LAYER_ADDRESS addr;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES link;
    NET_ADAPTER_TX_CAPABILITIES tx;
    NET_ADAPTER_RX_CAPABILITIES rx;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES filter;
    UCHAR mac[6];

    RtlCopyMemory(mac, Ctx->Bwl.Wlan.Mac, 6);
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
        mac[0] = 0x02; mac[1] = 0x43; mac[2] = 0x45; mac[3] = 0x00; mac[4] = 0x00; mac[5] = 0x01;
    }
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&addr, 6, mac);
    NetAdapterSetPermanentLinkLayerAddress(Ctx->Adapter, &addr);
    NetAdapterSetCurrentLinkLayerAddress(Ctx->Adapter, &addr);

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&link, BWL_LINK_SPEED, BWL_LINK_SPEED);
    NetAdapterSetLinkLayerCapabilities(Ctx->Adapter, &link);
    NetAdapterSetLinkLayerMtuSize(Ctx->Adapter, 1500);

    NET_ADAPTER_TX_CAPABILITIES_INIT(&tx, 1);
    /* an 802.11 frame is 18 bytes longer than its Ethernet frame */
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rx, 2048, 1);
    NetAdapterSetDataPathCapabilities(Ctx->Adapter, &tx, &rx);

    /* as dwc_eqos notes, tcpip does not reliably bind unless everything is claimed */
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&filter, BwlEvtSetReceiveFilter);
    filter.MaximumMulticastAddresses = 0;
    filter.SupportedPacketFilters = NetPacketFilterFlagDirected | NetPacketFilterFlagAllMulticast |
                                    NetPacketFilterFlagBroadcast | NetPacketFilterFlagPromiscuous;
    NetAdapterSetReceiveFilterCapabilities(Ctx->Adapter, &filter);
    return NetAdapterStart(Ctx->Adapter);
}
