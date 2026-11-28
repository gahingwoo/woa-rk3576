/*
Transmit queue behavior. Similar to the receive queue.
*/
#pragma once
#include "queue_common.h"

struct DeviceContext;
struct DeviceConfig;
struct ChannelRegisters;
struct MtlQueueRegisters;

// 3 = 1 hole in the ring + 1 context descriptor + 1 TSE descriptor.
UINT32 constexpr TxMaximumNumberOfFragments = QueueDescriptorMinCount - 3;

UINT16 constexpr TxLayer4HeaderOffsetLimit = 0x3FF; // 10 bits.
UINT32 constexpr TxMaximumOffloadSize = 0x3FFFF; // 18 bits.

#if DWC_EQOS_NETCX20

// Windows 10 build: transmit frames are copied into driver buffers (txqueue.cpp).
// One block per device, kept from the first transmit queue to
// DeviceReleaseHardware: one slot per descriptor.
struct TxBounce20
{
    UINT8* memory;          // Contiguous, write-combined.
    UINT64 physicalAddress;
    UINT32 slotSize;
    UINT32 count;
};

// Called by device.cpp DeviceReleaseHardware.
_IRQL_requires_(PASSIVE_LEVEL)
void
TxBounce20Free(_Inout_ TxBounce20* bounce);

#endif // DWC_EQOS_NETCX20

// Called by device.cpp AdapterCreateTxQueue.
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
TxQueueCreate(
    _Inout_ DeviceContext* deviceContext,
    _In_ DeviceConfig const& deviceConfig,
    _Inout_ NETTXQUEUE_INIT* queueInit,
    _In_ WDFDMAENABLER dma,
    _Inout_ ChannelRegisters* channelRegs,
    _Inout_ MtlQueueRegisters* mtlRegs);
