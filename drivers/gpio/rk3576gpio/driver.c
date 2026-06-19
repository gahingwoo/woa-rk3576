/*++

Module Name:

    driver.c

Abstract:

    DriverEntry, WDF device-add, and GpioClx client registration for the RK3576
    GPIO controller miniport.

Environment:

    Kernel mode.

--*/

#include "rk3576gpio.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, RkGpioEvtDeviceAdd)
#pragma alloc_text(PAGE, RkGpioEvtDriverUnload)
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
    WDFDRIVER driver;
    GPIO_CLIENT_REGISTRATION_PACKET registrationPacket;
    NTSTATUS status;

    RkLog(RK_DBG_INFO, "DriverEntry\n");

    //
    // Create the WDF driver object. GpioClx requires a KMDF driver; the
    // device-add callback wires each ACPI\RKCP3002 instance up to GpioClx.
    //
    WDF_DRIVER_CONFIG_INIT(&config, RkGpioEvtDeviceAdd);
    config.DriverPoolTag = RK3576GPIO_POOL_TAG;
    config.EvtDriverUnload = RkGpioEvtDriverUnload;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             &driver);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDriverCreate failed 0x%08x\n", status);
        return status;
    }

    //
    // Register as a GpioClx client. The packet wires our register-level
    // callbacks; GpioClx owns PnP/power/interrupt connection and the
    // GpioIo/GpioInt resource hub for downstream ACPI devices.
    //
    RtlZeroMemory(&registrationPacket, sizeof(registrationPacket));
    registrationPacket.Version = GPIO_CLIENT_VERSION;
    registrationPacket.Size = sizeof(registrationPacket);
    registrationPacket.GpioDeviceContextSize = sizeof(RK3576GPIO_CONTEXT);
    registrationPacket.GpioPinContextSize = 0;

    registrationPacket.CLIENT_PrepareController = RkGpioPrepareController;
    registrationPacket.CLIENT_ReleaseController = RkGpioReleaseController;
    registrationPacket.CLIENT_StartController = RkGpioStartController;
    registrationPacket.CLIENT_StopController = RkGpioStopController;
    registrationPacket.CLIENT_QueryControllerBasicInformation =
        RkGpioQueryControllerBasicInformation;

    registrationPacket.CLIENT_ConnectIoPins = RkGpioConnectIoPins;
    registrationPacket.CLIENT_DisconnectIoPins = RkGpioDisconnectIoPins;
    registrationPacket.CLIENT_ReadGpioPinsUsingMask = RkGpioReadPinsUsingMask;
    registrationPacket.CLIENT_WriteGpioPinsUsingMask = RkGpioWritePinsUsingMask;

    registrationPacket.CLIENT_EnableInterrupt = RkGpioEnableInterrupt;
    registrationPacket.CLIENT_DisableInterrupt = RkGpioDisableInterrupt;
    registrationPacket.CLIENT_MaskInterrupts = RkGpioMaskInterrupts;
    registrationPacket.CLIENT_UnmaskInterrupt = RkGpioUnmaskInterrupt;
    registrationPacket.CLIENT_QueryActiveInterrupts = RkGpioQueryActiveInterrupts;
    registrationPacket.CLIENT_ClearActiveInterrupts = RkGpioClearActiveInterrupts;
    registrationPacket.CLIENT_ReconfigureInterrupt = RkGpioReconfigureInterrupt;
    registrationPacket.CLIENT_QueryEnabledInterrupts = RkGpioQueryEnabledInterrupts;

    status = GPIO_CLX_RegisterClient(DriverObject,
                                     &registrationPacket,
                                     RegistryPath);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "GPIO_CLX_RegisterClient failed 0x%08x\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES fdoAttributes;
    WDFDEVICE device;
    NTSTATUS status;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT(&fdoAttributes);

    //
    // Let GpioClx adjust the device init (PnP/power policy, IO type, etc.)
    // before we create the FDO.
    //
    status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver,
                                                      DeviceInit,
                                                      &fdoAttributes);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "PreDeviceCreate failed 0x%08x\n", status);
        return status;
    }

    status = WdfDeviceCreate(&DeviceInit, &fdoAttributes, &device);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "WdfDeviceCreate failed 0x%08x\n", status);
        return status;
    }

    //
    // Hand the freshly created FDO back to GpioClx so it can attach its own
    // state and begin servicing the controller.
    //
    status = GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, device);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "PostDeviceCreate failed 0x%08x\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
RkGpioEvtDriverUnload(
    WDFDRIVER Driver
    )
{
    PDRIVER_OBJECT driverObject;
    NTSTATUS status;

    PAGED_CODE();

    driverObject = WdfDriverWdmGetDriverObject(Driver);

    status = GPIO_CLX_UnregisterClient(driverObject);
    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR, "GPIO_CLX_UnregisterClient failed 0x%08x\n", status);
    }
}
