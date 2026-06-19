/*++

Module Name:

    driver.c

Abstract:

    DriverEntry, SpbCx controller setup, and PnP hardware mapping for the
    Rockchip rk3x I2C controller.

Environment:

    Kernel mode.

--*/

#include "rk3xi2c.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Rk3xI2cEvtDeviceAdd)
#pragma alloc_text(PAGE, Rk3xI2cEvtPrepareHardware)
#pragma alloc_text(PAGE, Rk3xI2cEvtReleaseHardware)
#endif

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    RkLog(RK_DBG_INFO, "DriverEntry\n");

    WDF_DRIVER_CONFIG_INIT(&config, Rk3xI2cEvtDeviceAdd);
    config.DriverPoolTag = RK3XI2C_POOL_TAG;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDriverCreate failed 0x%08x\n", status);
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
Rk3xI2cEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    SPB_CONTROLLER_CONFIG spbConfig;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    //
    // Mark this as an SPB controller device. Must be done before WdfDeviceCreate.
    //
    status = SpbDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "SpbDeviceInitConfig failed 0x%08x\n", status);
        return status;
    }

    //
    // We map/unmap the controller registers in PnP prepare/release.
    //
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = Rk3xI2cEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = Rk3xI2cEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RK3XI2C_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDeviceCreate failed 0x%08x\n", status);
        return status;
    }

    //
    // Hand the bus over to SpbCx: it owns the I/O queue (sequential dispatch)
    // and the I2cSerialBus resource hub; we just service its callbacks.
    //
    SPB_CONTROLLER_CONFIG_INIT(&spbConfig);
    spbConfig.ControllerDispatchType = WdfIoQueueDispatchSequential;
    spbConfig.PowerManaged = WdfTrue;
    spbConfig.EvtSpbTargetConnect = Rk3xI2cEvtTargetConnect;
    spbConfig.EvtSpbControllerLock = Rk3xI2cEvtControllerLock;
    spbConfig.EvtSpbControllerUnlock = Rk3xI2cEvtControllerUnlock;
    spbConfig.EvtSpbIoRead = Rk3xI2cEvtIoRead;
    spbConfig.EvtSpbIoWrite = Rk3xI2cEvtIoWrite;
    spbConfig.EvtSpbIoSequence = Rk3xI2cEvtIoSequence;

    status = SpbDeviceInitialize(device, &spbConfig);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "SpbDeviceInitialize failed 0x%08x\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Rk3xI2cEvtPrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PRK3XI2C_CONTEXT ctx = GetControllerContext(Device);
    ULONG count;
    BOOLEAN memoryFound = FALSE;
    ULONG i;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i += 1) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc == NULL) {
            continue;
        }

        //
        // The controller MMIO window. The ACPI interrupt is intentionally left
        // unclaimed: v1 polls REG_IPD with hardware interrupts disabled.
        //
        if (desc->Type == CmResourceTypeMemory && !memoryFound) {
            ctx->RegsPhysical = desc->u.Memory.Start;
            ctx->RegsLength = desc->u.Memory.Length;
            ctx->Regs = (volatile UCHAR *)MmMapIoSpaceEx(desc->u.Memory.Start,
                                                         desc->u.Memory.Length,
                                                         PAGE_READWRITE | PAGE_NOCACHE);
            if (ctx->Regs == NULL) {
                RkLog(RK_DBG_ERROR, "MmMapIoSpaceEx failed 0x%llx\n",
                      desc->u.Memory.Start.QuadPart);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            memoryFound = TRUE;
        }
    }

    if (!memoryFound) {
        RkLog(RK_DBG_ERROR, "no memory resource in _CRS\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Rk3xI2cHwInit(ctx);

    RkLog(RK_DBG_INFO, "PrepareHardware @ 0x%llx\n", ctx->RegsPhysical.QuadPart);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Rk3xI2cEvtReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PRK3XI2C_CONTEXT ctx = GetControllerContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    if (ctx->Regs != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Regs, ctx->RegsLength);
        ctx->Regs = NULL;
    }

    return STATUS_SUCCESS;
}
