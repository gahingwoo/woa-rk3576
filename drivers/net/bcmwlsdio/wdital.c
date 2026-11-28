/*
 * wdital.c - the data path of the WDI miniport (bcmwlwdi.sys, Windows 10):
 * WDI's TAL handlers (dot11wdi.h) over the same TX and RX FIFOs as the
 * WiFiCx adapter's netadapter.c. The firmware is a full MAC and talks
 * Ethernet; Windows talks native 802.11 data frames; bwl_data.c converts.
 *
 * TX: WDI queues per port (TxTargetPriorityQueueing) and calls TxDataSend
 * from its TX thread; the frames it gives (NdisWdiTxDequeueIndication, as
 * many as the TX FIFO has room for) are copied into the FIFO as Ethernet
 * frames and handed back at once (transfer complete, then send complete
 * for those WDI asked it of). A full FIFO pauses WDI (credit), and the
 * worker restarts it once half of it has gone to the firmware. Only the
 * worker thread talks to the card (BwlNetTxPump).
 *
 * RX: the worker's poller hands each Ethernet frame to rx_eth, which turns
 * it into an 802.11 frame in one of BWL_TAL_RX_BUFS preallocated
 * NET_BUFFER_LISTs with WDI frame metadata (MiniportReserved[0]), queues
 * it, and tells WDI (NdisWdiRxInorderDataIndication at PASSIVE_LEVEL),
 * which takes the queue with RxGetMpdus and returns the frames with
 * RxReturnFrames. All frames go to the port's group peer, as Microsoft's
 * SDIO WDI sample does for a station. Frames that arrive before the
 * association has been indicated wait (RxHold).
 *
 * Every access to frame memory goes through RtlCopyMemory or byte loads:
 * pool memory, no DMA from this driver (sdbus owns that).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcmwlsdio.h"
#include <dot11wdi.h>

#define BWL_TAL_TAG      'tWcB'
#define BWL_TAL_RX_BUFS  64
#define BWL_TAL_RX_BUF   2048
#define BWL_TAL_TX_BATCH 16             /* frames per TxDataSend at most */
#define BWL_TAL_PEER     0              /* the group peer every frame goes to */

typedef struct _BWL_RXB {
    PNET_BUFFER_LIST Nbl;
    PMDL Mdl;
    PUCHAR Buf;
    struct _BWL_RXB *Next;
} BWL_RXB;

typedef struct _BWL_TAL {
    PBWL_DEVICE_CONTEXT Ctx;
    NDIS_HANDLE Adapter;                /* MiniportAdapterHandle, for the pool and MDLs */
    NDIS_HANDLE DataPath;               /* NdisMiniportDataPathHandle */
    NDIS_WDI_DATA_API Api;
    BOOLEAN HaveApi;
    NDIS_HANDLE NblPool;

    /* under the device's DataLock */
    BWL_RXB Rx[BWL_TAL_RX_BUFS];
    BWL_RXB *Free;
    PNET_BUFFER_LIST PendHead, PendTail;
    BOOLEAN Started, RxStopped, RxPaused, Indicating, StopPending, TxPaused, GroupPeer;
    WDI_PORT_ID Port;

    /* TxDataSend's scratch (WDI sends from one thread; the lock is for safety) */
    KSPIN_LOCK TxLock;
    UCHAR TxGather[1700];
    UCHAR TxEth[1700];

    ULONG TxSends, TxPauses, TxRestarts, RxIndications, RxNoBuffer, RxNoMeta;
} BWL_TAL, *PBWL_TAL;

static VOID BwlNetRxEth(_In_ void *C, _In_reads_(Len) const u8 *Eth, _In_ u32 Len);

const bwl_net_ops BwlNetOps = {
    BwlNetRxEth,
    BwlWifiScanBss,
};

