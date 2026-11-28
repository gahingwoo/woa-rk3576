#include "precomp.h"
#include "rxqueue.h"
#include "queue_common.h"
#include "device.h"
#include "registers.h"
#include "descriptors.h"
#include "trace.h"

static_assert(sizeof(RxDescriptor) == QueueDescriptorSize);

struct RxQueueContext
{
    ChannelRegisters* channelRegs;

    DeviceContext* deviceContext;
    NET_RING* packetRing;
    NET_RING* fragmentRing;
    WDFCOMMONBUFFER descBuffer;
    RxDescriptor* descVirtual;
    PHYSICAL_ADDRESS descPhysical;
    NET_EXTENSION packetIeee8021Q;
    NET_EXTENSION packetChecksum;
    NET_EXTENSION fragmentLogical;
#if DWC_EQOS_NETCX20
    NET_EXTENSION fragmentVirtual;
    NET_EXTENSION fragmentReturn;
    RxPool20* pool;
    RxBuffer20** fragBuffer;    // Buffer attached to each fragment, nonpaged pool.
#endif
    UINT32 descCount;   // A power of 2 between QueueDescriptorMinCount and QueueDescriptorMaxCount.
    UINT16 rxBufferSize;
    UINT8 rxPbl;
    bool running;

    UINT32 descBegin;   // Start of the RECEIVE region.
    UINT32 descEnd;     // End of the RECEIVE region, start of the EMPTY region.
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RxQueueContext, RxQueueGetContext)

#if DWC_EQOS_NETCX20

// Driver-managed receive buffers for NetAdapterCx 2.0 (device.cpp says why).
// One contiguous write-combined block cut into RxBufferSize() pieces; the
// MAC writes them by physical address, so this needs the DMA logical address
// to be the physical one, which RxQueueCreate checks on the descriptor ring.
// A buffer is either free (freeList), attached to a fragment (fragBuffer),
// or in the stack until EvtAdapterReturnRxBuffer. Packets the driver ignores
// keep their buffer: NetAdapterCx returns nothing for them.

static constexpr UINT32 RxPool20Tag = 'bRwD';

_Use_decl_annotations_
void
RxPool20Free(RxPool20* pool)
{
    if (pool->memory)
    {
        MmFreeContiguousMemory(pool->memory);
        pool->memory = nullptr;
    }

    if (pool->buffers)
    {
        ExFreePoolWithTag(pool->buffers, RxPool20Tag);
        pool->buffers = nullptr;
    }

    pool->count = 0;
    pool->memorySize = 0;
    InitializeSListHead(&pool->freeList);
}

_Use_decl_annotations_
void
RxPool20Return(RxPool20* pool, void* returnContext)
{
    // nullptr for the discarded NBL of a packet the driver ignored.
    if (returnContext != nullptr)
    {
        InterlockedPushEntrySList(&pool->freeList, &static_cast<RxBuffer20*>(returnContext)->entry);
    }
}

static RxBuffer20*
RxPool20Pop(_Inout_ RxPool20* pool)
{
    return CONTAINING_RECORD(InterlockedPopEntrySList(&pool->freeList), RxBuffer20, entry);
}

// Allocates the pool the first time; every time, puts every buffer on the
// free list (the previous queue, if any, has had all of them back).
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
RxPool20Reset(_Inout_ RxPool20* pool, UINT32 count, UINT32 bufferSize, PHYSICAL_ADDRESS maxPhysicalAddress)
{
    if (pool->count < count)
    {
        RxPool20Free(pool);

        pool->buffers = static_cast<RxBuffer20*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(RxBuffer20) * count, RxPool20Tag));
        if (pool->buffers == nullptr)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        PHYSICAL_ADDRESS lowest = {}, boundary = {};
        pool->memorySize = static_cast<SIZE_T>(bufferSize) * count;
        pool->memory = MmAllocateContiguousNodeMemory(
            pool->memorySize, lowest, maxPhysicalAddress, boundary,
            PAGE_READWRITE | PAGE_WRITECOMBINE, MM_ANY_NODE_OK);
        if (pool->memory == nullptr)
        {
            RxPool20Free(pool);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        auto const physical = MmGetPhysicalAddress(pool->memory).QuadPart;
        for (UINT32 i = 0; i != count; i += 1)
        {
            pool->buffers[i].virtualAddress = static_cast<UINT8*>(pool->memory) + static_cast<SIZE_T>(bufferSize) * i;
            pool->buffers[i].physicalAddress = physical + static_cast<UINT64>(bufferSize) * i;
        }

        pool->count = count;
    }

    InitializeSListHead(&pool->freeList);
    for (UINT32 i = 0; i != pool->count; i += 1)
    {
        InterlockedPushEntrySList(&pool->freeList, &pool->buffers[i].entry);
    }

    return STATUS_SUCCESS;
}

