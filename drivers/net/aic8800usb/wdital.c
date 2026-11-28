/*
 * wdital.c - the data path of the WDI miniport (aic8800wdi.sys, Windows
 * 10): WDI's TAL handlers (dot11wdi.h) in place of netadapter.c's
 * NetAdapterCx queues. The firmware is a full MAC and talks Ethernet;
 * Windows talks native 802.11 data frames; aic_data.c converts. The same
 * design as bcmwlsdio's wdital.c, with the AIC8800's USB TX slots.
 *
 * TX: WDI queues per port (TxTargetPriorityQueueing) and calls TxDataSend
 * from its TX thread; the frames it gives (NdisWdiTxDequeueIndication, at
 * most one per free TX slot) are turned into the firmware's data frames
 * (aic_80211_to_eth, aic_build_tx) in the AIC_TX_SLOTS preallocated USB
 * requests and sent on the data pipe asynchronously, then handed back at
 * once (transfer complete, then send complete for those WDI asked it of).
 * With every slot in flight WDI is paused (credit); a slot's completion
 * wakes the worker, which restarts it once half the slots are free.
 *
 * RX: the worker runs the core's receive path; each Ethernet frame it
 * produces (ops->rx_eth) becomes an 802.11 frame in one of AIC_TAL_RX_BUFS
 * preallocated NET_BUFFER_LISTs with WDI frame metadata
 * (MiniportReserved[0]), is queued, and WDI is told
 * (NdisWdiRxInorderDataIndication at PASSIVE_LEVEL), which takes the
 * queue with RxGetMpdus and returns the frames with RxReturnFrames. All
 * frames go to the port's group peer, as Microsoft's SDIO WDI sample does
 * for a station. Frames that arrive before the association has been
 * indicated wait (RxHold).
 *
 * Frame memory is pool memory, touched with RtlCopyMemory or byte loads:
 * no DMA from this driver (the USB stack owns that), so nothing here is
 * the Device-type memory that faults on unaligned accesses under 19041.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic8800usb.h"
#include <dot11wdi.h>

#define AIC_TAL_TAG      't8cA'
#define AIC_TAL_RX_BUFS  64
#define AIC_TAL_RX_BUF   2048           /* an Ethernet frame plus AIC_80211_OVERHEAD */
#define AIC_TAL_TX_BATCH 16             /* frames per TxDataSend at most */
#define AIC_TAL_PEER     0              /* the group peer every frame goes to */

typedef struct _AIC_RXB {
    PNET_BUFFER_LIST Nbl;
    PMDL Mdl;
    PUCHAR Buf;
    struct _AIC_RXB *Next;
} AIC_RXB;

typedef struct _AIC_TAL {
    PAIC_DEVICE_CONTEXT Ctx;
    NDIS_HANDLE Adapter;                /* MiniportAdapterHandle, for the pool and MDLs */
    NDIS_HANDLE DataPath;               /* NdisMiniportDataPathHandle */
    NDIS_WDI_DATA_API Api;
    BOOLEAN HaveApi;
    NDIS_HANDLE NblPool;

    /* under the device's DataLock */
    AIC_RXB Rx[AIC_TAL_RX_BUFS];
    AIC_RXB *Free;
    PNET_BUFFER_LIST PendHead, PendTail;
    BOOLEAN Started, RxStopped, RxPaused, Indicating, StopPending, TxPaused, GroupPeer;
    WDI_PORT_ID Port;

    /* TxDataSend's scratch (WDI sends from one thread; the lock is for safety) */
    KSPIN_LOCK TxLock;

    ULONG TxSends, TxPauses, TxRestarts, RxIndications, RxNoBuffer, RxNoMeta;
} AIC_TAL, *PAIC_TAL;

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
static EVT_WDF_REQUEST_COMPLETION_ROUTINE TalTxDone;

MINIPORT_WDI_TAL_TXRX_INITIALIZE AicTalInitialize;
MINIPORT_WDI_TAL_TXRX_DEINITIALIZE AicTalDeinitialize;
NTSTATUS AicTalAlloc(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ NDIS_HANDLE Adapter);
VOID AicTalService(_In_ PAIC_DEVICE_CONTEXT Ctx);

/* Records from TAL calls, which are not all at PASSIVE_LEVEL. */
static VOID
Put(_In_ PAIC_TAL T, _In_ PCWSTR Name, _In_ ULONG Value)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        AicPutULong(T->Ctx, Name, Value);
    }
}