static MINIPORT_WDI_TX_ABORT TalTxAbort;
static MINIPORT_WDI_TX_TARGET_DESC_INIT TalTxDescInit;
static MINIPORT_WDI_TX_TARGET_DESC_DEINIT TalTxDescDeInit;
static MINIPORT_WDI_TX_DATA_SEND TalTxDataSend;
static MINIPORT_WDI_TX_TAL_SEND TalTxTalSend;
static MINIPORT_WDI_TX_TAL_SEND_COMPLETE TalTxTalSendComplete;
static MINIPORT_WDI_TX_TAL_QUEUE_IN_ORDER TalTxQueueInOrder;
static MINIPORT_WDI_TX_PEER_BACKLOG TalTxPeerBacklog;
static MINIPORT_WDI_RX_STOP TalRxStop;
static MINIPORT_WDI_RX_FLUSH TalRxFlush;
static MINIPORT_WDI_RX_RESTART TalRxRestart;
static MINIPORT_WDI_RX_GET_MPDUS TalRxGetMpdus;
static MINIPORT_WDI_RX_RETURN_FRAMES TalRxReturnFrames;
static MINIPORT_WDI_RX_RESUME TalRxResume;
static MINIPORT_WDI_RX_THROTTLE TalRxThrottle;
static MINIPORT_WDI_RX_PPDU_RSSI TalRxPpduRssi;
static MINIPORT_WDI_TAL_TXRX_START TalStart;
static MINIPORT_WDI_TAL_TXRX_STOP TalStop;
static MINIPORT_WDI_TAL_TXRX_ADD_PORT TalAddPort;
static MINIPORT_WDI_TAL_TXRX_DELETE_PORT TalDeletePort;
static MINIPORT_WDI_TAL_TXRX_SET_PORT_OPMODE TalSetPortOpMode;
static MINIPORT_WDI_TAL_TXRX_RESET_PORT TalResetPort;
static MINIPORT_WDI_TAL_TXRX_PEER_CONFIG TalPeerConfig;
static MINIPORT_WDI_TAL_TXRX_PEER_DELETE_CONFIRM TalPeerDeleteConfirm;

/* Records from TAL calls, which are not all at PASSIVE_LEVEL. */
static VOID
Put(_In_ PBWL_TAL T, _In_ PCWSTR Name, _In_ ULONG Value)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        BwlPutULong(T->Ctx, Name, Value);
    }
}

/* Windows may send from association on (EAPOL first). */
static BOOLEAN
TxOpen(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    return Ctx->FwReady && Ctx->Bwl.Wlan.Link >= BWL_LINK_ASSOCIATED && Ctx->WorkerStop == 0;
}

/* --------------------------------------------------------- allocation */

/* AllocateAdapter: the FIFOs and the TAL state (the RX pool comes with the TAL). */
NTSTATUS
BwlTalAlloc(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ NDIS_HANDLE Adapter)
{
    PBWL_TAL t;

    Ctx->RxQ = (bwl_frameq *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(bwl_frameq), BWL_POOL_TAG);
    Ctx->TxQ = (bwl_frameq *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(bwl_frameq), BWL_POOL_TAG);
    t = (PBWL_TAL)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BWL_TAL), BWL_TAL_TAG);
    if (t != NULL) {
        t->Ctx = Ctx;
        t->Adapter = Adapter;
        KeInitializeSpinLock(&t->TxLock);
        Ctx->Tal = t;
    }
    return Ctx->RxQ != NULL && Ctx->TxQ != NULL && t != NULL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static VOID
FreeRxPool(_In_ PBWL_TAL T)
{
    ULONG i;

    for (i = 0; i < BWL_TAL_RX_BUFS; i++) {
        BWL_RXB *b = &T->Rx[i];

        if (b->Nbl != NULL) {
            NdisFreeNetBufferList(b->Nbl);
            b->Nbl = NULL;
        }
        if (b->Mdl != NULL) {
            NdisFreeMdl(b->Mdl);
            b->Mdl = NULL;
        }
        if (b->Buf != NULL) {
            ExFreePoolWithTag(b->Buf, BWL_TAL_TAG);
            b->Buf = NULL;
        }
    }
    T->Free = NULL;
    if (T->NblPool != NULL) {
        NdisFreeNetBufferListPool(T->NblPool);
        T->NblPool = NULL;
    }
}