// Gives the buffers of fragments [begin, end) back to the free list.
static void
RxQueueReclaim20(_Inout_ RxQueueContext* context, UINT32 begin, UINT32 end)
{
    for (auto i = begin; i != end; i = NetRingIncrementIndex(context->fragmentRing, i))
    {
        if (context->fragBuffer[i] != nullptr)
        {
            InterlockedPushEntrySList(&context->pool->freeList, &context->fragBuffer[i]->entry);
            context->fragBuffer[i] = nullptr;
        }
    }
}

#endif // DWC_EQOS_NETCX20

// Gets channelRegs->Current_App_RxDesc, converted to an index.
static UINT32
GetDescNext(_In_ RxQueueContext* context)
{
    // DISPATCH_LEVEL
    auto const current = Read32(&context->channelRegs->Current_App_RxDesc);
    return QueueDescriptorAddressToIndex(current, context->descPhysical, context->descCount);
}

// Sets channelRegs->RxDesc_Tail_Pointer to the physical address of the descriptor at the given index.
static void
SetDescEnd(
    _In_ RxQueueContext* context,
    _In_ UINT32 index)
{
    // DISPATCH_LEVEL
    NT_ASSERT(index < context->descCount);
    UINT32 const offset = index * sizeof(RxDescriptor);
    Write32(&context->channelRegs->RxDesc_Tail_Pointer, context->descPhysical.LowPart + offset);
    context->descEnd = index;
}

static EVT_PACKET_QUEUE_START RxQueueStart;
static void
RxQueueStart(_In_ NETPACKETQUEUE queue)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = RxQueueGetContext(queue);

    context->running = true;
    context->descBegin = 0;
    context->descEnd = 0;

    Write32(&context->channelRegs->RxDesc_List_Address_Hi, context->descPhysical.HighPart);
    Write32(&context->channelRegs->RxDesc_List_Address, context->descPhysical.LowPart);
    Write32(&context->channelRegs->RxDesc_Ring_Length, context->descCount - 1);
    SetDescEnd(context, context->descEnd);

#if DBG
    auto const descNext = GetDescNext(context);
    NT_ASSERT(descNext == context->descEnd); // If this fires, we need more reset logic.
#endif

    ChannelRxControl_t rxControl = {};
    rxControl.Start = true;
    rxControl.ReceiveBufferSize = context->rxBufferSize;
    rxControl.RxPbl = context->rxPbl;
    Write32(&context->channelRegs->Rx_Control, rxControl);

    TraceEntryExit(RxQueueStart, LEVEL_INFO);
}

