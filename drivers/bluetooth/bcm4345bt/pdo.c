/*
 * pdo.c - the child device the Windows Bluetooth stack sits on
 *
 * Once the controller is up, the driver enumerates one child with the
 * compatible ID MS_BTHX_BTHMINI. The in-box bth.inf binds BthMini.sys to
 * that ID (checked in the 22621 ARM64 image: bth.inf, [BthMini] section),
 * and BthMini talks the BthX DDI. Its IOCTLs arrive on the child and are
 * forwarded to this driver's own queue, where hci.c answers them.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcm4345bt.h"

/* GUID_CONTAINERID_INTERNALLY_CONNECTED_DEVICE: part of the computer. */
#define BT_CONTAINER_ID L"{00000000-0000-0000-ffff-ffffffffffff}"

static VOID
PdoEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                      _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    WDFDEVICE pdo = WdfIoQueueGetDevice(Queue);
    WDFDEVICE parent = WdfPdoGetParent(pdo);
    WDF_REQUEST_FORWARD_OPTIONS fwd;
    NTSTATUS status;

    switch (IoControlCode) {
    case IOCTL_BTHX_GET_VERSION:
    case IOCTL_BTHX_SET_VERSION:
    case IOCTL_BTHX_QUERY_CAPABILITIES:
    case IOCTL_BTHX_WRITE_HCI:
    case IOCTL_BTHX_READ_HCI:
        WDF_REQUEST_FORWARD_OPTIONS_INIT(&fwd);
        fwd.Flags = WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET;
        status = WdfRequestForwardToParentDeviceIoQueue(Request, WdfDeviceGetDefaultQueue(parent), &fwd);
        if (NT_SUCCESS(status)) {
            return;
        }
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    {
        PBT_DEVICE_CONTEXT ctx = BtGetContext(parent);
        InterlockedExchange(&ctx->BxLastUnknown, (LONG)IoControlCode);
        BtNote(ctx, BxOther, status, InputBufferLength, OutputBufferLength);
    }
    WdfRequestComplete(Request, status);
}

/* Nothing is expected here; recorded so a board run shows it if it comes. */
static VOID
PdoEvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                              _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(WdfPdoGetParent(WdfIoQueueGetDevice(Queue)));

    InterlockedExchange(&ctx->BxLastUnknown, (LONG)(IoControlCode | 0x80000000UL));
    BtNote(ctx, BxOther, STATUS_NOT_SUPPORTED, InputBufferLength, OutputBufferLength);
    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

NTSTATUS
BtCreateChild(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    PWDFDEVICE_INIT init;
    WDFDEVICE child = NULL;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    DECLARE_CONST_UNICODE_STRING(deviceId, BT_PDO_DEVICE_ID);
    DECLARE_CONST_UNICODE_STRING(compatId, BT_PDO_COMPATIBLE_ID);
    DECLARE_CONST_UNICODE_STRING(instanceId, L"0");
    DECLARE_CONST_UNICODE_STRING(containerId, BT_CONTAINER_ID);
    DECLARE_CONST_UNICODE_STRING(desc, L"BCM4345C5 Bluetooth (UART H4)");
    NTSTATUS status;

    if (Ctx->ChildCreated) {
        return STATUS_SUCCESS;
    }

    init = WdfPdoInitAllocate(Ctx->Device);
    if (init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BUS_EXTENDER);
    status = WdfPdoInitAssignDeviceID(init, &deviceId);
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAddHardwareID(init, &deviceId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAddCompatibleID(init, &compatId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAssignInstanceID(init, &instanceId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAssignContainerID(init, &containerId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAddDeviceText(init, &desc, &desc, 0x409);
    }
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }
    WdfPdoInitSetDefaultLocale(init, 0x409);
    WdfPdoInitAllowForwardingRequestToParent(init);

    status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &child);
    if (!NT_SUCCESS(status)) {
        if (init != NULL) {
            WdfDeviceInitFree(init);
        }
        return status;
    }

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.Removable = WdfFalse;
    pnpCaps.SurpriseRemovalOK = WdfFalse;
    pnpCaps.UniqueID = WdfTrue;
    pnpCaps.Address = 0;
    pnpCaps.UINumber = 0;
    WdfDeviceSetPnpCapabilities(child, &pnpCaps);

    /*
     * Not power managed: BthMini asks for the version and capabilities
     * before the child is in D0.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.PowerManaged = WdfFalse;
    qcfg.EvtIoDeviceControl = PdoEvtIoDeviceControl;
    qcfg.EvtIoInternalDeviceControl = PdoEvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(child, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(child);
        return status;
    }

    /*
     * Added after start, so between lock and unlock: the unlock is what
     * tells the PnP manager the relations changed.
     */
    WdfFdoLockStaticChildListForIteration(Ctx->Device);
    status = WdfFdoAddStaticChild(Ctx->Device, child);
    WdfFdoUnlockStaticChildListFromIteration(Ctx->Device);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(child);
        return status;
    }
    Ctx->ChildCreated = TRUE;
    BtRecordStatus(Ctx, L"ChildCreated", 1);
    BT_LOG("child %wZ enumerated for BthMini", &deviceId);
    return STATUS_SUCCESS;
}