static NTSTATUS
AllocRxPool(_In_ PBWL_TAL T)
{
    NET_BUFFER_LIST_POOL_PARAMETERS p;
    ULONG i;

    RtlZeroMemory(&p, sizeof(p));
    p.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    p.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    p.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    p.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    p.fAllocateNetBuffer = TRUE;
    p.PoolTag = BWL_TAL_TAG;
    T->NblPool = NdisAllocateNetBufferListPool(T->Adapter, &p);
    if (T->NblPool == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    T->Free = NULL;
    for (i = 0; i < BWL_TAL_RX_BUFS; i++) {
        BWL_RXB *b = &T->Rx[i];

        b->Buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, BWL_TAL_RX_BUF, BWL_TAL_TAG);
        b->Mdl = b->Buf != NULL ? NdisAllocateMdl(T->Adapter, b->Buf, BWL_TAL_RX_BUF) : NULL;
        b->Nbl = b->Mdl != NULL ? NdisAllocateNetBufferAndNetBufferList(T->NblPool, 0, 0, b->Mdl, 0, 0) : NULL;
        if (b->Nbl == NULL) {
            FreeRxPool(T);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        b->Next = T->Free;
        T->Free = b;
    }
    return STATUS_SUCCESS;
}

/* FreeAdapter: the TAL is deinitialized by now. */
VOID
BwlNetFree(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Tal != NULL) {
        FreeRxPool(Ctx->Tal);
        ExFreePoolWithTag(Ctx->Tal, BWL_TAL_TAG);
        Ctx->Tal = NULL;
    }
    if (Ctx->RxQ) {
        ExFreePoolWithTag(Ctx->RxQ, BWL_POOL_TAG);
        Ctx->RxQ = NULL;
    }
    if (Ctx->TxQ) {
        ExFreePoolWithTag(Ctx->TxQ, BWL_POOL_TAG);
        Ctx->TxQ = NULL;
    }
}

/* ------------------------------------------------------------------ TX */

static VOID
TalTxDescInit(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL, _Out_ NDIS_STATUS *pWifiStatus)
{
    PWDI_FRAME_METADATA m = (PWDI_FRAME_METADATA)pNBL->MiniportReserved[0];

    UNREFERENCED_PARAMETER(Context);
    if (m != NULL) {
        m->u.txMetaData.TxCost = 1;     /* one FIFO slot */
    }
    *pWifiStatus = NDIS_STATUS_SUCCESS;
}

static VOID
TalTxDescDeInit(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(pNBL);
}

/* The 802.1D priority of a QoS data frame (its TID), else 0. */
static unsigned
FramePrio(_In_reads_(Len) const UCHAR *F, _In_ ULONG Len)
{
    return (Len >= 26 && (F[0] & 0x8C) == 0x88) ? (unsigned)(F[24] & 7) : 0u;
}

