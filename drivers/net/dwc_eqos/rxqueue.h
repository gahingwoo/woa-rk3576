/*
Receive queue behavior. Similar to the transmit queue.
*/
#pragma once

struct DeviceContext;
struct DeviceConfig;
struct ChannelRegisters;

#if DWC_EQOS_NETCX20

// Windows 10 build: driver-managed receive buffers (rxqueue.cpp). One pool
// per device, kept from the first receive queue to DeviceReleaseHardware.
struct RxBuffer20
{
    SLIST_ENTRY entry;  // Free list link. First: SLIST_ENTRY wants 16-byte alignment.
    void* virtualAddress;
    UINT64 physicalAddress;
};

struct RxPool20
{
    SLIST_HEADER freeList;
    RxBuffer20* buffers;  // Nonpaged pool, count entries.
    void* memory;         // Contiguous, write-combined.
    SIZE_T memorySize;
    UINT32 count;
};

// Called by device.cpp DeviceReleaseHardware.
_IRQL_requires_(PASSIVE_LEVEL)
void
RxPool20Free(_Inout_ RxPool20* pool);

// Called by device.cpp AdapterReturnRxBuffer20 (EVT_NET_ADAPTER_RETURN_RX_BUFFER).
_IRQL_requires_max_(DISPATCH_LEVEL)
void
RxPool20Return(_Inout_ RxPool20* pool, _In_opt_ void* returnContext);

#endif // DWC_EQOS_NETCX20

// Called by device.cpp AdapterCreateRxQueue.
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
RxQueueCreate(
    _Inout_ DeviceContext* deviceContext,
    _In_ DeviceConfig const& deviceConfig,
    _Inout_ NETRXQUEUE_INIT* queueInit,
    _In_ WDFDMAENABLER dma,
    _Inout_ ChannelRegisters* channelRegs);
