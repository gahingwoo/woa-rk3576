/*++

Module Name:

    driver.c

Abstract:

    DriverEntry, SpbCx controller setup, and PnP hardware mapping for the
    Rockchip SPI controller.

Environment:

    Kernel mode.

--*/

#include "rk3xspi.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Rk3xSpiEvtDeviceAdd)
#pragma alloc_text(PAGE, Rk3xSpiEvtPrepareHardware)
#pragma alloc_text(PAGE, Rk3xSpiEvtReleaseHardware)
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

    WDF_DRIVER_CONFIG_INIT(&config, Rk3xSpiEvtDeviceAdd);
    config.DriverPoolTag = RK3XSPI_POOL_TAG;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDriverCreate failed 0x%08x\n", status);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS
Rk3xSpiEvtDeviceAdd(
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

    status = SpbDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "SpbDeviceInitConfig failed 0x%08x\n", status);
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = Rk3xSpiEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = Rk3xSpiEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RK3XSPI_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDeviceCreate failed 0x%08x\n", status);
        return status;
    }

    SPB_CONTROLLER_CONFIG_INIT(&spbConfig);
    spbConfig.ControllerDispatchType = WdfIoQueueDispatchSequential;
    spbConfig.PowerManaged = WdfTrue;
    spbConfig.EvtSpbTargetConnect = Rk3xSpiEvtTargetConnect;
    spbConfig.EvtSpbControllerLock = Rk3xSpiEvtControllerLock;
    spbConfig.EvtSpbControllerUnlock = Rk3xSpiEvtControllerUnlock;
    spbConfig.EvtSpbIoRead = Rk3xSpiEvtIoRead;
    spbConfig.EvtSpbIoWrite = Rk3xSpiEvtIoWrite;
    spbConfig.EvtSpbIoSequence = Rk3xSpiEvtIoSequence;
    spbConfig.EvtSpbIoOther = Rk3xSpiEvtIoOther;   // SPI full-duplex IOCTL

    status = SpbDeviceInitialize(device, &spbConfig);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "SpbDeviceInitialize failed 0x%08x\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Rk3xSpiEvtPrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PRK3XSPI_CONTEXT ctx = GetControllerContext(Device);
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
        if (desc->Type == CmResourceTypeMemory && !memoryFound) {
            ctx->RegsPhysical = desc->u.Memory.Start;
            ctx->RegsLength = desc->u.Memory.Length;
            ctx->Regs = (volatile UCHAR *)MmMapIoSpaceEx(desc->u.Memory.Start,
                                                         desc->u.Memory.Length,
                                                         PAGE_READWRITE | PAGE_NOCACHE);
            if (ctx->Regs == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            memoryFound = TRUE;
        }
    }

    if (!memoryFound) {
        RkLog(RK_DBG_ERROR, "no memory resource in _CRS\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Rk3xSpiHwInit(ctx);

    RkLog(RK_DBG_INFO, "PrepareHardware @ 0x%llx\n", ctx->RegsPhysical.QuadPart);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
Rk3xSpiEvtReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PRK3XSPI_CONTEXT ctx = GetControllerContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    if (ctx->Regs != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Regs, ctx->RegsLength);
        ctx->Regs = NULL;
    }
    return STATUS_SUCCESS;
}
