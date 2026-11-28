/*
 * rk3xnvme - NVMe storport miniport for Windows 10 on the RK3576 boards.
 *
 * Windows 10 ARM64 maps the DMA common buffers of a non-coherent device
 * (_CCA 0) as Device memory, where any unaligned access faults. So every
 * access this driver makes to DMA memory goes through the accessors below:
 * naturally aligned 32 and 64-bit loads and stores, never a structure copy
 * or a memcpy. Commands are built in ordinary (cached) memory and copied
 * into the queue with aligned 64-bit stores; completions and identify data
 * are read the same way.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <ntddk.h>
#include <storport.h>

#define RKNVME_TAG 'enKR'

/* ---- NVMe controller registers (NVMe 1.4, section 3.1) ---- */
#define NVME_REG_CAP_LO   0x00
#define NVME_REG_CAP_HI   0x04
#define NVME_REG_VS       0x08
#define NVME_REG_INTMS    0x0C
#define NVME_REG_INTMC    0x10
#define NVME_REG_CC       0x14
#define NVME_REG_CSTS     0x1C
#define NVME_REG_AQA      0x24
#define NVME_REG_ASQ_LO   0x28
#define NVME_REG_ASQ_HI   0x2C
#define NVME_REG_ACQ_LO   0x30
#define NVME_REG_ACQ_HI   0x34
#define NVME_REG_DBS      0x1000
/* End of the doorbells this driver uses (admin and I/O queue 1). */
#define RKNVME_REG_DB_END(dx) (NVME_REG_DBS + (2u * RKNVME_IO_QID + 2u) * (dx)->DoorbellStride)

#define NVME_CC_EN            0x00000001u
#define NVME_CC_CSS_NVM       (0u << 4)
#define NVME_CC_MPS_4K        (0u << 7)
#define NVME_CC_AMS_RR        (0u << 11)
#define NVME_CC_SHN_MASK      (3u << 14)
#define NVME_CC_SHN_NORMAL    (1u << 14)
#define NVME_CC_IOSQES_64     (6u << 16)
#define NVME_CC_IOCQES_16     (4u << 20)

#define NVME_CSTS_RDY         0x00000001u
#define NVME_CSTS_CFS         0x00000002u
#define NVME_CSTS_SHST_MASK   (3u << 2)
#define NVME_CSTS_SHST_DONE   (2u << 2)

/* ---- Opcodes ---- */
#define NVME_ADMIN_DELETE_SQ      0x00
#define NVME_ADMIN_CREATE_SQ      0x01
#define NVME_ADMIN_DELETE_CQ      0x04
#define NVME_ADMIN_CREATE_CQ      0x05
#define NVME_ADMIN_IDENTIFY       0x06
#define NVME_ADMIN_SET_FEATURES   0x09

#define NVME_NVM_FLUSH            0x00
#define NVME_NVM_WRITE            0x01
#define NVME_NVM_READ             0x02

#define NVME_FEAT_NUM_QUEUES      0x07

#define NVME_CNS_NAMESPACE        0x00
#define NVME_CNS_CONTROLLER       0x01
#define NVME_CNS_ACTIVE_NS_LIST   0x02

/* ---- Queue geometry ---- */
#define RKNVME_PAGE           4096u
#define RKNVME_ADMIN_DEPTH    16u     /* admin SQ/CQ entries */
#define RKNVME_IO_DEPTH       64u     /* I/O SQ/CQ entries */
#define RKNVME_SLOTS          32u     /* outstanding I/O commands (< IO_DEPTH) */
#define RKNVME_MAX_XFER       (128u * 1024u)
#define RKNVME_PRP_LIST_BYTES 512u    /* per slot: 64 entries, room for MAX_XFER */
#define RKNVME_SQE_BYTES      64u
#define RKNVME_CQE_BYTES      16u
#define RKNVME_IO_QID         1u

/*
 * The DMA area, one storport uncached extension, page aligned:
 *   page 0  admin SQ        page 3  I/O CQ
 *   page 1  admin CQ        page 4  identify buffer
 *   page 2  I/O SQ          page 5+ PRP lists, RKNVME_PRP_LIST_BYTES each
 */
#define RKNVME_OFF_ASQ        (0u * RKNVME_PAGE)
#define RKNVME_OFF_ACQ        (1u * RKNVME_PAGE)
#define RKNVME_OFF_IOSQ       (2u * RKNVME_PAGE)
#define RKNVME_OFF_IOCQ       (3u * RKNVME_PAGE)
#define RKNVME_OFF_IDENT      (4u * RKNVME_PAGE)
#define RKNVME_OFF_PRP        (5u * RKNVME_PAGE)
#define RKNVME_DMA_BYTES      (RKNVME_OFF_PRP + RKNVME_SLOTS * RKNVME_PRP_LIST_BYTES)