static EVT_PACKET_QUEUE_ADVANCE RxQueueAdvance;
static void
RxQueueAdvance(_In_ NETPACKETQUEUE queue)
{
    // DISPATCH_LEVEL
    auto const context = RxQueueGetContext(queue);
    auto const pktEnd = context->packetRing->EndIndex;
    auto const fragEnd = context->fragmentRing->EndIndex;
    auto const descMask = context->descCount - 1u;
    UINT32 descIndex, pktIndex, fragIndex;
    UINT32 ownDescriptors = 0, doneFrags = 0, queuedFrags = 0;

    /*
    Fragment indexes:
    [fragBegin] old [fragNext] new [fragEnd] owned by NetAdapterCx [fragBegin]

    Descriptor indexes:
    [descBegin] RECEIVED [descNext] RECEIVING [descEnd] EMPTY [descBegin-1]
    */

    // Indicate received packets.

    pktIndex = context->packetRing->BeginIndex;
    fragIndex = context->fragmentRing->BeginIndex;

    auto const descNext = GetDescNext(context);
    for (descIndex = context->descBegin; descIndex != descNext; descIndex = (descIndex + 1) & descMask)
    {
        NT_ASSERT(fragIndex != fragEnd);
        if (pktIndex == pktEnd || fragIndex == fragEnd)
        {
            break;
        }

        auto const& desc = context->descVirtual[descIndex];
        auto const descWrite = desc.Write;

        // Descriptor is still owned by the DMA engine?
        if (descWrite.Own)
        {
            /*
            This sometimes happens with transmit descriptors (DMA race).
            I've never seen it happen with receive descriptors, but if it does, breaking out here
            is the right way to deal with it.
            */
            TraceWrite("RxQueueAdvance-own", LEVEL_WARNING,
                TraceLoggingUInt32(descIndex, "descIndex"),
                TraceLoggingHexInt32(reinterpret_cast<UINT32 const*>(&descWrite)[3], "RDES3"));
            ownDescriptors = 1;
            break;
        }

        NT_ASSERT(descWrite.FragmentIndex == fragIndex);

        auto const pkt = NetRingGetPacketAtIndex(context->packetRing, pktIndex);
        pkt->FragmentIndex = fragIndex;
        pkt->FragmentCount = 1;
        pkt->Layout = {};

        auto const frag = NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex);
        frag->Offset = 0;
        NT_ASSERT(context->rxBufferSize <= frag->Capacity);

        if (descWrite.ErrorSummary ||
            descWrite.ContextType ||
            !descWrite.FirstDescriptor ||
            !descWrite.LastDescriptor)
        {
            if (descWrite.ErrorSummary)
            {
                TraceWrite("RxQueueAdvance-Dropped", LEVEL_INFO,
                    TraceLoggingUInt32(descIndex, "descIndex"),
                    TraceLoggingHexInt32(reinterpret_cast<UINT32 const*>(&descWrite)[3], "RDES3"));
            }
            else
            {
                TraceWrite("RxQueueAdvance-Unexpected", LEVEL_WARNING,
                    TraceLoggingUInt32(descIndex, "descIndex"),
                    TraceLoggingHexInt32(reinterpret_cast<UINT32 const*>(&descWrite)[3], "RDES3"));
            }

            pkt->Ignore = true;
            frag->ValidLength = 0;
#if DWC_EQOS_NETCX20
            RxQueueReclaim20(context, fragIndex, NetRingIncrementIndex(context->fragmentRing, fragIndex));
#endif
        }
        else
        {
            NT_ASSERT(descWrite.PacketLength >= 4); // PacketLength includes CRC
            frag->ValidLength = descWrite.PacketLength - 4;
#if DWC_EQOS_NETCX20
            context->fragBuffer[fragIndex] = nullptr; // The stack has it until EvtAdapterReturnRxBuffer.
#endif

            if (descWrite.Rdes0Valid && descWrite.OuterVlanTag != 0 && context->packetIeee8021Q.Enabled)
            {
                NET_PACKET_IEEE8021Q tag = {};
                tag.PriorityCodePoint = descWrite.OuterVlanTag >> 13;
                tag.VlanIdentifier = descWrite.OuterVlanTag;

                auto const ieee8021Q = NetExtensionGetPacketIeee8021Q(&context->packetIeee8021Q, pktIndex);
                *ieee8021Q = tag;
            }

            // If checksum offload is disabled by hardware then no IP headers will be
            // detected. If checksum offload is disabled by software then NetAdapterCx
            // will ignore our evaluation.
            if ((descWrite.IPv4HeaderPresent | descWrite.IPv6HeaderPresent) && context->packetChecksum.Enabled)
            {
                auto const checksum = NetExtensionGetPacketChecksum(&context->packetChecksum, pktIndex);
                checksum->Layer2 = NetPacketRxChecksumEvaluationValid;
                pkt->Layout.Layer2Type = NetPacketLayer2TypeEthernet;

                checksum->Layer3 = descWrite.IPHeaderError
                    ? NetPacketRxChecksumEvaluationInvalid
                    : NetPacketRxChecksumEvaluationValid;
                pkt->Layout.Layer3Type = descWrite.IPv4HeaderPresent
                    ? NetPacketLayer3TypeIPv4UnspecifiedOptions
                    : NetPacketLayer3TypeIPv6UnspecifiedExtensions;

                checksum->Layer4 = descWrite.IPPayloadError
                    ? NetPacketRxChecksumEvaluationInvalid
                    : NetPacketRxChecksumEvaluationValid;
                switch (descWrite.PayloadType)
                {
                case RxPayloadTypeUdp:
                    pkt->Layout.Layer4Type = NetPacketLayer4TypeUdp;
                    break;
                case RxPayloadTypeTcp:
                    pkt->Layout.Layer4Type = NetPacketLayer4TypeTcp;
                    break;
                default:
                    pkt->Layout.Layer4Type = NetPacketLayer4TypeUnspecified;
                    break;
                }
            }
        }

        pktIndex = NetRingIncrementIndex(context->packetRing, pktIndex);
        fragIndex = NetRingIncrementIndex(context->fragmentRing, fragIndex);
        doneFrags += 1;
    }

    context->descBegin = descIndex;
    context->packetRing->BeginIndex = pktIndex;
    context->fragmentRing->BeginIndex = fragIndex;

    if (context->running)
    {
        // Prepare more descriptors.

        fragIndex = context->fragmentRing->NextIndex;

        auto const descFull = (context->descBegin - 1u) & descMask;
        for (descIndex = context->descEnd; descIndex != descFull; descIndex = (descIndex + 1) & descMask)
        {
            if (fragIndex == fragEnd)
            {
                break;
            }

#if DWC_EQOS_NETCX20
            auto const buffer = RxPool20Pop(context->pool);
            if (buffer == nullptr)
            {
                break; // All in the stack: more come back before the next advance.
            }

            {
                auto const frag = NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex);
                frag->Capacity = context->rxBufferSize;
                frag->Offset = 0;
                frag->ValidLength = 0;
                NetExtensionGetFragmentVirtualAddress(&context->fragmentVirtual, fragIndex)->VirtualAddress = buffer->virtualAddress;
                NetExtensionGetFragmentReturnContext(&context->fragmentReturn, fragIndex)->Handle =
                    reinterpret_cast<NET_FRAGMENT_RETURN_CONTEXT_HANDLE>(buffer);
                context->fragBuffer[fragIndex] = buffer;
            }

            auto const fragLogicalAddress = buffer->physicalAddress;
#else
            NT_ASSERT(context->rxBufferSize <= NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex)->Capacity);
            auto const fragLogicalAddress = NetExtensionGetFragmentLogicalAddress(&context->fragmentLogical, fragIndex)->LogicalAddress;
