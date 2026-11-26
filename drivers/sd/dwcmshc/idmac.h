/*++

Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>

SPDX-License-Identifier: GPL-2.0-only

Module Name:

    idmac.h

Abstract:

    The DesignWare MSHC internal DMA controller (IDMAC), 32-bit descriptors:
    the descriptor format, a descriptor table builder and the FIFOTH value
    for a DMA transfer.

    Plain C with no Windows types, so the driver and the host unit test
    (tools/idmactest) compile the same code.

    The layout follows mainline Linux drivers/mmc/host/dw_mmc.c
    (dw_mci_prepare_desc32, dw_mci_idmac_init, dw_mci_adjust_fifoth): chained
    descriptors, at most DW_MCI_DESC_DATA_LENGTH (4 KiB) each, the first one
    marked FS, the last one LD with CH and DIC cleared, so the only
    descriptor interrupt is the last one's.

--*/

#ifndef MSHC_IDMAC_H
#define MSHC_IDMAC_H

typedef unsigned int MSHC_U32;
typedef unsigned long long MSHC_U64;

//
// DES0 (control and status)
//
#define MSHC_IDMAC_DES0_OWN                 (0x1U << 31)    // owned by the IDMAC
#define MSHC_IDMAC_DES0_CES                 (0x1U << 30)    // card error summary
#define MSHC_IDMAC_DES0_ER                  (0x1U << 5)     // end of ring
#define MSHC_IDMAC_DES0_CH                  (0x1U << 4)     // DES3 is the next descriptor
#define MSHC_IDMAC_DES0_FS                  (0x1U << 3)     // first descriptor
#define MSHC_IDMAC_DES0_LD                  (0x1U << 2)     // last descriptor
#define MSHC_IDMAC_DES0_DIC                 (0x1U << 1)     // no TI/RI on completion

//
// DES1 (buffer sizes, 13 bits each)
//
#define MSHC_IDMAC_DES1_BS2_SHIFT           13
#define MSHC_IDMAC_DES1_BS2_MASK            (0x1FFFU << 13)
#define MSHC_IDMAC_DES1_BS1_SHIFT           0
#define MSHC_IDMAC_DES1_BS1_MASK            (0x1FFFU << 0)

typedef struct _MSHC_IDMAC_DESCRIPTOR {
    MSHC_U32 Des0;  // control and status
    MSHC_U32 Des1;  // buffer size
    MSHC_U32 Des2;  // buffer address
    MSHC_U32 Des3;  // next descriptor address (chained)
} MSHC_IDMAC_DESCRIPTOR, *PMSHC_IDMAC_DESCRIPTOR;

//
// Bytes per descriptor.  BS1 has 13 bits (8191 at most); mainline uses one
// page (DW_MCI_DESC_DATA_LENGTH 0x1000), and so does this driver.  Splitting
// at 4 KiB also bounds the table: see MshcIdmacCapacityFor.
//
#define MSHC_IDMAC_MAX_LENGTH_PER_ENTRY     0x1000U

#define MSHC_IDMAC_PAGE_SIZE                0x1000U

//
// The 32-bit IDMAC drives 32 address bits: every byte a descriptor names,
// and the descriptor table itself, must be below 4 GiB.
//
#define MSHC_IDMAC_ADDRESS_LIMIT            0x100000000ULL

//
// Builder results.
//
#define MSHC_IDMAC_OK                       0
#define MSHC_IDMAC_E_LENGTH                 1   // zero, or not a multiple of 4
#define MSHC_IDMAC_E_ALIGN                  2   // buffer address not 4-byte aligned
#define MSHC_IDMAC_E_ABOVE_4G               3   // buffer reaches past 4 GiB
#define MSHC_IDMAC_E_FULL                   4   // more descriptors than the table holds
#define MSHC_IDMAC_E_EMPTY                  5   // Finish with no descriptor
#define MSHC_IDMAC_E_TABLE                  6   // table misaligned or past 4 GiB