/* Windows may send from association on (EAPOL first). */
static BOOLEAN
DataPathOpen(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    return Ctx->FwReady && (Ctx->Aic.Link == AIC_LINK_ASSOCIATED || Ctx->Aic.Link == AIC_LINK_UP) &&
           !Ctx->Stopping;
}

/* --------------------------------------------------------- allocation */

/* AllocateAdapter: the TX slots and the TAL state (the RX pool comes with
 * the TAL). The requests and their memory belong to the device. */
NTSTATUS
AicTalAlloc(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ NDIS_HANDLE Adapter)
{
    WDF_OBJECT_ATTRIBUTES attr;
    PAIC_TAL t;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;

    t = (PAIC_TAL)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(AIC_TAL), AIC_TAL_TAG);
    if (t == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    t->Ctx = Ctx;
    t->Adapter = Adapter;
    KeInitializeSpinLock(&t->TxLock);
    Ctx->Tal = t;
    for (i = 0; i < AIC_TX_SLOTS && NT_SUCCESS(status); i++) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Ctx->Device;
        status = WdfRequestCreate(&attr, NULL, &Ctx->TxReq[i]);
        if (NT_SUCCESS(status)) {
            status = WdfMemoryCreate(&attr, NonPagedPoolNx, AIC_POOL_TAG, AIC_TX_SLOT_SIZE, &Ctx->TxMem[i],
                                     (PVOID *)&Ctx->TxSlotBuf[i]);
        }
    }
    C_ASSERT(AIC_TX_SLOTS == 32);   /* one bit per slot in TxFreeMask */
    Ctx->TxFreeMask = NT_SUCCESS(status) ? 0xFFFFFFFFu : 0;
    return status;
}

