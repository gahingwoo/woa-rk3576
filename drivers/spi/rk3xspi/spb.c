/*++

Module Name:

    spb.c

Abstract:

    SpbCx callbacks for the rockchip SPI controller. Parses each SPI target's
    chip-select / speed / mode from its SpiSerialBus descriptor and routes
    Read / Write / Sequence / full-duplex requests to the engine in hw.c.

    A Sequence is coalesced into a single full-duplex transaction so chip-select
    stays asserted across all segments (the controller drops CS when disabled),
    which is what write-then-read SPI devices require.

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

#include "rk3xspi.h"

static
PVOID
Rk3xSpiMapTransfer(
    _In_ SPBREQUEST Request,
    _In_ ULONG Index,
    _Out_ PULONG Length,
    _Out_ PBOOLEAN Write
    )
{
    SPB_TRANSFER_DESCRIPTOR descriptor;
    PMDL mdl = NULL;

    SPB_TRANSFER_DESCRIPTOR_INIT(&descriptor);
    SpbRequestGetTransferParameters(Request, Index, &descriptor, &mdl);

    *Length = (ULONG)descriptor.TransferLength;
    *Write = (descriptor.Direction == SpbTransferDirectionToDevice);

    if (mdl == NULL) {
        return NULL;
    }
    return MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
}

_Use_decl_annotations_
NTSTATUS
Rk3xSpiEvtTargetConnect(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    SPB_CONNECTION_PARAMETERS params;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER connection;
    PPNP_SERIAL_BUS_DESCRIPTOR descriptor;
    PPNP_SPI_SERIAL_BUS_DESCRIPTOR spiDescriptor;
    PRK3XSPI_TARGET target;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Controller);

    SPB_CONNECTION_PARAMETERS_INIT(&params);
    SpbTargetGetConnectionParameters(SpbTarget, &params);

    connection =
        (PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER)params.ConnectionParameters;
    if (connection->PropertiesLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR)) {
        return STATUS_INVALID_PARAMETER;
    }

    descriptor = (PPNP_SERIAL_BUS_DESCRIPTOR)connection->ConnectionProperties;
    if (descriptor->SerialBusType != SPI_SERIAL_BUS_TYPE) {
        RkLog(RK_DBG_ERROR, "not an SPI target (type %u)\n", descriptor->SerialBusType);
        return STATUS_NOT_SUPPORTED;
    }

    spiDescriptor = (PPNP_SPI_SERIAL_BUS_DESCRIPTOR)connection->ConnectionProperties;

    if (spiDescriptor->DataBitLength != 8) {
        RkLog(RK_DBG_ERROR, "only 8-bit frames supported (got %u)\n",
              spiDescriptor->DataBitLength);
        return STATUS_NOT_SUPPORTED;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RK3XSPI_TARGET);
    status = WdfObjectAllocateContext(SpbTarget, &attributes, (PVOID *)&target);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    target->ChipSelect = spiDescriptor->DeviceSelection;
    target->ConnectionSpeed = spiDescriptor->ConnectionSpeed;
    if (target->ConnectionSpeed == 0) {
        target->ConnectionSpeed = RK3XSPI_DEFAULT_HZ;
    }
    //
    // ACPI SpiSerialBus: Phase 0 = first edge (CPHA 0), 1 = second edge;
    // Polarity 0 = idle low (CPOL 0), 1 = idle high.
    //
    target->CphaSecond = (spiDescriptor->Phase != 0);
    target->CpolHigh = (spiDescriptor->Polarity != 0);
    target->Baudr = Rk3xSpiComputeBaudr(RK3XSPI_INPUT_CLK_HZ, target->ConnectionSpeed);

    RkLog(RK_DBG_INFO, "TargetConnect cs %u speed %u baudr %u mode %u%u\n",
          target->ChipSelect, target->ConnectionSpeed, target->Baudr,
          target->CpolHigh, target->CphaSecond);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
Rk3xSpiEvtControllerLock(WDFDEVICE Controller, SPBTARGET SpbTarget)
{
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(SpbTarget);
}

_Use_decl_annotations_
VOID
Rk3xSpiEvtControllerUnlock(WDFDEVICE Controller, SPBTARGET SpbTarget)
{
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(SpbTarget);
}

_Use_decl_annotations_
VOID
Rk3xSpiEvtIoRead(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t Length
    )
{
    PRK3XSPI_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XSPI_TARGET target = GetTargetContext(SpbTarget);
    PVOID buffer;
    ULONG len;
    BOOLEAN write;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Length);

    buffer = Rk3xSpiMapTransfer(SpbRequest, 0, &len, &write);
    if (buffer == NULL) {
        SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    status = Rk3xSpiTransfer(ctx, target, NULL, (PUCHAR)buffer, len);

    WdfRequestSetInformation(SpbRequest, NT_SUCCESS(status) ? len : 0);
    SpbRequestComplete(SpbRequest, status);
}

_Use_decl_annotations_
VOID
Rk3xSpiEvtIoWrite(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t Length
    )
{
    PRK3XSPI_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XSPI_TARGET target = GetTargetContext(SpbTarget);
    PVOID buffer;
    ULONG len;
    BOOLEAN write;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Length);

    buffer = Rk3xSpiMapTransfer(SpbRequest, 0, &len, &write);
    if (buffer == NULL) {
        SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    status = Rk3xSpiTransfer(ctx, target, (PUCHAR)buffer, NULL, len);

    WdfRequestSetInformation(SpbRequest, NT_SUCCESS(status) ? len : 0);
    SpbRequestComplete(SpbRequest, status);
}

_Use_decl_annotations_
VOID
Rk3xSpiEvtIoSequence(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    ULONG TransferCount
    )
{
    PRK3XSPI_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XSPI_TARGET target = GetTargetContext(SpbTarget);
    PUCHAR txBuf = NULL;
    PUCHAR rxBuf = NULL;
    ULONG total = 0;
    ULONG offset;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    //
    // Sum the segment lengths, then coalesce into one CS-held full-duplex
    // transaction: write segments contribute their bytes to TX; read segments
    // clock zeros and capture RX. RX is scattered back afterwards.
    //
    for (i = 0; i < TransferCount; i += 1) {
        ULONG len;
        BOOLEAN write;
        (VOID)Rk3xSpiMapTransfer(SpbRequest, i, &len, &write);
        total += len;
    }

    if (total == 0) {
        SpbRequestComplete(SpbRequest, STATUS_INVALID_PARAMETER);
        return;
    }

    txBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, RK3XSPI_POOL_TAG);
    rxBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, RK3XSPI_POOL_TAG);
    if (txBuf == NULL || rxBuf == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    offset = 0;
    for (i = 0; i < TransferCount; i += 1) {
        ULONG len;
        BOOLEAN write;
        PVOID seg = Rk3xSpiMapTransfer(SpbRequest, i, &len, &write);
        if (seg == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        if (write) {
            RtlCopyMemory(txBuf + offset, seg, len);
        }
        offset += len;
    }

    status = Rk3xSpiTransfer(ctx, target, txBuf, rxBuf, total);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    //
    // Scatter received bytes back into the read segments.
    //
    offset = 0;
    for (i = 0; i < TransferCount; i += 1) {
        ULONG len;
        BOOLEAN write;
        PVOID seg = Rk3xSpiMapTransfer(SpbRequest, i, &len, &write);
        if (seg != NULL && !write) {
            RtlCopyMemory(seg, rxBuf + offset, len);
        }
        offset += len;
    }

Cleanup:
    if (txBuf != NULL) {
        ExFreePoolWithTag(txBuf, RK3XSPI_POOL_TAG);
    }
    if (rxBuf != NULL) {
        ExFreePoolWithTag(rxBuf, RK3XSPI_POOL_TAG);
    }

    WdfRequestSetInformation(SpbRequest, NT_SUCCESS(status) ? total : 0);
    SpbRequestComplete(SpbRequest, status);
}

_Use_decl_annotations_
VOID
Rk3xSpiEvtIoOther(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    PRK3XSPI_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XSPI_TARGET target = GetTargetContext(SpbTarget);
    PVOID txBuf;
    PVOID rxBuf;
    ULONG txLen;
    ULONG rxLen;
    BOOLEAN write;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    //
    // SPI full-duplex: two equal-length transfers (write = index 0, read =
    // index 1) clocked simultaneously.
    //
    if (IoControlCode != IOCTL_SPB_FULL_DUPLEX) {
        SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
        return;
    }

    txBuf = Rk3xSpiMapTransfer(SpbRequest, 0, &txLen, &write);
    rxBuf = Rk3xSpiMapTransfer(SpbRequest, 1, &rxLen, &write);
    if (txBuf == NULL || rxBuf == NULL || txLen != rxLen) {
        SpbRequestComplete(SpbRequest, STATUS_INVALID_PARAMETER);
        return;
    }

    status = Rk3xSpiTransfer(ctx, target, (PUCHAR)txBuf, (PUCHAR)rxBuf, txLen);

    WdfRequestSetInformation(SpbRequest, NT_SUCCESS(status) ? txLen : 0);
    SpbRequestComplete(SpbRequest, status);
}