static ULONG
TxRoom(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    ULONG n = 0;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (Ctx->TxQ != NULL) {
        n = (BWL_FRAMEQ_SLOTS - 1) - BwlFrameqCount(Ctx->TxQ);
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    return n;
}

/* One MSDU of a frame WDI handed over: to Ethernet, into the TX FIFO. */
static VOID
TxOne(_In_ PBWL_TAL T, _In_ PNET_BUFFER Nb)
{
    PBWL_DEVICE_CONTEXT ctx = T->Ctx;
    ULONG len = NET_BUFFER_DATA_LENGTH(Nb);
    const UCHAR *f;
    KIRQL irql;
    BOOLEAN ok;
    int n;

    if (len == 0 || len > sizeof(T->TxGather)) {
        ctx->TxDrops++;
        return;
    }
    /* contiguous already, or gathered into TxGather */
    f = (const UCHAR *)NdisGetDataBuffer(Nb, len, T->TxGather, 1, 0);
    if (f == NULL) {
        ctx->TxDrops++;
        return;
    }
    n = Bwl80211ToEth(f, len, T->TxEth, (u32)sizeof(T->TxEth));
    if (n < 14 || (u32)n > BWL_FRAMEQ_MAX) {
        ctx->TxDrops++;                 /* not a frame to pass (protected, management, too long) */
        return;
    }
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ok = ctx->TxQ != NULL && BwlFrameqCount(ctx->TxQ) < BWL_FRAMEQ_SLOTS - 1 &&
         BwlFrameqPush(ctx->TxQ, T->TxEth, (u32)n, FramePrio(f, len));
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (!ok) {
        ctx->TxDrops++;
    }
}

static VOID
TalTxDataSend(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
              _In_ WDI_EXTENDED_TID ExTid, _In_ UINT16 NumQueueFrames, _In_ UINT32 NumActiveFrames,
              _In_ BOOLEAN bRobustnessFlag)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    PBWL_DEVICE_CONTEXT ctx = T->Ctx;
    PNET_BUFFER_LIST chain = NULL, nbl;
    WDI_FRAME_ID idList[BWL_TAL_TX_BATCH * 4];
    ULONG room = TxRoom(ctx), credit, ids = 0;
    BOOLEAN open = TxOpen(ctx);
    KIRQL irql;

    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTid);
    UNREFERENCED_PARAMETER(NumQueueFrames);
    UNREFERENCED_PARAMETER(NumActiveFrames);
    UNREFERENCED_PARAMETER(bRobustnessFlag);

    credit = room < BWL_TAL_TX_BATCH ? room : BWL_TAL_TX_BATCH;
    if (credit != 0 || !open) {
        /* not associated: take them anyway and fail them (dropped) */
        T->Api.TxDequeueIndication(T->DataPath, WDI_QUANTUM_INVALID, (UINT8)(open ? credit : BWL_TAL_TX_BATCH),
                                   (UINT16)(open ? credit : BWL_TAL_TX_BATCH), &chain);
    }
    if (chain == NULL && !open) {
        return;
    }
    if (chain == NULL) {
        /* the FIFO is full: no more until the worker made room */
        KeAcquireSpinLock(&ctx->DataLock, &irql);
        T->TxPaused = TRUE;
        KeReleaseSpinLock(&ctx->DataLock, irql);
        T->TxPauses++;
        T->Api.TxSendPauseIndication(T->DataPath, WDI_PORT_ANY, WDI_PEER_ANY, WDI_EXT_TID_UNKNOWN,
                                     WDI_TX_PAUSE_REASON_CREDIT);
        return;
    }
    if (!open) {
        for (nbl = chain; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
            ctx->TxDrops++;
        }
        T->Api.TxTransferCompleteIndication(T->DataPath, WDI_TxFrameStatus_TransferFailed, chain);
        return;
    }
    KeAcquireSpinLock(&T->TxLock, &irql);
    for (nbl = chain; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
        PWDI_FRAME_METADATA m = (PWDI_FRAME_METADATA)nbl->MiniportReserved[0];
        PNET_BUFFER nb;

        for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb != NULL; nb = NET_BUFFER_NEXT_NB(nb)) {
            TxOne(T, nb);
        }
        NET_BUFFER_LIST_STATUS(nbl) = NDIS_STATUS_SUCCESS;
        /* the IDs first: WDI may reuse a frame once its transfer is complete */
        if (m != NULL && m->u.txMetaData.bTxCompleteRequired && ids < RTL_NUMBER_OF(idList)) {
            idList[ids++] = m->FrameID;
        }
        T->TxSends++;
    }
    KeReleaseSpinLock(&T->TxLock, irql);
    /* not under a lock of ours: WDI may call back in */
    T->Api.TxTransferCompleteIndication(T->DataPath, WDI_TxFrameStatus_Ok, chain);
    if (ids != 0) {
        T->Api.TxSendCompleteIndication(T->DataPath, WDI_TxFrameStatus_Ok, (UINT16)ids, idList, NULL);
    }
    KeSetEvent(&ctx->WakeEvent, IO_NO_INCREMENT, FALSE);
}

/* Frames this driver injected (none): taken and done. */
static VOID
TalTxTalSend(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
             _In_ WDI_EXTENDED_TID ExTid, _In_ UINT16 NumQueueFrames, _In_ UINT32 NumActiveFrames,
             _In_ BOOLEAN bRobustnessFlag)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    PNET_BUFFER_LIST chain = NULL;

    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTid);
    UNREFERENCED_PARAMETER(NumQueueFrames);
    UNREFERENCED_PARAMETER(NumActiveFrames);
    UNREFERENCED_PARAMETER(bRobustnessFlag);
    T->Api.TxDequeueIndication(T->DataPath, WDI_QUANTUM_INVALID, BWL_TAL_TX_BATCH, BWL_TAL_TX_BATCH, &chain);
    if (chain != NULL) {
        T->Api.TxTransferCompleteIndication(T->DataPath, WDI_TxFrameStatus_TransferFailed, chain);
    }
}

static VOID
TalTxTalSendComplete(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL, _In_ WDI_TX_FRAME_STATUS TxFrameStatus)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(pNBL);
    UNREFERENCED_PARAMETER(TxFrameStatus);
}