C_ASSERT(RKNVME_ADMIN_DEPTH * RKNVME_SQE_BYTES <= RKNVME_PAGE);
C_ASSERT(RKNVME_IO_DEPTH * RKNVME_SQE_BYTES <= RKNVME_PAGE);
C_ASSERT(RKNVME_SLOTS < RKNVME_IO_DEPTH);
C_ASSERT((RKNVME_MAX_XFER / RKNVME_PAGE + 1) * 8 <= RKNVME_PRP_LIST_BYTES);
C_ASSERT((RKNVME_PAGE % RKNVME_PRP_LIST_BYTES) == 0);

/* A submission queue entry, built in cached memory and copied as 8 ULONG64s. */
typedef struct _NVME_SQE {
    ULONG Cdw0;     /* opcode [7:0], fuse [9:8], psdt [15:14], cid [31:16] */
    ULONG Nsid;
    ULONG Cdw2;
    ULONG Cdw3;
    ULONG64 Mptr;
    ULONG64 Prp1;
    ULONG64 Prp2;
    ULONG Cdw10;
    ULONG Cdw11;
    ULONG Cdw12;
    ULONG Cdw13;
    ULONG Cdw14;
    ULONG Cdw15;
} NVME_SQE;
C_ASSERT(sizeof(NVME_SQE) == RKNVME_SQE_BYTES);

/* One completion queue entry as read from DMA memory, a dword at a time. */
typedef struct _NVME_CQE {
    ULONG Dw0;
    ULONG Dw1;
    USHORT SqHead;
    USHORT SqId;
    USHORT Cid;
    USHORT Status;  /* phase [0], status field [15:1] */
} NVME_CQE;

typedef struct _RKNVME_QUEUE {
    PUCHAR Sq;              /* DMA memory */
    PUCHAR Cq;              /* DMA memory */
    STOR_PHYSICAL_ADDRESS SqPhys;
    STOR_PHYSICAL_ADDRESS CqPhys;
    ULONG Depth;
    ULONG SqTail;
    ULONG CqHead;
    ULONG Phase;
    PULONG SqDoorbell;
    PULONG CqDoorbell;
} RKNVME_QUEUE, *PRKNVME_QUEUE;

/* Per request, in the SRB extension. */
typedef struct _RKNVME_SRB_EXT {
    ULONG Slot;
} RKNVME_SRB_EXT, *PRKNVME_SRB_EXT;

typedef struct _RKNVME_DEVEXT {
    PUCHAR Regs;
    ULONG RegsLength;
    ULONG DoorbellStride;   /* bytes */
    ULONG TimeoutMs;        /* CAP.TO in ms */
    BOOLEAN DumpMode;
    BOOLEAN Ready;          /* queues up, I/O may be issued */
    BOOLEAN ShutDown;

    PUCHAR Dma;             /* uncached extension, RKNVME_DMA_BYTES */
    STOR_PHYSICAL_ADDRESS DmaPhys;

    RKNVME_QUEUE Admin;
    RKNVME_QUEUE Io;
    USHORT AdminCid;

    /* Outstanding I/O, by command id (= slot). */
    PSCSI_REQUEST_BLOCK Slot[RKNVME_SLOTS];
    ULONG Outstanding;
    ULONG TimerArmed;

    STOR_PHYSICAL_ADDRESS IdentPhys;
    STOR_PHYSICAL_ADDRESS PrpPhys[RKNVME_SLOTS];
    ULONG IoDepth;          /* I/O queue entries, at most RKNVME_IO_DEPTH */
    ULONG Slots;            /* usable slots, at most RKNVME_SLOTS */

    /* Identify data, copied out of DMA memory into cached memory
       (ULONG64 arrays so the copy is aligned on both sides). */
    ULONG64 IdCtrl[RKNVME_PAGE / 8];
    ULONG64 IdNs[RKNVME_PAGE / 8];
    ULONG Nsid;
    ULONG64 NsBlocks;       /* NSZE */
    ULONG BlockShift;       /* LBA data size, log2 */
    ULONG MaxXfer;          /* bytes */
    BOOLEAN VolatileCache;  /* VWC */

    ULONG InterruptCount;
    ULONG SpuriousCount;
} RKNVME_DEVEXT, *PRKNVME_DEVEXT;
