/*
 * driver.c - read-only access to an I2C EEPROM (24C16 and the like)
 *
 * A KMDF function driver for ACPI\RKCP2416. Each I2cSerialBus in _CRS is
 * one 256-byte block of the part, in order: a 24C16 (BL24C16F on the ROCK
 * 4D) answers on 0x50..0x57 and takes the block number from the low address
 * bits, with a one-byte word address inside the block. A read is the word
 * address written, then the data read, in one SPB sequence.
 *
 * Read-only: the only bytes ever written to the part are word addresses.
 * Nothing writes the array, and the write-protect pin is not touched.
 *
 * \\.\RKEEPROM0 (rkeeprom_ioctl.h) is for tools\eeprom; the INF restricts it
 * to SYSTEM and Administrators.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>

#include "rkeeprom_ioctl.h"

#define EE_MAX_BLOCKS       8
#define EE_BLOCK_BYTES      256
#define EE_CHUNK            32      /* bytes per SPB read */
#define EE_I2C_TIMEOUT_MS   100

#define EE_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "rkeeprom: " fmt "\n", __VA_ARGS__)

typedef struct _EE_CONTEXT {
    WDFDEVICE       Device;
    WDFWAITLOCK     Lock;
    ULONG           Blocks;
    LARGE_INTEGER   Connection[EE_MAX_BLOCKS];
    WDFIOTARGET     Target[EE_MAX_BLOCKS];
    ULONG           Reads;
    ULONG           Errors;
} EE_CONTEXT, *PEE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EE_CONTEXT, EeGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EeEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EeEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EeEvtReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EeEvtIoDeviceControl;

static
NTSTATUS
EeReadChunk(
    _In_ PEE_CONTEXT Ctx,
    _In_ ULONG Block,
    _In_ UCHAR Word,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length
    )
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    WDF_MEMORY_DESCRIPTOR in;
    WDF_REQUEST_SEND_OPTIONS opts;
    ULONG_PTR bytes = 0;
    NTSTATUS status;

    SPB_TRANSFER_LIST_INIT(&seq.List, 2);
    seq.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice, 0, &Word, 1);
    seq.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice, 0, Buffer, Length);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &seq, sizeof(seq));
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(EE_I2C_TIMEOUT_MS));

    status = WdfIoTargetSendIoctlSynchronously(Ctx->Target[Block], NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &in, NULL, &opts, &bytes);
    if (NT_SUCCESS(status) && bytes != 1 + Length) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return status;
}

static
NTSTATUS
EeRead(
    _In_ PEE_CONTEXT Ctx,
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    while (Length != 0) {
        ULONG block = Offset / EE_BLOCK_BYTES;
        ULONG word = Offset % EE_BLOCK_BYTES;
        ULONG n = min(min(Length, EE_CHUNK), EE_BLOCK_BYTES - word);

        if (block >= Ctx->Blocks || Ctx->Target[block] == NULL) {
            return STATUS_INVALID_DEVICE_STATE;
        }
        status = EeReadChunk(Ctx, block, (UCHAR)word, Buffer, n);
        if (!NT_SUCCESS(status)) {
            Ctx->Errors++;
            EE_LOG("read of %u bytes at 0x%03X failed 0x%08X", n, Offset, status);
            return status;
        }
        Ctx->Reads++;
        Offset += n;
        Buffer += n;
        Length -= n;
    }
    return status;
}