static VOID
TalTxAbort(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
           _Out_ NDIS_STATUS *pWifiStatus)
{
    /* nothing of WDI's is held: every frame was completed when copied */
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    *pWifiStatus = NDIS_STATUS_SUCCESS;
}

static VOID
TalTxQueueInOrder(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PEER_ID PeerId, _In_ UINT32 ExTidBitmask)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTidBitmask);
}

static VOID
TalTxPeerBacklog(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
                 _In_ BOOLEAN bBacklogged)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(bBacklogged);
}

/* ------------------------------------------------------------------ RX */

static VOID
RxRecycle(_In_ PBWL_TAL T, _In_ PNET_BUFFER_LIST Nbl)
{
    PBWL_DEVICE_CONTEXT ctx = T->Ctx;
    PWDI_FRAME_METADATA m = (PWDI_FRAME_METADATA)Nbl->MiniportReserved[0];
    BWL_RXB *b = (BWL_RXB *)Nbl->MiniportReserved[1];
    KIRQL irql;

    Nbl->MiniportReserved[0] = NULL;
    NET_BUFFER_LIST_NEXT_NBL(Nbl) = NULL;
    if (m != NULL && T->HaveApi) {
        T->Api.FreeWiFiFrameMetaData(T->DataPath, m);
    }
    if (b != NULL) {
        KeAcquireSpinLock(&ctx->DataLock, &irql);
        b->Next = T->Free;
        T->Free = b;
        KeReleaseSpinLock(&ctx->DataLock, irql);
    }
}

/* Every queued frame back to the pool (flush, stop). */
static VOID
RxDropPending(_In_ PBWL_TAL T)
{
    PBWL_DEVICE_CONTEXT ctx = T->Ctx;
    PNET_BUFFER_LIST l, next;
    KIRQL irql;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    l = T->PendHead;
    T->PendHead = T->PendTail = NULL;
    KeReleaseSpinLock(&ctx->DataLock, irql);
    for (; l != NULL; l = next) {
        next = NET_BUFFER_LIST_NEXT_NBL(l);
        RxRecycle(T, l);
        ctx->RxDrops++;
    }
}

/* The worker: tell WDI what is queued, unless held, stopped or paused. */
static VOID
RxKick(_In_ PBWL_TAL T)
{
    PBWL_DEVICE_CONTEXT ctx = T->Ctx;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;
    BOOLEAN go, confirm = FALSE;
    KIRQL irql;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    go = T->HaveApi && T->Started && T->GroupPeer && T->PendHead != NULL && !ctx->RxHold && !T->RxStopped &&
         !T->RxPaused;
    if (go) {
        T->Indicating = TRUE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (!go) {
        return;
    }
    T->RxIndications++;
    T->Api.RxInorderDataIndication(T->DataPath, WDI_RX_INDICATION_PASSIVE, BWL_TAL_PEER, WDI_EXT_TID_UNKNOWN,
                                   NULL, &st);
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    T->Indicating = FALSE;
    if (st == NDIS_STATUS_PAUSED) {
        T->RxPaused = TRUE;             /* until RxResume */
    }
    if (T->StopPending) {
        T->StopPending = FALSE;
        T->RxStopped = TRUE;
        confirm = TRUE;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (confirm) {
        T->Api.RxStopConfirm(T->DataPath);
    }
}

/* BwlNetOps.rx_eth: the poller, on the worker thread. */
static VOID
BwlNetRxEth(_In_ void *C, _In_reads_(Len) const u8 *Eth, _In_ u32 Len)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)C;
    PBWL_TAL T = ctx->Tal;
    PWDI_FRAME_METADATA m;
    PNET_BUFFER nb;
    BWL_RXB *b = NULL;
    KIRQL irql;
    int n;

    if (T == NULL || !T->HaveApi || ctx->Bwl.Wlan.Link == BWL_LINK_DOWN) {
        ctx->RxDrops++;
        return;
    }
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    if (T->Started && !T->RxStopped && T->Free != NULL) {
        b = T->Free;
        T->Free = b->Next;
    }
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (b == NULL) {
        T->RxNoBuffer++;
        ctx->RxDrops++;
        return;
    }
    /* to the native 802.11 data frame Windows wants */
    n = BwlEthTo80211(ctx->Bwl.Wlan.Bssid, Eth, Len, b->Buf, BWL_TAL_RX_BUF);
    m = n > 0 ? T->Api.AllocateWiFiFrameMetaData(T->DataPath) : NULL;
    if (m == NULL) {
        if (n > 0) {
            T->RxNoMeta++;
        }
        b->Nbl->MiniportReserved[0] = NULL;
        b->Nbl->MiniportReserved[1] = b;
        RxRecycle(T, b->Nbl);
        ctx->RxDrops++;
        return;
    }
    m->pNBL = b->Nbl;
    m->u.rxMetaData.PayloadType = WDI_FRAME_MSDU;
    b->Nbl->MiniportReserved[0] = m;
    b->Nbl->MiniportReserved[1] = b;
    nb = NET_BUFFER_LIST_FIRST_NB(b->Nbl);
    NET_BUFFER_FIRST_MDL(nb) = b->Mdl;
    NET_BUFFER_CURRENT_MDL(nb) = b->Mdl;
    NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
    NET_BUFFER_DATA_OFFSET(nb) = 0;
    NET_BUFFER_DATA_LENGTH(nb) = (ULONG)n;
    NET_BUFFER_LIST_NEXT_NBL(b->Nbl) = NULL;
    NET_BUFFER_LIST_STATUS(b->Nbl) = NDIS_STATUS_SUCCESS;

    KeAcquireSpinLock(&ctx->DataLock, &irql);
    if (T->PendTail != NULL) {
        NET_BUFFER_LIST_NEXT_NBL(T->PendTail) = b->Nbl;
    } else {
        T->PendHead = b->Nbl;
    }
    T->PendTail = b->Nbl;
    KeReleaseSpinLock(&ctx->DataLock, irql);
    ctx->RxFrames++;
    RxKick(T);
}

