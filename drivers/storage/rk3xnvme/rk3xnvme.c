/*
 * rk3xnvme - NVMe storport miniport for Windows 10 (build 19041 to 19045)
 * on the RK3576 boards.
 *
 * Why it exists: on Windows 10 ARM64 the HAL gives a device whose DMA is not
 * cache coherent (_CCA 0, which is the truth on RK3576) its common buffers
 * with PAGE_NOCACHE, and Windows 10's memory manager maps PAGE_NOCACHE with
 * MAIR index 1, a Device memory type. Device memory faults on any unaligned
 * access. The inbox stornvme.sys reads its identify data with an 8-byte LDP
 * at offset 4 and bugchecks (0x7E, STATUS_DATATYPE_MISALIGNMENT). Windows 11
 * maps PAGE_NOCACHE with index 2 (Normal non-cacheable) and is not affected.
 *
 * This driver keeps its queues, PRP lists and identify buffer in the storport
 * uncached extension (the HAL's own non-coherent DMA memory, so no cache
 * maintenance is needed for them) and touches that memory only through
 * naturally aligned 32 and 64-bit loads and stores (Ld32, Ld64, St64).
 * Data buffers come from storport's scatter/gather lists; storport and the
 * HAL do their cache maintenance.
 *
 * One admin queue pair, one I/O queue pair, namespace 1 (or the first active
 * namespace) as LUN 0. Storport half-duplex: StartIo, the interrupt routines
 * and the timer poll (through StorPortSynchronizeAccess) never run at once.
 * Line-based and message-signalled interrupts are both handled; a 10 ms timer
 * also polls the completion queue while commands are outstanding, so a lost
 * interrupt delays a request instead of hanging it.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "rk3xnvme.h"

sp_DRIVER_INITIALIZE DriverEntry;
HW_FIND_ADAPTER RkNvmeFindAdapter;
HW_INITIALIZE RkNvmeInitialize;
HW_STARTIO RkNvmeStartIo;
HW_INTERRUPT RkNvmeInterrupt;
HW_MESSAGE_SIGNALED_INTERRUPT_ROUTINE RkNvmeMsiInterrupt;
HW_RESET_BUS RkNvmeResetBus;
HW_ADAPTER_CONTROL RkNvmeAdapterControl;
HW_TIMER RkNvmeTimer;
STOR_SYNCHRONIZED_ACCESS RkNvmeSyncPoll;

/* ------------------------------------------------------------------ */
/* DMA memory accessors: aligned, one access each, never combined.     */
/* ------------------------------------------------------------------ */

static __forceinline ULONG Ld32(const UCHAR *p)
{
    return *(const volatile ULONG *)p;
}

static __forceinline ULONG64 Ld64(const UCHAR *p)
{
    return *(const volatile ULONG64 *)p;
}


static __forceinline VOID St64(UCHAR *p, ULONG64 v)
{
    *(volatile ULONG64 *)p = v;
}

/* Writes to DMA memory reach memory before the doorbell write that follows. */
static __forceinline VOID DmaWriteBarrier(VOID)
{
    __dsb(_ARM64_BARRIER_SY);
}

/* A completion's other dwords are read only after its phase bit was seen. */
static __forceinline VOID DmaReadBarrier(VOID)
{
    __dmb(_ARM64_BARRIER_SY);
}