typedef struct _MSHC_IDMAC_BUILDER {
    PMSHC_IDMAC_DESCRIPTOR Table;   // CPU address of descriptor 0
    MSHC_U32 TableBus;              // bus address of descriptor 0
    MSHC_U32 Capacity;              // descriptors the table holds
    MSHC_U32 Count;                 // descriptors written
    MSHC_U32 Bytes;                 // bytes described
    int Error;                      // first error, sticky
} MSHC_IDMAC_BUILDER, *PMSHC_IDMAC_BUILDER;

//
// The table a transfer of Length bytes may need, at most: one descriptor per
// 4 KiB page the buffer touches.  A scatter/gather list only breaks at page
// boundaries, so an element of n bytes touches at least DIV_CEIL(n, 4 KiB)
// pages, and the elements together touch the pages the buffer spans, which
// is DIV_CEIL(Length, 4 KiB) + 1 at most for a buffer that does not start on
// a page.
//
// sdport (10.0.22621.608, SdhostInitializeScatterGatherDma) allocates
// DmaDescriptorSize * NumberOfMapRegisters bytes of common buffer and caps
// its maximum transfer at (NumberOfMapRegisters - 1) pages, so a table of
// this size always fits in it.
//
static __inline MSHC_U32
MshcIdmacCapacityFor(MSHC_U32 Length)
{
    return (Length + MSHC_IDMAC_PAGE_SIZE - 1) / MSHC_IDMAC_PAGE_SIZE + 1;
}

static __inline void
MshcIdmacBegin(
    PMSHC_IDMAC_BUILDER Builder,
    PMSHC_IDMAC_DESCRIPTOR Table,
    MSHC_U64 TableBus,
    MSHC_U32 Capacity
    )
{
    Builder->Table = Table;
    Builder->TableBus = (MSHC_U32)TableBus;
    Builder->Capacity = Capacity;
    Builder->Count = 0;
    Builder->Bytes = 0;
    Builder->Error = MSHC_IDMAC_OK;

    if ((TableBus & 3) != 0 ||
        TableBus + (MSHC_U64)Capacity * sizeof(MSHC_IDMAC_DESCRIPTOR) > MSHC_IDMAC_ADDRESS_LIMIT) {
        Builder->Error = MSHC_IDMAC_E_TABLE;
    }
}

//
// Describe one scatter/gather element.  Every descriptor goes out owned by
// the IDMAC, chained to the next one, with its completion interrupt off;
// MshcIdmacFinish fixes up the first and the last.
//
static __inline int
MshcIdmacAppend(
    PMSHC_IDMAC_BUILDER Builder,
    MSHC_U64 Address,
    MSHC_U32 Length
    )
{
    if (Builder->Error != MSHC_IDMAC_OK) {
        return Builder->Error;
    }

    if (Length == 0 || (Length & 3) != 0) {
        Builder->Error = MSHC_IDMAC_E_LENGTH;
        return Builder->Error;
    }

    if ((Address & 3) != 0) {
        Builder->Error = MSHC_IDMAC_E_ALIGN;
        return Builder->Error;
    }

    if (Address >= MSHC_IDMAC_ADDRESS_LIMIT ||
        Address + Length > MSHC_IDMAC_ADDRESS_LIMIT) {
        Builder->Error = MSHC_IDMAC_E_ABOVE_4G;
        return Builder->Error;
    }

    while (Length > 0) {
        MSHC_U32 Chunk = (Length < MSHC_IDMAC_MAX_LENGTH_PER_ENTRY) ?
                         Length : MSHC_IDMAC_MAX_LENGTH_PER_ENTRY;
        PMSHC_IDMAC_DESCRIPTOR Descriptor;

        if (Builder->Count >= Builder->Capacity) {
            Builder->Error = MSHC_IDMAC_E_FULL;
            return Builder->Error;
        }

        Descriptor = &Builder->Table[Builder->Count];
        Descriptor->Des1 = (Chunk << MSHC_IDMAC_DES1_BS1_SHIFT) & MSHC_IDMAC_DES1_BS1_MASK;
        Descriptor->Des2 = (MSHC_U32)Address;
        Descriptor->Des3 = Builder->TableBus +
                           (Builder->Count + 1) * (MSHC_U32)sizeof(MSHC_IDMAC_DESCRIPTOR);
        Descriptor->Des0 = MSHC_IDMAC_DES0_OWN | MSHC_IDMAC_DES0_DIC | MSHC_IDMAC_DES0_CH;

        Builder->Count++;
        Builder->Bytes += Chunk;
        Address += Chunk;
        Length -= Chunk;
    }

    return MSHC_IDMAC_OK;
}