VOID
EeEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    PEE_CONTEXT ctx = EeGetContext(WdfIoQueueGetDevice(Queue));
    ULONG_PTR info = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_RKEEPROM_GET_INFO: {
        RKEEPROM_INFO *out;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(out, sizeof(*out));
            out->Size = sizeof(*out);
            out->Version = RKEEPROM_INTERFACE_VERSION;
            out->Blocks = ctx->Blocks;
            out->BlockBytes = EE_BLOCK_BYTES;
            out->Bytes = ctx->Blocks * EE_BLOCK_BYTES;
            out->Reads = ctx->Reads;
            out->Errors = ctx->Errors;
            info = sizeof(*out);
        }
        break;
    }

    case IOCTL_RKEEPROM_READ: {
        RKEEPROM_READ *in;
        RKEEPROM_READ req;
        PUCHAR out;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }
        /* METHOD_BUFFERED: in and out share one buffer, so copy first. */
        req = *in;
        if (req.Length == 0 || req.Length > RKEEPROM_MAX_READ ||
            req.Offset >= ctx->Blocks * EE_BLOCK_BYTES ||
            req.Length > ctx->Blocks * EE_BLOCK_BYTES - req.Offset) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, req.Length, (PVOID *)&out, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }
        WdfWaitLockAcquire(ctx->Lock, NULL);
        status = EeRead(ctx, req.Offset, out, req.Length);
        WdfWaitLockRelease(ctx->Lock);
        if (NT_SUCCESS(status)) {
            info = req.Length;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}

NTSTATUS
EeEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PEE_CONTEXT ctx = EeGetContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
    ULONG count, i;
    UCHAR probe;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    ctx->Blocks = 0;
    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count && ctx->Blocks < EE_MAX_BLOCKS; i++) {
        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc != NULL && desc->Type == CmResourceTypeConnection &&
            desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            ctx->Connection[ctx->Blocks].LowPart = desc->u.Connection.IdLowPart;
            ctx->Connection[ctx->Blocks].HighPart = desc->u.Connection.IdHighPart;
            ctx->Blocks++;
        }
    }
    if (ctx->Blocks == 0) {
        EE_LOG("no I2cSerialBus in _CRS%s", "");
        return STATUS_NOT_FOUND;
    }

    for (i = 0; i < ctx->Blocks; i++) {
        DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
        WDF_OBJECT_ATTRIBUTES attr;
        WDF_IO_TARGET_OPEN_PARAMS open;

        status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, ctx->Connection[i].LowPart,
                                                  ctx->Connection[i].HighPart);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Device;
        status = WdfIoTargetCreate(Device, &attr, &ctx->Target[i]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        /* SPB needs write access for the word address of a read. */
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &path, GENERIC_READ | GENERIC_WRITE);
        status = WdfIoTargetOpen(ctx->Target[i], &open);
        if (!NT_SUCCESS(status)) {
            EE_LOG("block %u: opening the I2C connection failed 0x%08X", i, status);
            WdfObjectDelete(ctx->Target[i]);
            ctx->Target[i] = NULL;
            return status;
        }
    }

    /* Is it there? One byte from the start of the first block. */
    status = EeReadChunk(ctx, 0, 0, &probe, 1);
    EE_LOG("%u blocks of %u bytes; first byte 0x%02X (0x%08X)",
           ctx->Blocks, EE_BLOCK_BYTES, NT_SUCCESS(status) ? probe : 0, status);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_DEVICE_NOT_CONNECTED;
}

NTSTATUS
EeEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PEE_CONTEXT ctx = EeGetContext(Device);
    ULONG i;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    for (i = 0; i < EE_MAX_BLOCKS; i++) {
        if (ctx->Target[i] != NULL) {
            WdfIoTargetClose(ctx->Target[i]);
            WdfObjectDelete(ctx->Target[i]);
            ctx->Target[i] = NULL;
        }
    }
    ctx->Blocks = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
EeEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFDEVICE device;
    PEE_CONTEXT ctx;
    UNICODE_STRING link;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EeEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EeEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, EE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        EE_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }
    ctx = EeGetContext(device);
    ctx->Device = device;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfWaitLockCreate(&attr, &ctx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoDeviceControl = EeEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&link, RKEEPROM_DOS_NAME);
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) {
        EE_LOG("WdfDeviceCreateSymbolicLink failed 0x%08X", status);
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, EeEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}