static VOID
FreeRxPool(_In_ PAIC_TAL T)
{
    ULONG i;

    for (i = 0; i < AIC_TAL_RX_BUFS; i++) {
        AIC_RXB *b = &T->Rx[i];

        if (b->Nbl != NULL) {
            NdisFreeNetBufferList(b->Nbl);
            b->Nbl = NULL;
        }
        if (b->Mdl != NULL) {
            NdisFreeMdl(b->Mdl);
            b->Mdl = NULL;
        }
        if (b->Buf != NULL) {
            ExFreePoolWithTag(b->Buf, AIC_TAL_TAG);
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
AllocRxPool(_In_ PAIC_TAL T)
{
    NET_BUFFER_LIST_POOL_PARAMETERS p;
    ULONG i;

    RtlZeroMemory(&p, sizeof(p));
    p.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    p.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    p.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    p.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    p.fAllocateNetBuffer = TRUE;
    p.PoolTag = AIC_TAL_TAG;
    T->NblPool = NdisAllocateNetBufferListPool(T->Adapter, &p);
    if (T->NblPool == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    T->Free = NULL;
    for (i = 0; i < AIC_TAL_RX_BUFS; i++) {
        AIC_RXB *b = &T->Rx[i];

        b->Buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, AIC_TAL_RX_BUF, AIC_TAL_TAG);
        b->Mdl = b->Buf != NULL ? NdisAllocateMdl(T->Adapter, b->Buf, AIC_TAL_RX_BUF) : NULL;
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

/* FreeAdapter (and the loader device's cleanup, which has none): the TAL
 * is deinitialized by now. */
VOID
AicNetFree(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    if (Ctx->Tal != NULL) {
        FreeRxPool(Ctx->Tal);
        ExFreePoolWithTag(Ctx->Tal, AIC_TAL_TAG);
        Ctx->Tal = NULL;
    }
}

/* WDI reports the link from the association indications. */
VOID
AicNetUpdateLink(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

/* ------------------------------------------------------------------ TX */

static VOID
TalTxDescInit(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL, _Out_ NDIS_STATUS *pWifiStatus)
{
    PWDI_FRAME_METADATA m = (PWDI_FRAME_METADATA)pNBL->MiniportReserved[0];

    UNREFERENCED_PARAMETER(Context);
    if (m != NULL) {
        m->u.txMetaData.TxCost = 1;     /* one USB slot */
    }
    *pWifiStatus = NDIS_STATUS_SUCCESS;
}

static VOID
TalTxDescDeInit(_In_ TAL_TXRX_HANDLE Context, _In_ PNET_BUFFER_LIST pNBL)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(pNBL);
}

static ULONG
FreeSlots(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    ULONG m, n = 0;

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    m = Ctx->TxFreeMask;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    for (; m != 0; m &= m - 1) {
        n++;
    }
    return n;
}

/* No USB write in flight (before a pairwise key: message 4 must leave
 * unencrypted). */
BOOLEAN
AicNetTxIdle(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    return FreeSlots(Ctx) == AIC_TX_SLOTS;
}

static VOID
TalTxDone(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target, _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
          _In_ WDFCONTEXT Context)
{
    PAIC_DEVICE_CONTEXT ctx = AicGetContext(WdfIoTargetGetDevice(Target));
    ULONG slot = (ULONG)(ULONG_PTR)Context;
    WDF_REQUEST_REUSE_PARAMS reuse;
    BOOLEAN wake;
    KIRQL irql;

    if (!NT_SUCCESS(Params->IoStatus.Status)) {
        InterlockedIncrement((volatile LONG *)&ctx->TxErrors);
    }
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    (VOID)WdfRequestReuse(Request, &reuse);
    KeAcquireSpinLock(&ctx->DataLock, &irql);
    ctx->TxFreeMask |= 1u << slot;
    wake = ctx->Tal != NULL && ctx->Tal->TxPaused;
    KeReleaseSpinLock(&ctx->DataLock, irql);
    if (wake) {
        KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);     /* the worker restarts WDI's TX */
    }
}

static ULONG
TakeSlot(_In_ PAIC_DEVICE_CONTEXT Ctx)
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
GiveSlot(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ ULONG Slot)
{
    WDF_REQUEST_REUSE_PARAMS reuse;
    KIRQL irql;

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    (VOID)WdfRequestReuse(Ctx->TxReq[Slot], &reuse);
    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    Ctx->TxFreeMask |= 1u << Slot;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
}

/* One MSDU of a frame WDI handed over: to Ethernet, to the firmware's data
 * frame, onto the data pipe. Under TxLock (TxGather, TxEth). */
static VOID
TxOne(_In_ PAIC_TAL T, _In_ PNET_BUFFER Nb)
{
    PAIC_DEVICE_CONTEXT ctx = T->Ctx;
    ULONG len = NET_BUFFER_DATA_LENGTH(Nb), slot;
    WDFMEMORY_OFFSET range;
    const UCHAR *f;
    NTSTATUS status;
    int n;

    if (len == 0 || len > sizeof(ctx->TxGather)) {
        ctx->TxDrops++;
        return;
    }
    /* contiguous already, or gathered into TxGather */
    f = (const UCHAR *)NdisGetDataBuffer(Nb, len, ctx->TxGather, 1, 0);
    if (f == NULL) {
        ctx->TxDrops++;
        return;
    }
    n = aic_80211_to_eth(f, len, ctx->TxEth, (u32)sizeof(ctx->TxEth));
    if (n < 14) {
        ctx->TxDrops++;                 /* not a frame to pass (protected, management) */
        return;
    }
    slot = TakeSlot(ctx);
    if (slot == AIC_TX_SLOTS) {
        ctx->TxDrops++;                 /* more frames than the credit WDI was given */
        return;
    }
    len = (ULONG)n;
    n = aic_build_tx(&ctx->Aic, ctx->TxEth, len, ctx->TxSlotBuf[slot], AIC_TX_SLOT_SIZE);
    if (n <= 0) {
        GiveSlot(ctx, slot);
        ctx->TxDrops++;
        return;
    }
    range.BufferOffset = 0;
    range.BufferLength = (size_t)n;
    status = WdfUsbTargetPipeFormatRequestForWrite(ctx->OutPipe[0], ctx->TxReq[slot], ctx->TxMem[slot], &range);
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(ctx->TxReq[slot], TalTxDone, (WDFCONTEXT)(ULONG_PTR)slot);
        if (WdfRequestSend(ctx->TxReq[slot], WdfUsbTargetPipeGetIoTarget(ctx->OutPipe[0]), WDF_NO_SEND_OPTIONS)) {
            ctx->TxFrames++;
            /* an EAPOL frame counts as answered from here (SetAddCipherKeys) */
            aic_tx_note(&ctx->Aic, ctx->TxEth, len);
            return;
        }
    }
    GiveSlot(ctx, slot);
    ctx->TxErrors++;
    ctx->TxDrops++;
}

static VOID
TalTxDataSend(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
              _In_ WDI_EXTENDED_TID ExTid, _In_ UINT16 NumQueueFrames, _In_ UINT32 NumActiveFrames,
              _In_ BOOLEAN bRobustnessFlag)
{
    PAIC_TAL T = (PAIC_TAL)Context;
    PAIC_DEVICE_CONTEXT ctx = T->Ctx;
    PNET_BUFFER_LIST chain = NULL, nbl;
    WDI_FRAME_ID idList[AIC_TAL_TX_BATCH * 4];
    ULONG room = FreeSlots(ctx), credit, ids = 0;
    BOOLEAN open = DataPathOpen(ctx);
    KIRQL irql;

    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTid);
    UNREFERENCED_PARAMETER(NumQueueFrames);
    UNREFERENCED_PARAMETER(NumActiveFrames);
    UNREFERENCED_PARAMETER(bRobustnessFlag);

    credit = room < AIC_TAL_TX_BATCH ? room : AIC_TAL_TX_BATCH;
    if (credit != 0 || !open) {
        /* not associated: take them anyway and fail them (dropped) */
        T->Api.TxDequeueIndication(T->DataPath, WDI_QUANTUM_INVALID, (UINT8)(open ? credit : AIC_TAL_TX_BATCH),
                                   (UINT16)(open ? credit : AIC_TAL_TX_BATCH), &chain);
    }
    if (chain == NULL && !open) {
        return;
    }
    if (chain == NULL) {
        /* every slot in flight: no more until the worker sees them back */
        KeAcquireSpinLock(&ctx->DataLock, &irql);
        T->TxPaused = TRUE;
        KeReleaseSpinLock(&ctx->DataLock, irql);
        T->TxPauses++;
        T->Api.TxSendPauseIndication(T->DataPath, WDI_PORT_ANY, WDI_PEER_ANY, WDI_EXT_TID_UNKNOWN,
                                     WDI_TX_PAUSE_REASON_CREDIT);
        KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);     /* a slot may have come back meanwhile */
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
}

/* Frames this driver injected (none): taken and done. */
static VOID
TalTxTalSend(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _In_ WDI_PEER_ID PeerId,
             _In_ WDI_EXTENDED_TID ExTid, _In_ UINT16 NumQueueFrames, _In_ UINT32 NumActiveFrames,
             _In_ BOOLEAN bRobustnessFlag)
{
    PAIC_TAL T = (PAIC_TAL)Context;
    PNET_BUFFER_LIST chain = NULL;

    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PeerId);
    UNREFERENCED_PARAMETER(ExTid);
    UNREFERENCED_PARAMETER(NumQueueFrames);
    UNREFERENCED_PARAMETER(NumActiveFrames);
    UNREFERENCED_PARAMETER(bRobustnessFlag);
    T->Api.TxDequeueIndication(T->DataPath, WDI_QUANTUM_INVALID, AIC_TAL_TX_BATCH, AIC_TAL_TX_BATCH, &chain);
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
RxRecycle(_In_ PAIC_TAL T, _In_ PNET_BUFFER_LIST Nbl)
{
    PAIC_DEVICE_CONTEXT ctx = T->Ctx;
    PWDI_FRAME_METADATA m = (PWDI_FRAME_METADATA)Nbl->MiniportReserved[0];
    AIC_RXB *b = (AIC_RXB *)Nbl->MiniportReserved[1];
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
RxDropPending(_In_ PAIC_TAL T)
{
    PAIC_DEVICE_CONTEXT ctx = T->Ctx;
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
RxKick(_In_ PAIC_TAL T)
{
    PAIC_DEVICE_CONTEXT ctx = T->Ctx;
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
    T->Api.RxInorderDataIndication(T->DataPath, WDI_RX_INDICATION_PASSIVE, AIC_TAL_PEER, WDI_EXT_TID_UNKNOWN,
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

/* ops->rx_eth: the core's receive path, on the worker thread. */
VOID
AicNetRxFrame(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_reads_(Len) const UCHAR *Frame, _In_ ULONG Len)
{
    PAIC_TAL T = Ctx->Tal;
    PWDI_FRAME_METADATA m;
    PNET_BUFFER nb;
    AIC_RXB *b = NULL;
    KIRQL irql;
    int n;

    if (T == NULL || !T->HaveApi || !DataPathOpen(Ctx)) {
        Ctx->RxDrops++;
        return;
    }
    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (T->Started && !T->RxStopped && T->Free != NULL) {
        b = T->Free;
        T->Free = b->Next;
    }
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    if (b == NULL) {
        T->RxNoBuffer++;
        Ctx->RxDrops++;
        return;
    }
    /* to the native 802.11 data frame Windows wants */
    n = aic_eth_to_80211(Ctx->Aic.Bssid, Frame, Len, b->Buf, AIC_TAL_RX_BUF);
    m = n > 0 ? T->Api.AllocateWiFiFrameMetaData(T->DataPath) : NULL;
    if (m == NULL) {
        if (n > 0) {
            T->RxNoMeta++;
        }
        b->Nbl->MiniportReserved[0] = NULL;
        b->Nbl->MiniportReserved[1] = b;
        RxRecycle(T, b->Nbl);
        Ctx->RxDrops++;
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

    KeAcquireSpinLock(&Ctx->DataLock, &irql);
    if (T->PendTail != NULL) {
        NET_BUFFER_LIST_NEXT_NBL(T->PendTail) = b->Nbl;
    } else {
        T->PendHead = b->Nbl;
    }
    T->PendTail = b->Nbl;
    KeReleaseSpinLock(&Ctx->DataLock, irql);
    Ctx->RxFrames++;
    RxKick(T);
}

/* The association is indicated: let the frames that waited go up. */
VOID
AicNetRxRelease(_In_ PAIC_DEVICE_CONTEXT Ctx)
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
    PAIC_TAL T = (PAIC_TAL)Context;
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
    PAIC_TAL T = (PAIC_TAL)Context;
    PNET_BUFFER_LIST next;

    for (; pNBL != NULL; pNBL = next) {
        next = NET_BUFFER_LIST_NEXT_NBL(pNBL);
        RxRecycle(T, pNBL);
    }
}

static VOID
TalRxStop(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId, _Out_ NDIS_STATUS *pWifiStatus)
{
    PAIC_TAL T = (PAIC_TAL)Context;
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
    PAIC_TAL T = (PAIC_TAL)Context;

    UNREFERENCED_PARAMETER(PortId);
    RxDropPending(T);
    T->Api.RxFlushConfirm(T->DataPath);
}

static VOID
TalRxRestart(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId)
{
    PAIC_TAL T = (PAIC_TAL)Context;
    KIRQL irql;

    UNREFERENCED_PARAMETER(PortId);
    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->RxStopped = FALSE;
    T->StopPending = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    KeSetEvent(&T->Ctx->RxEvent, IO_NO_INCREMENT, FALSE);     /* the worker indicates what waits */
}

static VOID
TalRxResume(_In_ TAL_TXRX_HANDLE Context)
{
    PAIC_TAL T = (PAIC_TAL)Context;
    KIRQL irql;

    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->RxPaused = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    KeSetEvent(&T->Ctx->RxEvent, IO_NO_INCREMENT, FALSE);
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
    PAIC_TAL T = (PAIC_TAL)Context;
    aic_dev *d = &T->Ctx->Aic;
    ULONG i = d->CurBss;
    int rssi = i < d->BssCount ? (int)d->Bss[i].Rssi : -60;

    UNREFERENCED_PARAMETER(pNBL);
    *pRssi = (UINT8)(INT8)rssi;
}

/* ------------------------------------------------------ worker side */

/* Each turn of the worker's loop: WDI's TX restarted once half the slots
 * are back, and RX that waits indicated. */
VOID
AicTalService(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    PAIC_TAL T = Ctx->Tal;
    BOOLEAN restart = FALSE;
    KIRQL irql;

    if (T == NULL) {
        return;
    }
    if (FreeSlots(Ctx) >= AIC_TX_SLOTS / 2) {
        KeAcquireSpinLock(&Ctx->DataLock, &irql);
        if (T->TxPaused) {
            T->TxPaused = FALSE;
            restart = T->HaveApi;
        }
        KeReleaseSpinLock(&Ctx->DataLock, irql);
    }
    if (restart) {
        T->TxRestarts++;
        T->Api.TxSendRestartIndication(T->DataPath, WDI_PORT_ANY, WDI_PEER_ANY, WDI_EXT_TID_UNKNOWN,
                                       WDI_TX_PAUSE_REASON_CREDIT);
    }
    RxKick(T);
}

/* -------------------------------------------------------------- TAL */

static NDIS_STATUS
TalStart(_In_ TAL_TXRX_HANDLE Context, _In_ PWDI_TXRX_TARGET_CONFIGURATION pWifiTxRxConfiguration,
         _Out_ PTAL_TXRX_PARAMETERS pTalTxRxParameters)
{
    PAIC_TAL T = (PAIC_TAL)Context;
    KIRQL irql;

    UNREFERENCED_PARAMETER(pWifiTxRxConfiguration);
    if (pTalTxRxParameters != NULL) {
        pTalTxRxParameters->MaxOutstandingTransfers = AIC_TX_SLOTS;
    }
    KeAcquireSpinLock(&T->Ctx->DataLock, &irql);
    T->Started = TRUE;
    T->RxStopped = FALSE;
    T->RxPaused = FALSE;
    T->TxPaused = FALSE;
    KeReleaseSpinLock(&T->Ctx->DataLock, irql);
    AicCrumb(T->Ctx, "wdi: data path started");
    return NDIS_STATUS_SUCCESS;
}

static VOID
TalStop(_In_ TAL_TXRX_HANDLE Context)
{
    PAIC_TAL T = (PAIC_TAL)Context;
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
    PAIC_TAL T = (PAIC_TAL)Context;
    WDI_MAC_ADDRESS bcast;

    UNREFERENCED_PARAMETER(OpMode);
    if (T->GroupPeer) {
        return;
    }
    RtlFillMemory(bcast.Address, sizeof(bcast.Address), 0xFF);
    T->Port = PortId;
    T->GroupPeer = TRUE;
    T->Api.PeerCreateIndication(T->DataPath, PortId, AIC_TAL_PEER, bcast);
    Put(T, L"TalPort", PortId);
}

static VOID
TalDeletePort(_In_ TAL_TXRX_HANDLE Context, _In_ WDI_PORT_ID PortId)
{
    PAIC_TAL T = (PAIC_TAL)Context;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;

    if (!T->GroupPeer || PortId != T->Port) {
        return;
    }
    RxDropPending(T);
    T->GroupPeer = FALSE;
    T->Api.PeerDeleteIndication(T->DataPath, PortId, AIC_TAL_PEER, &st);
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
    RxDropPending((PAIC_TAL)Context);
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
AicTalInitialize(_In_ NDIS_HANDLE MiniportAdapterContext, _In_ NDIS_HANDLE NdisMiniportDataPathHandle,
                 _In_ PNDIS_WDI_DATA_API NdisWdiDataPathApi, _Out_ PTAL_TXRX_HANDLE pMiniportTalTxRxContext,
                 _Inout_ PNDIS_MINIPORT_WDI_DATA_HANDLERS pMiniportDataHandlers,
                 _Out_ UINT32 *pMiniportWdiFrameMetadataExtraSpace)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)MiniportAdapterContext;
    PAIC_TAL T = ctx->Tal;
    NDIS_MINIPORT_WDI_DATA_HANDLERS h;

    *pMiniportTalTxRxContext = NULL;
    *pMiniportWdiFrameMetadataExtraSpace = 0;
    AicCrumb(ctx, "wdi: TAL initialize");
    if (T == NULL) {
        return NDIS_STATUS_RESOURCES;
    }
    T->DataPath = NdisMiniportDataPathHandle;
    /* WDI's table as WDI sized it: revision 1 or 2 */
    RtlZeroMemory(&T->Api, sizeof(T->Api));
    RtlCopyMemory(&T->Api, NdisWdiDataPathApi,
                  min((SIZE_T)NdisWdiDataPathApi->Header.Size, sizeof(T->Api)));
    if (!NT_SUCCESS(AllocRxPool(T))) {
        AicCrumb(ctx, "wdi: RX pool failed");
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
AicTalDeinitialize(_In_ TAL_TXRX_HANDLE MiniportTalTxRxContext)
{
    PAIC_TAL T = (PAIC_TAL)MiniportTalTxRxContext;

    if (T == NULL) {
        return;
    }
    RxDropPending(T);
    T->HaveApi = FALSE;
    FreeRxPool(T);
    AicCrumb(T->Ctx, "wdi: TAL deinitialized");
}