//
// Close the table: FS on the first descriptor; on the last, LD and ER, CH
// and DIC off (its RI/TI is the transfer's), and DES3 back to the start, so
// even an IDMAC that looked past LD would find a descriptor it already
// closed rather than whatever follows the table.
//
static __inline int
MshcIdmacFinish(
    PMSHC_IDMAC_BUILDER Builder
    )
{
    PMSHC_IDMAC_DESCRIPTOR Last;

    if (Builder->Error != MSHC_IDMAC_OK) {
        return Builder->Error;
    }

    if (Builder->Count == 0) {
        Builder->Error = MSHC_IDMAC_E_EMPTY;
        return Builder->Error;
    }

    Builder->Table[0].Des0 |= MSHC_IDMAC_DES0_FS;

    Last = &Builder->Table[Builder->Count - 1];
    Last->Des3 = Builder->TableBus;
    Last->Des0 &= ~(MSHC_IDMAC_DES0_CH | MSHC_IDMAC_DES0_DIC);
    Last->Des0 |= MSHC_IDMAC_DES0_LD | MSHC_IDMAC_DES0_ER;

    return MSHC_IDMAC_OK;
}

//
// FIFOTH for a DMA transfer, as mainline dw_mci_adjust_fifoth computes it:
// TX watermark at half the FIFO, and the largest burst (MSIZE) that divides
// both the block (in FIFO words) and the free space above the TX
// watermark, with the RX watermark one below it.  A block that is not a
// whole number of 32-bit FIFO words gets single transfers.
//
#define MSHC_FIFOTH_VALUE(_Msize, _RxWmark, _TxWmark) \
    ((((MSHC_U32)(_Msize) & 0x7U) << 28) | \
     (((MSHC_U32)(_RxWmark) & 0xFFFU) << 16) | \
     ((MSHC_U32)(_TxWmark) & 0xFFFU))

static __inline MSHC_U32
MshcIdmacFifoth(
    MSHC_U32 FifoDepth,
    MSHC_U32 BlockSize
    )
{
    static const MSHC_U32 Mszs[] = { 1, 4, 8, 16, 32, 64, 128, 256 };
    MSHC_U32 BlockDepth = BlockSize / 4;
    MSHC_U32 TxWmark = FifoDepth / 2;
    MSHC_U32 TxWmarkInvers = FifoDepth - TxWmark;
    MSHC_U32 Msize = 0;
    MSHC_U32 RxWmark = 1;
    int Index = (int)(sizeof(Mszs) / sizeof(Mszs[0])) - 1;

    if ((BlockSize % 4) == 0 && BlockDepth != 0) {
        do {
            if ((BlockDepth % Mszs[Index]) == 0 &&
                (TxWmarkInvers % Mszs[Index]) == 0) {
                Msize = (MSHC_U32)Index;
                RxWmark = Mszs[Index] - 1;
                break;
            }
        } while (--Index > 0);
    }

    return MSHC_FIFOTH_VALUE(Msize, RxWmark, TxWmark);
}

#endif // MSHC_IDMAC_H
