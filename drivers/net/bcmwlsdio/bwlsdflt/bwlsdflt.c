/*
 * bwlsdflt.c - a WDM lower filter between bcmwlsdio's FDO and the SDIO
 * function PDO that sdport.sys creates. It passes everything down except
 * one request: IRP_MN_QUERY_INTERFACE for GUID_REENUMERATE_SELF_INTERFACE_STANDARD,
 * which it completes untouched (STATUS_NOT_SUPPORTED, as the IRP arrives).
 *
 * Why: KMDF queries that interface from the device below every FDO when
 * the device starts (FxPkgFdo::QueryForReenumerationInterface) and calls
 * its InterfaceDereference when the device is removed
 * (FxPkgFdo::ReleaseReenumerationInterface). sdport 22621
 * (SdhostPdoGetReenumerationInterface) hands out a 0x18-byte pool block as
 * the interface context, and its SdhostReenumerationInterfaceDereference
 * frees Context - 0x18 instead of Context. That address is never 16-byte
 * aligned, so ExFreePoolWithTag stops the system: BAD_POOL_CALLER 0xC2,
 * P1 0x99, P2 = Context - 0x18, every time a KMDF function driver's device
 * on an SDIO function goes away (disable, driver update, uninstall).
 * Without the interface KMDF has nothing to release; it uses it only for
 * WdfDeviceSetFailed(WdfDeviceFailedAttemptRestart).
 *
 * The filter must not be KMDF itself: a KMDF filter is an FxPkgFdo too
 * and would make the same query.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <ntddk.h>
#include <initguid.h>
#include <wdmguid.h>

#define FLT_TAG 'tFwB'

#define FLT_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "bwlsdflt: " fmt "\n", __VA_ARGS__)

typedef struct _FLT_EXT {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Lower;
    IO_REMOVE_LOCK RemoveLock;
    volatile LONG Refused;
} FLT_EXT, *PFLT_EXT;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE FltAddDevice;
static DRIVER_UNLOAD FltUnload;
_Dispatch_type_(IRP_MJ_PNP) static DRIVER_DISPATCH FltPnp;
static DRIVER_DISPATCH FltPass;

static NTSTATUS
FltPass(_In_ PDEVICE_OBJECT Device, _Inout_ PIRP Irp)
{
    PFLT_EXT x = (PFLT_EXT)Device->DeviceExtension;
    NTSTATUS status;

    status = IoAcquireRemoveLock(&x->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(x->Lower, Irp);
    IoReleaseRemoveLock(&x->RemoveLock, Irp);
    return status;
}

static NTSTATUS
FltPnp(_In_ PDEVICE_OBJECT Device, _Inout_ PIRP Irp)
{
    PFLT_EXT x = (PFLT_EXT)Device->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    status = IoAcquireRemoveLock(&x->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (sp->MinorFunction == IRP_MN_QUERY_INTERFACE &&
        sp->Parameters.QueryInterface.InterfaceType != NULL &&
        IsEqualGUID(sp->Parameters.QueryInterface.InterfaceType, &GUID_REENUMERATE_SELF_INTERFACE_STANDARD)) {
        /* Not supported, as far as anyone above is concerned: do not touch the IRP's status. */
        status = Irp->IoStatus.Status;
        FLT_LOG("refused GUID_REENUMERATE_SELF_INTERFACE_STANDARD (%ld so far), status 0x%08X",
                InterlockedIncrement(&x->Refused), status);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&x->RemoveLock, Irp);
        return status;
    }

    if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(x->Lower, Irp);
        IoReleaseRemoveLockAndWait(&x->RemoveLock, Irp);
        IoDetachDevice(x->Lower);
        IoDeleteDevice(Device);
        return status;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(x->Lower, Irp);
    IoReleaseRemoveLock(&x->RemoveLock, Irp);
    return status;
}

static NTSTATUS
FltAddDevice(_In_ PDRIVER_OBJECT Driver, _In_ PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT dev;
    PFLT_EXT x;
    NTSTATUS status;

    status = IoCreateDevice(Driver, sizeof(FLT_EXT), NULL, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    x = (PFLT_EXT)dev->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Self = dev;
    IoInitializeRemoveLock(&x->RemoveLock, FLT_TAG, 0, 0);
    x->Lower = IoAttachDeviceToDeviceStack(dev, Pdo);
    if (x->Lower == NULL) {
        IoDeleteDevice(dev);
        return STATUS_NO_SUCH_DEVICE;
    }
    dev->DeviceType = x->Lower->DeviceType;
    dev->Characteristics = x->Lower->Characteristics;
    dev->Flags |= x->Lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE | DO_POWER_INRUSH);
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    FLT_LOG("attached above %p", Pdo);
    return STATUS_SUCCESS;
}

static VOID
FltUnload(_In_ PDRIVER_OBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT Driver, _In_ PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
#pragma warning(suppress: 28168 28169)  /* one pass-through for every major function */
        Driver->MajorFunction[i] = FltPass;
    }
    Driver->MajorFunction[IRP_MJ_PNP] = FltPnp;
    Driver->DriverExtension->AddDevice = FltAddDevice;
    Driver->DriverUnload = FltUnload;
    return STATUS_SUCCESS;
}