/* The association is indicated: let the frames that waited go up. */
VOID
BwlNetRxRelease(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    KIRQL irql;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    Ctx->RxHold = FALSE;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (Ctx->Tal != NULL) {
        RxKick(Ctx->Tal);
    }
}

/* Inside RxInorderDataIndication (or later): everything queued, in order. */
static VOID
TalRxGetMpdus(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PEER_ID PeerId, _In_ WDI_EXTENDED_TID ExTid,
              _Out_ PNET_BUFFER_LIST *ppNBL)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    KIRQL irql;

    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTid);
    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    *ppNBL = T->PendHead;
    T->PendHead = T->PendTail = NULL;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
}

static VOID
TalRxReturnFrames(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    PNET_BUFFER_LIST next;

    for (; pNBL != NULL; pNBL = next) {
        next = NET_BUFFER_LIST_NEXT_NBL(pNBL);
        RxRecycle(T, pNBL);
    }
}

static VOID
TalRxStop(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _Out_ NDIS_STATUS *pWifiStatus)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    KIRQL irql;

    UNREFERENCED_PARAMETER(PortId);
    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    if (T->Indicating) {
        T->StopPending = TRUE;          /* RxStopConfirm when the indication returns */
        *pWifiStatus = NDIS_STATUS_PENDING;
    } else {
        T->RxStopped = TRUE;
        *pWifiStatus = NDIS_STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
}

static VOID
TalRxFlush(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId)
{
    PBWL_TAL T = (PBWL_TAL)Context;

    UNREFERENCED_PARAMETER(PortId);
    RxDropPending(T);
    T->Api.RxFlushConfirm(T->DataPath);
}

static VOID
TalRxRestart(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    KIRQL irql;

    UNREFERENCED_PARAMETER(PortId);
    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->RxStopped = FALSE;
    T->StopPending = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    KeSetEvent(&T->Ctx->WakeEvent, IO_NO_INCREMENT, FALSE);    /* the worker indicates what waits */
}

static VOID
TalRxResume(_In_ TAL_TXRX_HANDLE Context)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    KIRQL irql;

    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->RxPaused = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    KeSetEvent(&T->Ctx->WakeEvent, IO_NO_INCREMENT, FALSE);
}

static VOID
TalRxThrottle(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_RX_THROTTLE_LEVEL RxThrottleLevel)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(RxThrottleLevel);
}