#endif

            RxDescriptorRead descRead = {};
            descRead.Buf1ApLow = fragLogicalAddress & 0xFFFFFFFF;
            descRead.Buf1ApHigh = fragLogicalAddress >> 32;
            descRead.Buf1Valid = true;
            descRead.InterruptOnCompletion = true;
            descRead.Own = true;
#if DBG
            descRead.FragmentIndex = fragIndex;
#endif

            context->descVirtual[descIndex].Read = descRead;

            fragIndex = NetRingIncrementIndex(context->fragmentRing, fragIndex);
            queuedFrags += 1;
        }

        // In some error cases, the device may stall until we write to the tail pointer
        // again. Write to the tail pointer if there are pending descriptors, even if we
        // didn't fill any new ones.
        if (descIndex != descNext)
        {
            SetDescEnd(context, descIndex);
            context->fragmentRing->NextIndex = fragIndex;
        }
    }
    else if (descIndex == descNext)
    {
        while (pktIndex != pktEnd)
        {
            auto const pkt = NetRingGetPacketAtIndex(context->packetRing, pktIndex);
            pkt->Ignore = true;
            pktIndex = NetRingIncrementIndex(context->packetRing, pktIndex);
        }

#if DWC_EQOS_NETCX20
        RxQueueReclaim20(context, fragIndex, context->fragmentRing->NextIndex);
#endif
        context->packetRing->BeginIndex = pktEnd;
        context->fragmentRing->BeginIndex = fragEnd;
    }

    DeviceAddStatisticsRxQueue(context->deviceContext, ownDescriptors, doneFrags);

    TraceEntryExit(RxQueueAdvance, LEVEL_VERBOSE,
        TraceLoggingUInt32(ownDescriptors),
        TraceLoggingUInt32(doneFrags),
        TraceLoggingUInt32(queuedFrags));
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED RxQueueSetNotificationEnabled;
static void
RxQueueSetNotificationEnabled(
    _In_ NETPACKETQUEUE queue,
    _In_ BOOLEAN notificationEnabled)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    auto const context = RxQueueGetContext(queue);
    DeviceSetNotificationRxQueue(context->deviceContext, notificationEnabled ? queue : nullptr);
    TraceEntryExit(RxQueueSetNotificationEnabled, LEVEL_VERBOSE,
        TraceLoggingBoolean(notificationEnabled, "enabled"));
}

