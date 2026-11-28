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
    UINT32 cacheLineSize;
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

// Windows 10 maps the uncached common buffers NetAdapterCx gives as receive
// buffers as Device memory, where tcpip's misaligned loads of the frame
// fault (bugcheck 0x1E STATUS_DATATYPE_MISALIGNMENT in tcpip, measured on
// 19045 once tcpip bound). The Windows 10 build asks for cached receive
// buffers instead (device.cpp, CacheEnabled) and keeps them coherent with
// the non-coherent MAC itself, as Linux's dma_map/unmap do: clean and
// invalidate a buffer before the MAC owns it, invalidate what it wrote
// before the stack reads it. NetAdapterCx does neither for receive buffers
// on ARM64. The buffers are cache line aligned and a whole number of lines
// (device.cpp), so no line is shared with another buffer.

static UINT32
RxCacheLineSize()
{
    // CTR_EL0.DminLine: log2 of the smallest data cache line, in words.
    auto const ctr = _ReadStatusReg(ARM64_SYSREG(3, 3, 0, 0, 1));
    return 4u << ((ctr >> 16) & 0xF);
}

// DC CIVAC over [va, va + length). The caller issues the DSB.
static void
RxCacheCleanInvalidate(_In_ RxQueueContext const* context, _In_ void const* va, _In_ SIZE_T length)
{
    auto const line = context->cacheLineSize;
    auto p = reinterpret_cast<UINT_PTR>(va) & ~static_cast<UINT_PTR>(line - 1);
    auto const end = reinterpret_cast<UINT_PTR>(va) + length;
    for (; p < end; p += line)
    {
        __sys(ARM64_SYSINSTR(1, 3, 7, 14, 1), static_cast<__int64>(p)); // DC CIVAC
    }
}

static void*
RxFragmentVirtualAddress(_In_ RxQueueContext const* context, UINT32 fragIndex)
{
    auto const frag = NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex);
    return static_cast<UINT8*>(NetExtensionGetFragmentVirtualAddress(&context->fragmentVirtual, fragIndex)->VirtualAddress)
        + frag->Offset;
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
        }
        else
        {
            NT_ASSERT(descWrite.PacketLength >= 4); // PacketLength includes CRC
            frag->ValidLength = descWrite.PacketLength - 4;
#if DWC_EQOS_NETCX20
            RxCacheCleanInvalidate(context, RxFragmentVirtualAddress(context, fragIndex), frag->ValidLength);
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

#if DWC_EQOS_NETCX20
    if (doneFrags != 0)
    {
        __dsb(_ARM64_BARRIER_SY); // Invalidations done before the stack reads.
    }
#endif

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

            NT_ASSERT(context->rxBufferSize <= NetRingGetFragmentAtIndex(context->fragmentRing, fragIndex)->Capacity);
            auto const fragLogicalAddress = NetExtensionGetFragmentLogicalAddress(&context->fragmentLogical, fragIndex)->LogicalAddress;
#if DWC_EQOS_NETCX20
            RxCacheCleanInvalidate(context, RxFragmentVirtualAddress(context, fragIndex), context->rxBufferSize);
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
#if DWC_EQOS_NETCX20
        if (queuedFrags != 0)
        {
            __dsb(_ARM64_BARRIER_SY); // Buffers clean before the MAC may write them.
        }
#endif

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

        NET_EXTENSION_QUERY_INIT(&query,
            NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
            NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
            NetExtensionTypeFragment);
        NetRxQueueGetExtension(queue, &query, &context->fragmentLogical);

#if DWC_EQOS_NETCX20
        NET_EXTENSION_QUERY_INIT(&query,
            NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
            NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
            NetExtensionTypeFragment);
        NetRxQueueGetExtension(queue, &query, &context->fragmentVirtual);
        context->cacheLineSize = RxCacheLineSize();
#endif
    }

    status = STATUS_SUCCESS;

Done:

    TraceEntryExitWithStatus(RxQueueCreate, LEVEL_INFO, status);
    return status;
}