/* The link's signal; the firmware does not give it per frame. */
static VOID
TalRxPpduRssi(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL, _Out_ UINT8 *pRssi)
{
    PBWL_TAL T = (PBWL_TAL)Context;

    UNREFERENCED_PARAMETER(pNBL);
    *pRssi = (UINT8)(INT8)T->Ctx->Bwl.Wlan.Rssi;
}

/* ------------------------------------------------------ worker side */

/* Worker thread: what the FIFO holds, to the firmware, while it takes it;
 * then WDI's TX restarted if it was paused, and RX that waits indicated. */
BOOLEAN
BwlNetTxPump(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    PBWL_TAL T = Ctx->Tal;
    BOOLEAN sent = FALSE, restart = FALSE;
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
        /* only this thread pops, and a push never touches the oldest slot */
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
    if (T == NULL) {
        return sent;
    }
    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (T->TxPaused && Ctx->TxQ != NULL && BwlFrameqCount(Ctx->TxQ) < BWL_FRAMEQ_SLOTS / 2) {
        T->TxPaused = FALSE;
        restart = T->HaveApi;
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (restart) {
        T->TxRestarts++;
        T->Api.TxSendRestartIndication(T->DataPath, WDI_PORT_ANY, WDI_PEER_ANY, WDI_EXT_TID_UNKNOWN,
                                       WDI_TX_PAUSE_REASON_CREDIT);
    }
    RxKick(T);
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

/* Leaving a network: the TX FIFO emptied, RX held and dropped. */
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
    if (Ctx->Tal != NULL) {
        RxDropPending(Ctx->Tal);
    }
}

/* -------------------------------------------------------------- TAL */

static NDIS_STATUS
TalStart(_In_ TAL_TXRX_HANDLE Context, _In_ PWDI_TXRX_TARGET_CONFIGURATION pWifiTxRxConfiguration,
         _Out_ PTAL_TXRX_PARAMETERS pTalTxRxParameters)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    KIRQL irql;

    UNREFERENCED_PARAMETER(pWifiTxRxConfiguration);
    if (pTalTxRxParameters != NULL) {
        pTalTxRxParameters->MaxOutstandingTransfers = BWL_FRAMEQ_SLOTS;
    }
    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->Started = TRUE;
    T->RxStopped = FALSE;
    T->RxPaused = FALSE;
    T->TxPaused = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    BwlCrumb(T->Ctx, "wdi: data path started");
    return NDIS_STATUS_SUCCESS;
}

static VOID
TalStop(_In_ TAL_TXRX_HANDLE Context)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    KIRQL irql;

    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->Started = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    RxDropPending(T);
    Put(T, L"TalTxSends", T->TxSends);
    Put(T, L"TalTxPauses", T->TxPauses);
    Put(T, L"TalTxRestarts", T->TxRestarts);
    Put(T, L"TalRxIndications", T->RxIndications);
    Put(T, L"TalRxNoBuffer", T->RxNoBuffer);
    Put(T, L"TalRxNoMeta", T->RxNoMeta);
}

/* The port's group peer: where every received frame is put. */
static VOID
TalAddPort(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_OPERATION_MODE OpMode)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    WDI_MAC_ADDRESS bcast;

    UNREFERENCED_PARAMETER(OpMode);
    if (T->GroupPeer) {
        return;
    }
    RtlFillMemory(bcast.Address, sizeof(bcast.Address), 0xFF);
    T->Port = PortId;
    T->GroupPeer = TRUE;
    T->Api.PeerCreateIndication(T->DataPath, PortId, BWL_TAL_PEER, bcast);
    Put(T, L"TalPort", PortId);
}

static VOID
TalDeletePort(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId)
{
    PBWL_TAL T = (PBWL_TAL)Context;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;

    if (!T->GroupPeer || PortId != T->Port) {
        return;
    }
    RxDropPending(T);
    T->GroupPeer = FALSE;
    T->Api.PeerDeleteIndication(T->DataPath, PortId, BWL_TAL_PEER, &st);
}

static VOID
TalSetPortOpMode(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_OPERATION_MODE Opmode)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(Opmode);
}

static VOID
TalResetPort(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId)
{
    UNREFERENCED_PARAMETER(PortId);
    RxDropPending((PBWL_TAL)Context);
}

static VOID
TalPeerConfig(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
              _In_ PWDI_TXRX_PEER_CFG pPeerCfg)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(pPeerCfg);
}