static EVT_PACKET_QUEUE_CANCEL RxQueueCancel;
static void
RxQueueCancel(_In_ NETPACKETQUEUE queue)
{
    // DISPATCH_LEVEL (verifier says so)
    auto const context = RxQueueGetContext(queue);

    // Shut down and flush. RxQueueAdvance should return the remaining packets.

    context->running = false;

    ChannelRxControl_t rxControl = {};
    rxControl.Start = false;
    rxControl.RxPacketFlush = true;
    Write32(&context->channelRegs->Rx_Control, rxControl);

    TraceEntryExit(RxQueueCancel, LEVEL_INFO);
}

static EVT_PACKET_QUEUE_STOP RxQueueStop;
__declspec(code_seg("PAGE"))
static void
RxQueueStop(_In_ NETPACKETQUEUE queue)
{
    // PASSIVE_LEVEL
    PAGED_CODE();
    auto const context = RxQueueGetContext(queue);

    DeviceSetNotificationRxQueue(context->deviceContext, nullptr);

    TraceEntryExit(RxQueueStop, LEVEL_INFO);
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP RxQueueCleanup;
static void
RxQueueCleanup(_In_ WDFOBJECT queue)
{
    // DISPATCH_LEVEL
    auto context = RxQueueGetContext(queue);
    if (context->descBuffer)
    {
        WdfObjectDelete(context->descBuffer);
        context->descBuffer = nullptr;
    }

#if DWC_EQOS_NETCX20
    if (context->fragBuffer)
    {
        ExFreePoolWithTag(context->fragBuffer, 'fRwD');
        context->fragBuffer = nullptr;
    }
#endif

    TraceEntryExit(RxQueueCleanup, LEVEL_VERBOSE);
}

_Use_decl_annotations_ NTSTATUS
RxQueueCreate(
    DeviceContext* deviceContext,
    DeviceConfig const& deviceConfig,
    NETRXQUEUE_INIT* queueInit,
    WDFDMAENABLER dma,
    ChannelRegisters* channelRegs)
{
    // PASSIVE_LEVEL, nonpaged (resume path)
    NTSTATUS status;
    NETPACKETQUEUE queue = nullptr;

    // Create queue.

    {
        NET_PACKET_QUEUE_CONFIG config;
        NET_PACKET_QUEUE_CONFIG_INIT(&config, RxQueueAdvance, RxQueueSetNotificationEnabled, RxQueueCancel);
        config.EvtStart = RxQueueStart;
        config.EvtStop = RxQueueStop;

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RxQueueContext);
        attributes.EvtCleanupCallback = RxQueueCleanup;

        status = NetRxQueueCreate(queueInit, &attributes, &config, &queue);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("NetRxQueueCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }
    }

    // Configure queue.

    {
        auto const rings = NetRxQueueGetRingCollection(queue);

        auto const context = RxQueueGetContext(queue);
        context->channelRegs = channelRegs;

        context->deviceContext = deviceContext;
        context->packetRing = NetRingCollectionGetPacketRing(rings);
        context->fragmentRing = NetRingCollectionGetFragmentRing(rings);
        context->descCount = QueueDescriptorCount(context->fragmentRing->NumberOfElements);
        context->rxBufferSize = deviceConfig.RxBufferSize();
        context->rxPbl = deviceConfig.rxPbl;

        TraceWrite("RxQueueCreate-size", LEVEL_VERBOSE,
            TraceLoggingHexInt32(context->packetRing->NumberOfElements, "packets"),
            TraceLoggingHexInt32(context->fragmentRing->NumberOfElements, "fragments"),
            TraceLoggingHexInt32(context->descCount, "descriptors"));

        WDF_COMMON_BUFFER_CONFIG bufferConfig;
        WDF_COMMON_BUFFER_CONFIG_INIT(&bufferConfig, QueueDescriptorAlignment - 1);
        status = WdfCommonBufferCreateWithConfig(
            dma,
            sizeof(RxDescriptor) * context->descCount,
            &bufferConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &context->descBuffer);
        if (!NT_SUCCESS(status))
        {
            TraceWrite("WdfCommonBufferCreate-failed", LEVEL_ERROR,
                TraceLoggingNTStatus(status));
            goto Done;
        }

        context->descVirtual = static_cast<RxDescriptor*>(
            WdfCommonBufferGetAlignedVirtualAddress(context->descBuffer));
        // A common buffer: uncached, Device memory under Windows 10, where the
        // DC ZVA of memset faults. Aligned 8-byte stores (rk_devmem.h).
        RkDevZero(context->descVirtual, sizeof(RxDescriptor) * context->descCount);
        context->descPhysical =
            WdfCommonBufferGetAlignedLogicalAddress(context->descBuffer);
        TraceWrite("RxQueueCreate-desc", LEVEL_VERBOSE,
            TraceLoggingHexInt64(context->descPhysical.QuadPart, "physical"),
            TraceLoggingPointer(context->descVirtual, "virtual"));

        NET_EXTENSION_QUERY query;

#if !DWC_EQOS_NETCX20
        // NetAdapterCx 2.0 knows no 802.1Q or GSO extension: asking for one
        // is BUGCODE_NETADAPTER_DRIVER (0x19E) with the name as parameter 2,
        // not a disabled NET_EXTENSION as 2.1 returns. The context is zeroed,
        // so they stay disabled there.
        NET_EXTENSION_QUERY_INIT(&query,
            NET_PACKET_EXTENSION_IEEE8021Q_NAME,
            NET_PACKET_EXTENSION_IEEE8021Q_VERSION_1,
            NetExtensionTypePacket);
        NetRxQueueGetExtension(queue, &query, &context->packetIeee8021Q);
#endif

        NET_EXTENSION_QUERY_INIT(&query,
            NET_PACKET_EXTENSION_CHECKSUM_NAME,
            NET_PACKET_EXTENSION_CHECKSUM_VERSION_1,
            NetExtensionTypePacket);
        NetRxQueueGetExtension(queue, &query, &context->packetChecksum);

#if DWC_EQOS_NETCX20
        // Driver-managed buffers: no logical address extension, the driver
        // gives the virtual address and the return context.
        NET_EXTENSION_QUERY_INIT(&query,
            NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
            NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
            NetExtensionTypeFragment);
        NetRxQueueGetExtension(queue, &query, &context->fragmentVirtual);

        NET_EXTENSION_QUERY_INIT(&query,
            NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_NAME,
            NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_VERSION_1,
            NetExtensionTypeFragment);
        NetRxQueueGetExtension(queue, &query, &context->fragmentReturn);

        if (!context->fragmentVirtual.Enabled || !context->fragmentReturn.Enabled)
        {
            TraceWrite("RxQueueCreate-no-driver-buffer-extensions", LEVEL_ERROR);
            status = STATUS_NOT_SUPPORTED;
            goto Done;
        }

        // The MAC gets the pool's physical addresses: only right while DMA
        // logical addresses are physical (no SMMU translation for it).
        if (MmGetPhysicalAddress(context->descVirtual).QuadPart != context->descPhysical.QuadPart)
        {
            TraceWrite("RxQueueCreate-logical-not-physical", LEVEL_ERROR,
                TraceLoggingHexInt64(context->descPhysical.QuadPart, "logical"),
                TraceLoggingHexInt64(MmGetPhysicalAddress(context->descVirtual).QuadPart, "physical"));
            DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
                "dwc_eqos: DMA logical address is not physical, no receive\n");
            status = STATUS_NOT_SUPPORTED;
            goto Done;
        }

        {
            auto const fragCount = context->fragmentRing->NumberOfElements;
            context->fragBuffer = static_cast<RxBuffer20**>(ExAllocatePool2(
                POOL_FLAG_NON_PAGED, sizeof(RxBuffer20*) * fragCount, 'fRwD'));
            if (context->fragBuffer == nullptr)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
                goto Done;
            }

            // Every fragment can hold one, and as many again can be in the stack.
            context->pool = DeviceGetRxPool20(deviceContext);
            status = RxPool20Reset(context->pool, fragCount * 2, context->rxBufferSize,
                DeviceGetMaxPhysicalAddress(deviceContext));
            if (!NT_SUCCESS(status))
            {
                TraceWrite("RxQueueCreate-pool-failed", LEVEL_ERROR,
                    TraceLoggingNTStatus(status));
                goto Done;
            }

            DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL,
                "dwc_eqos: %u driver receive buffers of %u bytes at %p\n",
                context->pool->count, context->rxBufferSize, context->pool->memory);
        }
#else
        NET_EXTENSION_QUERY_INIT(&query,
            NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
            NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
            NetExtensionTypeFragment);
        NetRxQueueGetExtension(queue, &query, &context->fragmentLogical);
#endif
    }

    status = STATUS_SUCCESS;

Done:

    TraceEntryExitWithStatus(RxQueueCreate, LEVEL_INFO, status);
    return status;
}