static VOID DmaZero(UCHAR *p, ULONG bytes)
{
    ULONG i;
    for (i = 0; i < bytes; i += 8) {
        St64(p + i, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static ULONG RegRead(PRKNVME_DEVEXT dx, ULONG off)
{
    return StorPortReadRegisterUlong(dx, (PULONG)(dx->Regs + off));
}

static VOID RegWrite(PRKNVME_DEVEXT dx, ULONG off, ULONG v)
{
    StorPortWriteRegisterUlong(dx, (PULONG)(dx->Regs + off), v);
}

static VOID RegWrite64(PRKNVME_DEVEXT dx, ULONG off, ULONG64 v)
{
    /* Two 32-bit writes, low half first (NVMe 1.4, 3.1). */
    RegWrite(dx, off, (ULONG)v);
    RegWrite(dx, off + 4, (ULONG)(v >> 32));
}

/* Waits up to ms milliseconds for (CSTS & mask) == want. */
static BOOLEAN WaitCsts(PRKNVME_DEVEXT dx, ULONG mask, ULONG want, ULONG ms)
{
    ULONG i;
    for (i = 0; i <= ms; i++) {
        ULONG csts = RegRead(dx, NVME_REG_CSTS);
        if (csts == 0xFFFFFFFFu) {
            return FALSE;   /* the device is gone from the bus */
        }
        if ((csts & mask) == want) {
            return TRUE;
        }
        StorPortStallExecution(1000);
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Queues                                                              */
/* ------------------------------------------------------------------ */

static VOID QueueReset(PRKNVME_QUEUE q)
{
    q->SqTail = 0;
    q->CqHead = 0;
    q->Phase = 1;
}

static VOID QueueSubmit(PRKNVME_DEVEXT dx, PRKNVME_QUEUE q, const NVME_SQE *sqe)
{
    const ULONG64 *src = (const ULONG64 *)sqe;
    UCHAR *dst = q->Sq + q->SqTail * RKNVME_SQE_BYTES;
    ULONG i;

    for (i = 0; i < RKNVME_SQE_BYTES / 8; i++) {
        St64(dst + i * 8, src[i]);
    }
    q->SqTail = (q->SqTail + 1) % q->Depth;
    DmaWriteBarrier();
    StorPortWriteRegisterUlong(dx, q->SqDoorbell, q->SqTail);
}

/* Takes the completion at the head if the controller has posted it. */
static BOOLEAN QueuePeek(PRKNVME_QUEUE q, NVME_CQE *cqe)
{
    const UCHAR *e = q->Cq + q->CqHead * RKNVME_CQE_BYTES;
    ULONG dw3 = Ld32(e + 12);
    ULONG dw2;

    if (((dw3 >> 16) & 1u) != q->Phase) {
        return FALSE;
    }
    DmaReadBarrier();
    cqe->Dw0 = Ld32(e + 0);
    cqe->Dw1 = Ld32(e + 4);
    dw2 = Ld32(e + 8);
    cqe->SqHead = (USHORT)dw2;
    cqe->SqId = (USHORT)(dw2 >> 16);
    cqe->Cid = (USHORT)dw3;
    cqe->Status = (USHORT)(dw3 >> 16);

    q->CqHead++;
    if (q->CqHead == q->Depth) {
        q->CqHead = 0;
        q->Phase ^= 1u;
    }
    return TRUE;
}

static VOID QueueAckHead(PRKNVME_DEVEXT dx, PRKNVME_QUEUE q)
{
    StorPortWriteRegisterUlong(dx, q->CqDoorbell, q->CqHead);
}

static ULONG CqeStatus(const NVME_CQE *cqe)
{
    return (cqe->Status >> 1) & 0x7FFFu;    /* SC [7:0], SCT [10:8], CRD, M, DNR */
}

/* Runs one admin command to completion by polling. PASSIVE or DIRQL. */
static BOOLEAN AdminSync(PRKNVME_DEVEXT dx, NVME_SQE *sqe, NVME_CQE *out, ULONG ms)
{
    USHORT cid = dx->AdminCid++;
    ULONG i;
    NVME_CQE cqe;

    sqe->Cdw0 = (sqe->Cdw0 & 0xFFFFu) | ((ULONG)cid << 16);
    QueueSubmit(dx, &dx->Admin, sqe);
    for (i = 0; i < ms * 100u; i++) {
        while (QueuePeek(&dx->Admin, &cqe)) {
            QueueAckHead(dx, &dx->Admin);
            if (cqe.Cid == cid) {
                if (out != NULL) {
                    *out = cqe;
                }
                return CqeStatus(&cqe) == 0;
            }
        }
        StorPortStallExecution(10);
    }
    return FALSE;
}

static VOID IdentBufferCopy(PRKNVME_DEVEXT dx, ULONG64 *dst)
{
    const UCHAR *src = dx->Dma + RKNVME_OFF_IDENT;
    ULONG i;
    for (i = 0; i < RKNVME_PAGE / 8; i++) {
        dst[i] = Ld64(src + i * 8);
    }
}

static BOOLEAN Identify(PRKNVME_DEVEXT dx, ULONG cns, ULONG nsid, ULONG64 *dst)
{
    NVME_SQE sqe;

    DmaZero(dx->Dma + RKNVME_OFF_IDENT, RKNVME_PAGE);
    RtlZeroMemory(&sqe, sizeof(sqe));
    sqe.Cdw0 = NVME_ADMIN_IDENTIFY;
    sqe.Nsid = nsid;
    sqe.Prp1 = (ULONG64)dx->IdentPhys.QuadPart;
    sqe.Cdw10 = cns;
    if (!AdminSync(dx, &sqe, NULL, 10000)) {
        return FALSE;
    }
    IdentBufferCopy(dx, dst);
    return TRUE;
}

static ULONG IdU32(const ULONG64 *id, ULONG off)
{
    const UCHAR *b = (const UCHAR *)id + off;
    return (ULONG)b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

static ULONG64 IdU64(const ULONG64 *id, ULONG off)
{
    return (ULONG64)IdU32(id, off) | ((ULONG64)IdU32(id, off + 4) << 32);
}

static UCHAR IdU8(const ULONG64 *id, ULONG off)
{
    return ((const UCHAR *)id)[off];
}

static BOOLEAN ReadNamespace(PRKNVME_DEVEXT dx, ULONG nsid)
{
    ULONG flbas, lbaf, lbads;

    if (!Identify(dx, NVME_CNS_NAMESPACE, nsid, dx->IdNs)) {
        return FALSE;
    }
    dx->NsBlocks = IdU64(dx->IdNs, 0);
    if (dx->NsBlocks == 0) {
        return FALSE;
    }
    flbas = IdU8(dx->IdNs, 26) & 0x0Fu;
    lbaf = IdU32(dx->IdNs, 128 + flbas * 4);
    lbads = (lbaf >> 16) & 0xFFu;
    if (lbads < 9 || lbads > 12 || (lbaf & 0xFFFFu) != 0) {
        /* Only 512 B to 4 KiB blocks without inline metadata. */
        return FALSE;
    }
    dx->BlockShift = lbads;
    dx->Nsid = nsid;
    return TRUE;
}

/*
 * Brings the controller from any state (UEFI leaves it enabled) to an
 * admin queue, one I/O queue pair and a namespace. Polled: callable from
 * HwFindAdapter, the reset path and ScsiRestartAdapter.
 */
static BOOLEAN InitController(PRKNVME_DEVEXT dx)
{
    NVME_SQE sqe;
    NVME_CQE cqe;
    ULONG cc, mdts, nn;

    dx->Ready = FALSE;

    cc = RegRead(dx, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        RegWrite(dx, NVME_REG_CC, cc & ~NVME_CC_EN);
    }
    if (!WaitCsts(dx, NVME_CSTS_RDY, 0, dx->TimeoutMs)) {
        return FALSE;
    }

    DmaZero(dx->Dma, RKNVME_OFF_PRP);
    QueueReset(&dx->Admin);
    QueueReset(&dx->Io);

    RegWrite(dx, NVME_REG_AQA, ((RKNVME_ADMIN_DEPTH - 1) << 16) | (RKNVME_ADMIN_DEPTH - 1));
    RegWrite64(dx, NVME_REG_ASQ_LO, (ULONG64)dx->Admin.SqPhys.QuadPart);
    RegWrite64(dx, NVME_REG_ACQ_LO, (ULONG64)dx->Admin.CqPhys.QuadPart);
    cc = NVME_CC_IOCQES_16 | NVME_CC_IOSQES_64 | NVME_CC_AMS_RR | NVME_CC_MPS_4K | NVME_CC_CSS_NVM;
    RegWrite(dx, NVME_REG_CC, cc);
    RegWrite(dx, NVME_REG_CC, cc | NVME_CC_EN);
    if (!WaitCsts(dx, NVME_CSTS_RDY | NVME_CSTS_CFS, NVME_CSTS_RDY, dx->TimeoutMs)) {
        return FALSE;
    }

    if (!Identify(dx, NVME_CNS_CONTROLLER, 0, dx->IdCtrl)) {
        return FALSE;
    }
    mdts = IdU8(dx->IdCtrl, 77);
    dx->MaxXfer = RKNVME_MAX_XFER;
    if (mdts != 0 && mdts < 20 && (RKNVME_PAGE << mdts) < dx->MaxXfer) {
        dx->MaxXfer = RKNVME_PAGE << mdts;
    }
    dx->VolatileCache = (IdU8(dx->IdCtrl, 525) & 1u) != 0;
    nn = IdU32(dx->IdCtrl, 516);

    /* One I/O submission and one completion queue (zero based counts). */
    RtlZeroMemory(&sqe, sizeof(sqe));
    sqe.Cdw0 = NVME_ADMIN_SET_FEATURES;
    sqe.Cdw10 = NVME_FEAT_NUM_QUEUES;
    sqe.Cdw11 = 0;
    if (!AdminSync(dx, &sqe, &cqe, 5000)) {
        return FALSE;
    }

    RtlZeroMemory(&sqe, sizeof(sqe));
    sqe.Cdw0 = NVME_ADMIN_CREATE_CQ;
    sqe.Prp1 = (ULONG64)dx->Io.CqPhys.QuadPart;
    sqe.Cdw10 = ((dx->Io.Depth - 1) << 16) | RKNVME_IO_QID;
    sqe.Cdw11 = (0u << 16) | 0x2u | 0x1u;  /* vector 0, IEN, PC */
    if (!AdminSync(dx, &sqe, NULL, 5000)) {
        return FALSE;
    }

    RtlZeroMemory(&sqe, sizeof(sqe));
    sqe.Cdw0 = NVME_ADMIN_CREATE_SQ;
    sqe.Prp1 = (ULONG64)dx->Io.SqPhys.QuadPart;
    sqe.Cdw10 = ((dx->Io.Depth - 1) << 16) | RKNVME_IO_QID;
    sqe.Cdw11 = (RKNVME_IO_QID << 16) | 0x1u;  /* CQ 1, priority 0, PC */
    if (!AdminSync(dx, &sqe, NULL, 5000)) {
        return FALSE;
    }

    if (!ReadNamespace(dx, 1)) {
        /* Namespace 1 is not active: take the first one that is. The
           list lands in IdNs, which ReadNamespace then overwrites. */
        ULONG first;
        if (nn < 2 || !Identify(dx, NVME_CNS_ACTIVE_NS_LIST, 0, dx->IdNs)) {
            return FALSE;
        }
        first = IdU32(dx->IdNs, 0);
        if (first == 0 || first == 1 || !ReadNamespace(dx, first)) {
            return FALSE;
        }
    }

    dx->Ready = TRUE;
    return TRUE;
}

static VOID ShutdownController(PRKNVME_DEVEXT dx)
{
    ULONG cc = RegRead(dx, NVME_REG_CC);
    ULONG ms = dx->TimeoutMs < 5000 ? dx->TimeoutMs : 5000;

    dx->Ready = FALSE;
    if (cc == 0xFFFFFFFFu || !(cc & NVME_CC_EN)) {
        return;
    }
    RegWrite(dx, NVME_REG_CC, (cc & ~NVME_CC_SHN_MASK) | NVME_CC_SHN_NORMAL);
    (VOID)WaitCsts(dx, NVME_CSTS_SHST_MASK, NVME_CSTS_SHST_DONE, ms);
}

/* ------------------------------------------------------------------ */
/* SCSI helpers                                                        */
/* ------------------------------------------------------------------ */

static VOID SetSense(PSCSI_REQUEST_BLOCK srb, UCHAR key, UCHAR asc, UCHAR ascq)
{
    srb->SrbStatus = SRB_STATUS_ERROR;
    srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    if (srb->SenseInfoBuffer != NULL && srb->SenseInfoBufferLength >= 18 &&
        !(srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        PUCHAR s = (PUCHAR)srb->SenseInfoBuffer;
        RtlZeroMemory(s, srb->SenseInfoBufferLength);
        s[0] = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
        s[2] = key;
        s[7] = 10;      /* additional sense length */
        s[12] = asc;
        s[13] = ascq;
        srb->SenseInfoBufferLength = 18;
        srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
}

/* Copies a response into the request's buffer, clipped to both lengths. */
static VOID Reply(PSCSI_REQUEST_BLOCK srb, const VOID *data, ULONG len, ULONG alloc)
{
    ULONG n = len;
    if (n > alloc) {
        n = alloc;
    }
    if (n > srb->DataTransferLength) {
        n = srb->DataTransferLength;
    }
    if (n != 0 && srb->DataBuffer != NULL) {
        RtlCopyMemory(srb->DataBuffer, data, n);
    }
    srb->DataTransferLength = n;
    srb->SrbStatus = SRB_STATUS_SUCCESS;
    srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID PutBe16(PUCHAR p, ULONG v)
{
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

static VOID PutBe32(PUCHAR p, ULONG v)
{
    p[0] = (UCHAR)(v >> 24);
    p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);
    p[3] = (UCHAR)v;
}

static ULONG GetBe16(const UCHAR *p)
{
    return ((ULONG)p[0] << 8) | p[1];
}

static ULONG GetBe32(const UCHAR *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

/* An identify string field (space padded ASCII) into dst, len bytes. */
static VOID IdString(const ULONG64 *id, ULONG off, ULONG len, PUCHAR dst)
{
    const UCHAR *s = (const UCHAR *)id + off;
    ULONG i;
    for (i = 0; i < len; i++) {
        UCHAR c = s[i];
        dst[i] = (UCHAR)((c >= 0x20 && c < 0x7F) ? c : ' ');
    }
}

static BOOLEAN AllZero(const UCHAR *p, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

static VOID ScsiInquiry(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb)
{
    const UCHAR *cdb = srb->Cdb;
    ULONG alloc = GetBe16(cdb + 3);
    UCHAR b[128];
    ULONG n;

    RtlZeroMemory(b, sizeof(b));
    if (!(cdb[1] & 1u)) {
        if (cdb[2] != 0) {
            SetSense(srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
            return;
        }
        b[0] = DIRECT_ACCESS_DEVICE;
        b[2] = 0x06;            /* SPC-4 */
        b[3] = 0x02;            /* response data format */
        b[4] = 36 - 5;
        b[7] = 0x02;            /* CmdQue */
        RtlCopyMemory(b + 8, "NVMe    ", 8);
        IdString(dx->IdCtrl, 24, 16, b + 16);   /* model number */
        IdString(dx->IdCtrl, 64, 4, b + 32);    /* firmware revision */
        Reply(srb, b, 36, alloc);
        return;
    }

    switch (cdb[2]) {
    case 0x00:                  /* supported pages */
        b[1] = 0x00;
        b[3] = 3;
        b[4] = 0x00;
        b[5] = 0x80;
        b[6] = 0x83;
        Reply(srb, b, 7, alloc);
        return;
    case 0x80:                  /* unit serial number */
        b[1] = 0x80;
        b[3] = 20;
        IdString(dx->IdCtrl, 4, 20, b + 4);
        Reply(srb, b, 24, alloc);
        return;
    case 0x83: {                /* device identification */
        const UCHAR *ns = (const UCHAR *)dx->IdNs;
        n = 4;
        if (!AllZero(ns + 120, 8)) {            /* EUI64 */
            b[n + 0] = 0x01;                    /* binary */
            b[n + 1] = 0x02;                    /* EUI-64, LUN */
            b[n + 3] = 8;
            RtlCopyMemory(b + n + 4, ns + 120, 8);
            n += 12;
        } else if (!AllZero(ns + 104, 16)) {    /* NGUID */
            b[n + 0] = 0x01;
            b[n + 1] = 0x02;
            b[n + 3] = 16;
            RtlCopyMemory(b + n + 4, ns + 104, 16);
            n += 20;
        }
        /* T10 vendor id: "NVMe", the model and serial numbers. */
        b[n + 0] = 0x02;                        /* ASCII */
        b[n + 1] = 0x01;                        /* T10 vendor id, LUN */
        b[n + 3] = 8 + 40 + 20;
        RtlCopyMemory(b + n + 4, "NVMe    ", 8);
        IdString(dx->IdCtrl, 24, 40, b + n + 12);
        IdString(dx->IdCtrl, 4, 20, b + n + 52);
        n += 4 + 68;
        b[1] = 0x83;
        PutBe16(b + 2, n - 4);
        Reply(srb, b, n, alloc);
        return;
    }
    default:
        SetSense(srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }
}

static VOID ScsiReadCapacity(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb, BOOLEAN sixteen)
{
    UCHAR b[32];
    ULONG64 last = dx->NsBlocks - 1;
    ULONG bs = 1u << dx->BlockShift;

    RtlZeroMemory(b, sizeof(b));
    if (!sixteen) {
        PutBe32(b, last > 0xFFFFFFFFull ? 0xFFFFFFFFu : (ULONG)last);
        PutBe32(b + 4, bs);
        Reply(srb, b, 8, 8);
        return;
    }
    PutBe32(b, (ULONG)(last >> 32));
    PutBe32(b + 4, (ULONG)last);
    PutBe32(b + 8, bs);
    Reply(srb, b, 32, GetBe32(srb->Cdb + 10));
}

static VOID ScsiModeSense(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb, BOOLEAN ten)
{
    const UCHAR *cdb = srb->Cdb;
    UCHAR page = (UCHAR)(cdb[2] & 0x3Fu);
    ULONG hdr = ten ? 8u : 4u;
    ULONG alloc = ten ? GetBe16(cdb + 7) : cdb[4];
    UCHAR b[64];
    ULONG n = hdr;

    RtlZeroMemory(b, sizeof(b));
    if (page == 0x08 || page == 0x3F) {     /* caching */
        b[n + 0] = 0x08;
        b[n + 1] = 0x12;
        if (dx->VolatileCache) {
            b[n + 2] = 0x04;                /* WCE */
        }
        n += 20;
    }
    if (ten) {
        PutBe16(b, n - 2);
    } else {
        b[0] = (UCHAR)(n - 1);
    }
    Reply(srb, b, n, alloc);
}

static VOID ScsiReportLuns(PSCSI_REQUEST_BLOCK srb)
{
    UCHAR b[16];
    RtlZeroMemory(b, sizeof(b));
    PutBe32(b, 8);      /* one LUN, LUN 0 */
    Reply(srb, b, 16, GetBe32(srb->Cdb + 6));
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

static LONG SlotTake(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb)
{
    ULONG i;
    for (i = 0; i < dx->Slots; i++) {
        if (dx->Slot[i] == NULL) {
            dx->Slot[i] = srb;
            dx->Outstanding++;
            return (LONG)i;
        }
    }
    return -1;
}

static VOID ArmTimer(PRKNVME_DEVEXT dx)
{
    if (!dx->DumpMode && !dx->TimerArmed) {
        dx->TimerArmed = 1;
        StorPortNotification(RequestTimerCall, dx, RkNvmeTimer, 10000);
    }
}

/*
 * PRP1/PRP2 for bytes of the request's scatter/gather list (NVMe 1.4,
 * 4.3): the first entry may start anywhere dword aligned, every later one
 * must start on a page; a list goes into the slot's PRP list page.
 */
static BOOLEAN BuildPrps(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb, ULONG slot, ULONG bytes, NVME_SQE *sqe)
{
    PSTOR_SCATTER_GATHER_LIST sg = StorPortGetScatterGatherList(dx, srb);
    ULONG64 ent[RKNVME_PRP_LIST_BYTES / 8];
    ULONG n = 0, e = 0, used = 0, want;
    ULONG64 addr;
    ULONG left;

    if (sg == NULL || sg->NumberOfElements == 0) {
        return FALSE;
    }
    addr = (ULONG64)sg->List[0].PhysicalAddress.QuadPart;
    if (addr & 3u) {
        return FALSE;
    }
    sqe->Prp1 = addr;
    want = RKNVME_PAGE - (ULONG)(addr & (RKNVME_PAGE - 1));
    left = bytes;
    for (;;) {
        ULONG take = sg->List[e].Length - used;
        if (take > want) {
            take = want;
        }
        if (take >= left) {
            break;          /* this entry ends the transfer */
        }
        if (take < want) {
            return FALSE;   /* an element ends inside a page: not expressible as PRPs */
        }
        left -= take;
        used += take;
        if (used == sg->List[e].Length) {
            e++;
            used = 0;
            if (e >= sg->NumberOfElements) {
                return FALSE;
            }
        }
        addr = (ULONG64)sg->List[e].PhysicalAddress.QuadPart + used;
        if ((addr & (RKNVME_PAGE - 1)) != 0 || n == RKNVME_PRP_LIST_BYTES / 8) {
            return FALSE;
        }
        ent[n++] = addr;
        want = RKNVME_PAGE;
    }

    if (n == 0) {
        sqe->Prp2 = 0;
    } else if (n == 1) {
        sqe->Prp2 = ent[0];
    } else {
        UCHAR *list = dx->Dma + RKNVME_OFF_PRP + slot * RKNVME_PRP_LIST_BYTES;
        ULONG i;
        for (i = 0; i < n; i++) {
            St64(list + i * 8, ent[i]);
        }
        sqe->Prp2 = (ULONG64)dx->PrpPhys[slot].QuadPart;
    }
    return TRUE;
}

/* Issues an NVM command for srb, or sets the status it completes with now. */
static VOID IssueIo(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb, UCHAR opcode, ULONG64 lba, ULONG count, BOOLEAN fua)
{
    NVME_SQE sqe;
    LONG slot;
    ULONG bytes = 0;

    if (opcode != NVME_NVM_FLUSH) {
        if (count == 0) {
            srb->DataTransferLength = 0;
            srb->SrbStatus = SRB_STATUS_SUCCESS;
            return;
        }
        if (lba >= dx->NsBlocks || count > dx->NsBlocks - lba) {
            SetSense(srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);
            return;
        }
        bytes = count << dx->BlockShift;
        if (count > 0x10000u || bytes > dx->MaxXfer || bytes > srb->DataTransferLength) {
            SetSense(srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
            return;
        }
    }

    slot = SlotTake(dx, srb);
    if (slot < 0) {
        srb->SrbStatus = SRB_STATUS_BUSY;
        return;
    }

    RtlZeroMemory(&sqe, sizeof(sqe));
    sqe.Cdw0 = opcode | ((ULONG)slot << 16);
    sqe.Nsid = dx->Nsid;
    if (opcode != NVME_NVM_FLUSH) {
        if (!BuildPrps(dx, srb, (ULONG)slot, bytes, &sqe)) {
            dx->Slot[slot] = NULL;
            dx->Outstanding--;
            srb->SrbStatus = SRB_STATUS_ERROR;
            return;
        }
        sqe.Cdw10 = (ULONG)lba;
        sqe.Cdw11 = (ULONG)(lba >> 32);
        sqe.Cdw12 = (count - 1) | (fua ? (1u << 30) : 0);
        srb->DataTransferLength = bytes;
    }
    srb->SrbStatus = SRB_STATUS_PENDING;
    QueueSubmit(dx, &dx->Io, &sqe);
    ArmTimer(dx);
}

static VOID CompleteIo(PRKNVME_DEVEXT dx, const NVME_CQE *cqe)
{
    PSCSI_REQUEST_BLOCK srb;
    ULONG st, sct, sc;

    if (cqe->Cid >= dx->Slots || dx->Slot[cqe->Cid] == NULL) {
        return;     /* not ours (or completed by a reset already) */
    }
    srb = dx->Slot[cqe->Cid];
    dx->Slot[cqe->Cid] = NULL;
    dx->Outstanding--;

    st = CqeStatus(cqe);
    sc = st & 0xFFu;
    sct = (st >> 8) & 0x7u;
    if (st == 0) {
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        srb->ScsiStatus = SCSISTAT_GOOD;
    } else if (sct == 0 && sc == 0x80) {
        SetSense(srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);
    } else if (sct == 2) {
        SetSense(srb, SCSI_SENSE_MEDIUM_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
    } else {
        SetSense(srb, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
    }
    StorPortNotification(RequestComplete, dx, srb);
}

/* Drains the I/O completion queue. Under the interrupt lock. */
static ULONG ProcessIo(PRKNVME_DEVEXT dx)
{
    NVME_CQE cqe;
    ULONG n = 0;

    if (!dx->Ready) {
        return 0;
    }
    while (QueuePeek(&dx->Io, &cqe)) {
        CompleteIo(dx, &cqe);
        n++;
    }
    if (n != 0) {
        QueueAckHead(dx, &dx->Io);
    }
    return n;
}

static VOID ScsiExecute(PRKNVME_DEVEXT dx, PSCSI_REQUEST_BLOCK srb)
{
    const UCHAR *c = srb->Cdb;
    BOOLEAN fua = (c[1] & 0x08u) != 0;

    srb->ScsiStatus = SCSISTAT_GOOD;
    switch (c[0]) {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_VERIFY:
    case SCSIOP_VERIFY16:
    case SCSIOP_RESERVE_UNIT:
    case SCSIOP_RELEASE_UNIT:
    case SCSIOP_MEDIUM_REMOVAL:
        srb->DataTransferLength = 0;
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        return;
    case SCSIOP_REQUEST_SENSE: {
        UCHAR s[18];
        RtlZeroMemory(s, sizeof(s));
        s[0] = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
        s[7] = 10;
        Reply(srb, s, sizeof(s), c[4]);
        return;
    }
    case SCSIOP_INQUIRY:
        ScsiInquiry(dx, srb);
        return;
    case SCSIOP_READ_CAPACITY:
        ScsiReadCapacity(dx, srb, FALSE);
        return;
    case SCSIOP_SERVICE_ACTION_IN16:
        if ((c[1] & 0x1Fu) == SERVICE_ACTION_READ_CAPACITY16) {
            ScsiReadCapacity(dx, srb, TRUE);
            return;
        }
        break;
    case SCSIOP_MODE_SENSE:
        ScsiModeSense(dx, srb, FALSE);
        return;
    case SCSIOP_MODE_SENSE10:
        ScsiModeSense(dx, srb, TRUE);
        return;
    case SCSIOP_REPORT_LUNS:
        ScsiReportLuns(srb);
        return;
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_SYNCHRONIZE_CACHE16:
        if (!dx->VolatileCache) {
            srb->DataTransferLength = 0;
            srb->SrbStatus = SRB_STATUS_SUCCESS;
            return;
        }
        IssueIo(dx, srb, NVME_NVM_FLUSH, 0, 0, FALSE);
        return;
    case SCSIOP_READ6:
    case SCSIOP_WRITE6: {
        ULONG lba = ((ULONG)(c[1] & 0x1Fu) << 16) | ((ULONG)c[2] << 8) | c[3];
        ULONG cnt = c[4] ? c[4] : 256u;
        IssueIo(dx, srb, c[0] == SCSIOP_READ6 ? NVME_NVM_READ : NVME_NVM_WRITE, lba, cnt, FALSE);
        return;
    }
    case SCSIOP_READ:
    case SCSIOP_WRITE:
        IssueIo(dx, srb, c[0] == SCSIOP_READ ? NVME_NVM_READ : NVME_NVM_WRITE,
                GetBe32(c + 2), GetBe16(c + 7), fua);
        return;
    case SCSIOP_READ12:
    case SCSIOP_WRITE12:
        IssueIo(dx, srb, c[0] == SCSIOP_READ12 ? NVME_NVM_READ : NVME_NVM_WRITE,
                GetBe32(c + 2), GetBe32(c + 6), fua);
        return;
    case SCSIOP_READ16:
    case SCSIOP_WRITE16:
        IssueIo(dx, srb, c[0] == SCSIOP_READ16 ? NVME_NVM_READ : NVME_NVM_WRITE,
                ((ULONG64)GetBe32(c + 2) << 32) | GetBe32(c + 6), GetBe32(c + 10), fua);
        return;
    default:
        break;
    }
    SetSense(srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_COMMAND, 0);
}

/* ------------------------------------------------------------------ */
/* Storport entry points                                               */
/* ------------------------------------------------------------------ */

static BOOLEAN PhysOf(PRKNVME_DEVEXT dx, ULONG off, ULONG need, STOR_PHYSICAL_ADDRESS *pa)
{
    ULONG len = 0;
    *pa = StorPortGetPhysicalAddress(dx, NULL, dx->Dma + off, &len);
    return pa->QuadPart != 0 && len >= need;
}

ULONG RkNvmeFindAdapter(PVOID ext, PVOID hwContext, PVOID busInfo, PCHAR argString,
                        PPORT_CONFIGURATION_INFORMATION ci, PBOOLEAN again)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;
    PACCESS_RANGE r;
    ULONG caplo, caphi, mqes, i;

    UNREFERENCED_PARAMETER(hwContext);
    UNREFERENCED_PARAMETER(busInfo);
    UNREFERENCED_PARAMETER(argString);
    *again = FALSE;

    if (ci->NumberOfAccessRanges < 1) {
        return SP_RETURN_NOT_FOUND;
    }
    r = &(*ci->AccessRanges)[0];
    if (r->RangeInMemory == FALSE || r->RangeLength < 0x2000) {
        return SP_RETURN_NOT_FOUND;
    }
    dx->Regs = (PUCHAR)StorPortGetDeviceBase(dx, ci->AdapterInterfaceType, ci->SystemIoBusNumber,
                                             r->RangeStart, r->RangeLength, FALSE);
    if (dx->Regs == NULL) {
        return SP_RETURN_ERROR;
    }
    dx->RegsLength = r->RangeLength;
    dx->DumpMode = ci->DumpMode != 0;

    caplo = RegRead(dx, NVME_REG_CAP_LO);
    caphi = RegRead(dx, NVME_REG_CAP_HI);
    if (caplo == 0xFFFFFFFFu) {
        return SP_RETURN_NOT_FOUND;
    }
    if (((caphi >> 16) & 0xFu) != 0 || !((caphi >> 5) & 1u)) {
        return SP_RETURN_BAD_CONFIG;    /* needs 4 KiB pages and the NVM command set */
    }
    mqes = (caplo & 0xFFFFu) + 1;
    dx->DoorbellStride = 4u << (caphi & 0xFu);
    dx->TimeoutMs = ((caplo >> 24) & 0xFFu) * 500u;
    if (dx->TimeoutMs < 500) {
        dx->TimeoutMs = 500;
    }
    dx->IoDepth = mqes < RKNVME_IO_DEPTH ? mqes : RKNVME_IO_DEPTH;
    dx->Slots = dx->IoDepth - 1 < RKNVME_SLOTS ? dx->IoDepth - 1 : RKNVME_SLOTS;
    if (dx->IoDepth < 2 || mqes < RKNVME_ADMIN_DEPTH) {
        return SP_RETURN_BAD_CONFIG;
    }
    if (RKNVME_REG_DB_END(dx) > dx->RegsLength) {
        return SP_RETURN_BAD_CONFIG;
    }

    ci->MaximumTransferLength = RKNVME_MAX_XFER;
    ci->NumberOfPhysicalBreaks = RKNVME_MAX_XFER / RKNVME_PAGE + 1;
    ci->AlignmentMask = 3;
    ci->NumberOfBuses = 1;
    ci->MaximumNumberOfTargets = 1;
    ci->MaximumNumberOfLogicalUnits = 1;
    ci->ScatterGather = TRUE;
    ci->Master = TRUE;
    ci->CachesData = FALSE;
    ci->Dma32BitAddresses = TRUE;
    ci->Dma64BitAddresses = SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED;
    ci->MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    ci->NeedPhysicalAddresses = TRUE;
    ci->TaggedQueuing = TRUE;
    ci->AutoRequestSense = TRUE;
    ci->MultipleRequestPerLu = TRUE;
    ci->ResetTargetSupported = FALSE;
    ci->SynchronizationModel = StorSynchronizeHalfDuplex;
    ci->HwMSInterruptRoutine = RkNvmeMsiInterrupt;
    ci->InterruptSynchronizationMode = InterruptSynchronizeAll;
    ci->MaxNumberOfIO = dx->Slots;
    ci->MaxIOsPerLun = dx->Slots;
    ci->InitialLunQueueDepth = dx->Slots;

    /* After the DMA fields: storport makes its DMA adapter from them here. */
    dx->Dma = (PUCHAR)StorPortGetUncachedExtension(dx, ci, RKNVME_DMA_BYTES);
    if (dx->Dma == NULL || ((ULONG_PTR)dx->Dma & (RKNVME_PAGE - 1)) != 0) {
        return SP_RETURN_ERROR;
    }
    if (!PhysOf(dx, RKNVME_OFF_ASQ, RKNVME_PAGE, &dx->Admin.SqPhys) ||
        !PhysOf(dx, RKNVME_OFF_ACQ, RKNVME_PAGE, &dx->Admin.CqPhys) ||
        !PhysOf(dx, RKNVME_OFF_IOSQ, RKNVME_PAGE, &dx->Io.SqPhys) ||
        !PhysOf(dx, RKNVME_OFF_IOCQ, RKNVME_PAGE, &dx->Io.CqPhys) ||
        !PhysOf(dx, RKNVME_OFF_IDENT, RKNVME_PAGE, &dx->IdentPhys)) {
        return SP_RETURN_ERROR;
    }
    for (i = 0; i < RKNVME_SLOTS; i++) {
        if (!PhysOf(dx, RKNVME_OFF_PRP + i * RKNVME_PRP_LIST_BYTES, RKNVME_PRP_LIST_BYTES, &dx->PrpPhys[i])) {
            return SP_RETURN_ERROR;
        }
    }

    dx->Admin.Sq = dx->Dma + RKNVME_OFF_ASQ;
    dx->Admin.Cq = dx->Dma + RKNVME_OFF_ACQ;
    dx->Admin.Depth = RKNVME_ADMIN_DEPTH;
    dx->Admin.SqDoorbell = (PULONG)(dx->Regs + NVME_REG_DBS + 0 * dx->DoorbellStride);
    dx->Admin.CqDoorbell = (PULONG)(dx->Regs + NVME_REG_DBS + 1 * dx->DoorbellStride);
    dx->Io.Sq = dx->Dma + RKNVME_OFF_IOSQ;
    dx->Io.Cq = dx->Dma + RKNVME_OFF_IOCQ;
    dx->Io.Depth = dx->IoDepth;
    dx->Io.SqDoorbell = (PULONG)(dx->Regs + NVME_REG_DBS + (2 * RKNVME_IO_QID) * dx->DoorbellStride);
    dx->Io.CqDoorbell = (PULONG)(dx->Regs + NVME_REG_DBS + (2 * RKNVME_IO_QID + 1) * dx->DoorbellStride);

    if (!InitController(dx)) {
        return SP_RETURN_ERROR;
    }
    if (dx->MaxXfer < RKNVME_MAX_XFER) {
        ci->MaximumTransferLength = dx->MaxXfer;
        ci->NumberOfPhysicalBreaks = dx->MaxXfer / RKNVME_PAGE + 1;
    }
    return SP_RETURN_FOUND;
}

BOOLEAN RkNvmeInitialize(PVOID ext)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;
    return dx->Ready;
}

BOOLEAN RkNvmeStartIo(PVOID ext, PSCSI_REQUEST_BLOCK srb)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;

    switch (srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        if (srb->PathId != 0 || srb->TargetId != 0) {
            srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        } else if (srb->Lun != 0) {
            srb->SrbStatus = SRB_STATUS_INVALID_LUN;
        } else if (!dx->Ready) {
            srb->SrbStatus = dx->ShutDown ? SRB_STATUS_NO_DEVICE : SRB_STATUS_BUSY;
        } else {
            ScsiExecute(dx, srb);
        }
        break;
    case SRB_FUNCTION_FLUSH:
    case SRB_FUNCTION_SHUTDOWN:
        if (dx->Ready && dx->VolatileCache) {
            IssueIo(dx, srb, NVME_NVM_FLUSH, 0, 0, FALSE);
        } else {
            srb->SrbStatus = SRB_STATUS_SUCCESS;
        }
        break;
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_RESET_LOGICAL_UNIT:
        (VOID)ProcessIo(dx);
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_ABORT_COMMAND:
    case SRB_FUNCTION_POWER:
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    default:
        srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
    if (srb->SrbStatus != SRB_STATUS_PENDING) {
        StorPortNotification(RequestComplete, dx, srb);
    }
    return TRUE;
}

BOOLEAN RkNvmeInterrupt(PVOID ext)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;
    ULONG n = ProcessIo(dx);
    if (n == 0) {
        dx->SpuriousCount++;
        return FALSE;   /* a shared line: not ours */
    }
    dx->InterruptCount++;
    return TRUE;
}

BOOLEAN RkNvmeMsiInterrupt(PVOID ext, ULONG messageId)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;
    UNREFERENCED_PARAMETER(messageId);
    if (ProcessIo(dx) == 0) {
        dx->SpuriousCount++;
    } else {
        dx->InterruptCount++;
    }
    return TRUE;
}

BOOLEAN RkNvmeSyncPoll(PVOID ext, PVOID context)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;
    UNREFERENCED_PARAMETER(context);
    (VOID)ProcessIo(dx);
    /* Decided under the interrupt lock, as ArmTimer is: no request is
       left without a poll behind it. */
    dx->TimerArmed = (dx->Outstanding != 0 && dx->Ready) ? 1u : 0u;
    return TRUE;
}

VOID RkNvmeTimer(PVOID ext)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;

    StorPortSynchronizeAccess(dx, RkNvmeSyncPoll, NULL);
    if (dx->TimerArmed) {
        StorPortNotification(RequestTimerCall, dx, RkNvmeTimer, 10000);
    }
}

/* Gives every request the controller still holds back to storport. */
static VOID FailOutstanding(PRKNVME_DEVEXT dx, UCHAR status)
{
    ULONG i;
    for (i = 0; i < RKNVME_SLOTS; i++) {
        PSCSI_REQUEST_BLOCK srb = dx->Slot[i];
        if (srb != NULL) {
            dx->Slot[i] = NULL;
            srb->SrbStatus = status;
            StorPortNotification(RequestComplete, dx, srb);
        }
    }
    dx->Outstanding = 0;
}

BOOLEAN RkNvmeResetBus(PVOID ext, ULONG pathId)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;
    UNREFERENCED_PARAMETER(pathId);

    (VOID)ProcessIo(dx);
    if (dx->Outstanding == 0) {
        return TRUE;
    }
    /* Something is stuck: disable the controller, which drops every
       command it holds, give the requests back and start over. */
    dx->Ready = FALSE;
    RegWrite(dx, NVME_REG_CC, RegRead(dx, NVME_REG_CC) & ~NVME_CC_EN);
    (VOID)WaitCsts(dx, NVME_CSTS_RDY, 0, dx->TimeoutMs);
    FailOutstanding(dx, SRB_STATUS_BUS_RESET);
    return InitController(dx);
}

SCSI_ADAPTER_CONTROL_STATUS RkNvmeAdapterControl(PVOID ext, SCSI_ADAPTER_CONTROL_TYPE type, PVOID p)
{
    PRKNVME_DEVEXT dx = (PRKNVME_DEVEXT)ext;

    switch (type) {
    case ScsiQuerySupportedControlTypes: {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST l = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)p;
        if (l->MaxControlType > ScsiQuerySupportedControlTypes) {
            l->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
        }
        if (l->MaxControlType > ScsiStopAdapter) {
            l->SupportedTypeList[ScsiStopAdapter] = TRUE;
        }
        if (l->MaxControlType > ScsiRestartAdapter) {
            l->SupportedTypeList[ScsiRestartAdapter] = TRUE;
        }
        return ScsiAdapterControlSuccess;
    }
    case ScsiStopAdapter:
        (VOID)ProcessIo(dx);
        ShutdownController(dx);
        dx->ShutDown = TRUE;
        return ScsiAdapterControlSuccess;
    case ScsiRestartAdapter:
        dx->ShutDown = FALSE;
        FailOutstanding(dx, SRB_STATUS_BUS_RESET);
        return InitController(dx) ? ScsiAdapterControlSuccess : ScsiAdapterControlUnsuccessful;
    default:
        return ScsiAdapterControlUnsuccessful;
    }
}

ULONG DriverEntry(PVOID driverObject, PVOID registryPath)
{
    HW_INITIALIZATION_DATA init;

    RtlZeroMemory(&init, sizeof(init));
    init.HwInitializationDataSize = sizeof(init);
    init.AdapterInterfaceType = PCIBus;
    init.HwInitialize = RkNvmeInitialize;
    init.HwStartIo = RkNvmeStartIo;
    init.HwInterrupt = RkNvmeInterrupt;
    /* HwFindAdapter is a PVOID in this header's HW_INITIALIZATION_DATA. */
#pragma warning(suppress: 4152)
    init.HwFindAdapter = RkNvmeFindAdapter;
    init.HwResetBus = RkNvmeResetBus;
    init.HwAdapterControl = RkNvmeAdapterControl;
    init.DeviceExtensionSize = sizeof(RKNVME_DEVEXT);
    init.SrbExtensionSize = sizeof(RKNVME_SRB_EXT);
    init.NumberOfAccessRanges = 6;
    init.MapBuffers = STOR_MAP_NON_READ_WRITE_BUFFERS;
    init.NeedPhysicalAddresses = TRUE;
    init.TaggedQueuing = TRUE;
    init.AutoRequestSense = TRUE;
    init.MultipleRequestPerLu = TRUE;
    init.SrbTypeFlags = SRB_TYPE_FLAG_SCSI_REQUEST_BLOCK;

    return StorPortInitialize(driverObject, registryPath, &init, NULL);
}