static VOID
TalPeerDeleteConfirm(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
}

NDIS_STATUS
BwlTalInitialize(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
                 _In_ PNDIS_WDI_DATA_API NdisWdiDataPathApi, _Out_ PTAL_TXRX_HANDLE pMiniportTalTxRxContext,
                 _Inout_ PNDIS_MINIPORT_WDI_DATA_HANDLERS pMiniportDataHandlers,
                 _Out_ UINT32 *pMiniportWdiFrameMetadataExtraSpace)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)MiniportAdapterContext;
    PBWL_TAL T = ctx->Tal;
    NDIS_MINIPORT_WDI_DATA_HANDLERS h;

    *pMiniportTalTxRxContext = NULL;
    *pMiniportWdiFrameMetadataExtraSpace = 0;
    BwlCrumb(ctx, "wdi: TAL initialize");
    if (T == NULL) {
        return NDIS_STATUS_RESOURCES;
    }
    T->DataPath = NdisMiniportDataPathHandle;
    /* WDI's table as WDI sized it: revision 1 or 2 */
    RtlZeroMemory(&T->Api, sizeof(T->Api));
    RtlCopyMemory(&T->Api, NdisWdiDataPathApi,
                  min((SIZE_T)NdisWdiDataPathApi->Header.Size, sizeof(T->Api)));
    if (!NT_SUCCESS(AllocRxPool(T))) {
        BwlCrumb(ctx, "wdi: RX pool failed");
        return NDIS_STATUS_RESOURCES;
    }

    RtlZeroMemory(&h, sizeof(h));
    h.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_WDI_DATA_HANDLERS;
    h.Header.Revision = NDIS_OBJECT_TYPE_MINIPORT_WDI_DATA_HANDLERS_REVISION_1;
    h.Header.Size = NDIS_SIZEOF_MINIPORT_WDI_DATA_HANDLERS_REVISION_1;
    h.TxAbortHandler = TalTxAbort;
    h.TxTargetDescInitHandler = TalTxDescInit;
    h.TxTargetDescDeInitHandler = TalTxDescDeInit;
    h.TxDataSendHandler = TalTxDataSend;
    h.TxTalSendHandler = TalTxTalSend;
    h.TxTalSendCompleteHandler = TalTxTalSendComplete;
    h.TxTalQueueInOrderHandler = TalTxQueueInOrder;
    h.TxPeerBacklogHandler = TalTxPeerBacklog;
    h.RxStopHandler = TalRxStop;
    h.RxFlushHandler = TalRxFlush;
    h.RxRestartHandler = TalRxRestart;
    h.RxGetMpdusHandler = TalRxGetMpdus;
    h.RxReturnFramesHandler = TalRxReturnFrames;
    h.RxResumeHandler = TalRxResume;
    h.RxThrottleHandler = TalRxThrottle;
    h.RxPpduRssiHandler = TalRxPpduRssi;
    h.TalTxRxStartHandler = TalStart;
    h.TalTxRxStopHandler = TalStop;
    h.TalTxRxAddPortHandler = TalAddPort;
    h.TalTxRxDeletePortHandler = TalDeletePort;
    h.TalTxRxSetPortOpModeHandler = TalSetPortOpMode;
    h.TalTxRxResetPortHandler = TalResetPort;
    h.TalTxRxPeerConfigHandler = TalPeerConfig;
    h.TalTxRxPeerDeleteConfirmHandler = TalPeerDeleteConfirm;
    RtlCopyMemory(pMiniportDataHandlers, &h, NDIS_SIZEOF_MINIPORT_WDI_DATA_HANDLERS_REVISION_1);
    T->HaveApi = TRUE;
    *pMiniportTalTxRxContext = T;
    Put(T, L"TalApiSize", NdisWdiDataPathApi->Header.Size);
    return NDIS_STATUS_SUCCESS;
}

VOID
BwlTalDeinitialize(_In_ TAL_TXRX_HANDLE MiniportTalTxRxContext)
{
    PBWL_TAL T = (PBWL_TAL)MiniportTalTxRxContext;

    if (T == NULL) {
        return;
    }
    RxDropPending(T);
    T->HaveApi = FALSE;
    FreeRxPool(T);
    BwlCrumb(T->Ctx, "wdi: TAL deinitialized");
}
