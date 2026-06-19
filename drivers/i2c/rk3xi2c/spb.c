/*++

Module Name:

    spb.c

Abstract:

    SpbCx callbacks for the rk3x I2C controller. Each peripheral that opens the
    bus (via an I2cSerialBus connection descriptor in its ACPI _CRS) becomes an
    SPBTARGET; we parse its address/speed here and route Read/Write/Sequence
    requests to the rk3x transfer engine in hw.c.

    SpbCx dispatches I/O sequentially at PASSIVE_LEVEL, so the polled engine runs
    directly in these callbacks without extra locking.

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

#include "rk3xi2c.h"

//
// Maximum transfers we accept in a single SPB sequence. The common case is 2
// (write register offset, then read); 8 is generous headroom.
//
#define RK3XI2C_MAX_SEQUENCE    8

_Use_decl_annotations_
NTSTATUS
Rk3xI2cEvtTargetConnect(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    SPB_CONNECTION_PARAMETERS params;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER connection;
    PPNP_SERIAL_BUS_DESCRIPTOR descriptor;
    PPNP_I2C_SERIAL_BUS_DESCRIPTOR i2cDescriptor;
    PRK3XI2C_TARGET target;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Controller);

    SPB_CONNECTION_PARAMETERS_INIT(&params);
    SpbTargetGetConnectionParameters(SpbTarget, &params);

    connection =
        (PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER)params.ConnectionParameters;
    if (connection->PropertiesLength < sizeof(PNP_SERIAL_BUS_DESCRIPTOR)) {
        RkLog(RK_DBG_ERROR, "connection descriptor too small\n");
        return STATUS_INVALID_PARAMETER;
    }

    descriptor = (PPNP_SERIAL_BUS_DESCRIPTOR)connection->ConnectionProperties;
    if (descriptor->SerialBusType != I2C_SERIAL_BUS_TYPE) {
        RkLog(RK_DBG_ERROR, "not an I2C target (type %u)\n", descriptor->SerialBusType);
        return STATUS_NOT_SUPPORTED;
    }

    i2cDescriptor = (PPNP_I2C_SERIAL_BUS_DESCRIPTOR)connection->ConnectionProperties;

    //
    // 10-bit addressing is not implemented in the v1 engine (it masks to 7 bit).
    //
    if (descriptor->TypeSpecificFlags & 0x0001) {
        RkLog(RK_DBG_ERROR, "10-bit addressing not supported\n");
        return STATUS_NOT_SUPPORTED;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RK3XI2C_TARGET);
    status = WdfObjectAllocateContext(SpbTarget, &attributes, (PVOID *)&target);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfObjectAllocateContext failed 0x%08x\n", status);
        return status;
    }

    target->Address = i2cDescriptor->SlaveAddress;
    target->TenBitAddress = FALSE;
    target->ConnectionSpeed = i2cDescriptor->ConnectionSpeed;
    if (target->ConnectionSpeed == 0) {
        target->ConnectionSpeed = RK3XI2C_DEFAULT_BUS_HZ;
    }
    target->ClkDiv = Rk3xI2cComputeClkDiv(RK3XI2C_INPUT_CLK_HZ,
                                          target->ConnectionSpeed);

    RkLog(RK_DBG_INFO, "TargetConnect addr 0x%02x speed %u clkdiv 0x%08x\n",
          target->Address, target->ConnectionSpeed, target->ClkDiv);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
Rk3xI2cEvtControllerLock(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget
    )
{
    //
    // Sequential dispatch already serializes requests and every request issues
    // its own STOP, so bus locking is a no-op in v1.
    //
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(SpbTarget);
}

_Use_decl_annotations_
VOID
Rk3xI2cEvtControllerUnlock(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget
    )
{
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(SpbTarget);
}

_Use_decl_annotations_
VOID
Rk3xI2cEvtIoRead(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t Length
    )
{
    PRK3XI2C_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XI2C_TARGET target = GetTargetContext(SpbTarget);
    SPB_TRANSFER_DESCRIPTOR descriptor;
    PMDL mdl = NULL;
    PVOID buffer;
    RK3X_I2C_MSG msg;
    ULONG bytes = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Length);

    SPB_TRANSFER_DESCRIPTOR_INIT(&descriptor);
    SpbRequestGetTransferParameters(SpbRequest, 0, &descriptor, &mdl);

    buffer = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (buffer == NULL) {
        SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    msg.Address = target->Address;
    msg.Read = TRUE;
    msg.Buffer = (PUCHAR)buffer;
    msg.Length = (ULONG)descriptor.TransferLength;

    status = Rk3xI2cTransfer(ctx, target->ClkDiv, &msg, 1, &bytes);

    WdfRequestSetInformation(SpbRequest, bytes);
    SpbRequestComplete(SpbRequest, status);
}

_Use_decl_annotations_
VOID
Rk3xI2cEvtIoWrite(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t Length
    )
{
    PRK3XI2C_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XI2C_TARGET target = GetTargetContext(SpbTarget);
    SPB_TRANSFER_DESCRIPTOR descriptor;
    PMDL mdl = NULL;
    PVOID buffer;
    RK3X_I2C_MSG msg;
    ULONG bytes = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Length);

    SPB_TRANSFER_DESCRIPTOR_INIT(&descriptor);
    SpbRequestGetTransferParameters(SpbRequest, 0, &descriptor, &mdl);

    buffer = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (buffer == NULL) {
        SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    msg.Address = target->Address;
    msg.Read = FALSE;
    msg.Buffer = (PUCHAR)buffer;
    msg.Length = (ULONG)descriptor.TransferLength;

    status = Rk3xI2cTransfer(ctx, target->ClkDiv, &msg, 1, &bytes);

    WdfRequestSetInformation(SpbRequest, bytes);
    SpbRequestComplete(SpbRequest, status);
}

_Use_decl_annotations_
VOID
Rk3xI2cEvtIoSequence(
    WDFDEVICE Controller,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    ULONG TransferCount
    )
{
    PRK3XI2C_CONTEXT ctx = GetControllerContext(Controller);
    PRK3XI2C_TARGET target = GetTargetContext(SpbTarget);
    RK3X_I2C_MSG msgs[RK3XI2C_MAX_SEQUENCE];
    ULONG bytes = 0;
    ULONG i;
    NTSTATUS status;

    if (TransferCount == 0 || TransferCount > RK3XI2C_MAX_SEQUENCE) {
        SpbRequestComplete(SpbRequest, STATUS_INVALID_PARAMETER);
        return;
    }

    for (i = 0; i < TransferCount; i += 1) {
        SPB_TRANSFER_DESCRIPTOR descriptor;
        PMDL mdl = NULL;
        PVOID buffer;

        SPB_TRANSFER_DESCRIPTOR_INIT(&descriptor);
        SpbRequestGetTransferParameters(SpbRequest, i, &descriptor, &mdl);

        buffer = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
        if (buffer == NULL) {
            SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
            return;
        }

        msgs[i].Address = target->Address;
        msgs[i].Read = (descriptor.Direction == SpbTransferDirectionFromDevice);
        msgs[i].Buffer = (PUCHAR)buffer;
        msgs[i].Length = (ULONG)descriptor.TransferLength;
    }

    //
    // The whole list runs as one bus transaction: START + (repeated START
    // between messages) + STOP at the end — i.e. the classic write-then-read.
    //
    status = Rk3xI2cTransfer(ctx, target->ClkDiv, msgs, TransferCount, &bytes);

    WdfRequestSetInformation(SpbRequest, bytes);
    SpbRequestComplete(SpbRequest, status);
}
