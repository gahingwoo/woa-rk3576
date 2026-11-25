/*
 * driver.c - KMDF/SerCx2 controller driver for the RK3576 UARTs on the
 * 40-pin header (ACPI\RKCP3006)
 *
 * Why a driver at all: Windows' inbox serial.sys drives a 16550 whose
 * registers are one byte apart. Its register offsets are compile-time
 * constants (SERIAL_REGISTER_STRIDE is 1 in the serial.sys sources
 * Microsoft publishes, Windows-driver-samples serial/serial/serial.h), and
 * it reads and writes them a byte at a time. The RK3576 UARTs are
 * DesignWare APB UARTs with 32-bit registers four bytes apart, and they
 * raise a DesignWare-only "busy detect" interrupt (IIR = 7) that serial.sys
 * does not clear. No registry value or _DSD property changes either.
 *
 * So this is a SerCx2 controller driver, PIO only. The firmware describes
 * the UART with "SerCx-FriendlyName" in _DSD, which makes SerCx2 publish a
 * GUID_DEVINTERFACE_COMPORT interface (Windows 10 1903 and later): that is
 * what Windows.Devices.SerialCommunication finds, and what Win32 programs
 * open with CreateFile on the interface path. For programs that only take
 * a COM name, the driver also creates \DosDevices\COMn (UARTn -> COMn,
 * "PortName" in the device's hardware key overrides it, an empty value
 * turns it off) and lists it under HARDWARE\DEVICEMAP\SERIALCOMM, which is
 * where such programs look for ports.
 *
 * The firmware sets the pins and the UART clock (sclk_uartN = xin24m,
 * 24 MHz) only when its "40-pin UART" setup option is on; the ACPI device
 * exists only then. "clock-frequency" in _DSD says the rate.
 *
 * UART0 is the firmware console and the kernel debugger's port on every
 * RK3576 board here (SPCR/DBG2): if a table ever points this driver at it,
 * the driver refuses to start.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xuart.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD RkuEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE RkuEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE RkuEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY RkuEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT RkuEvtD0Exit;

static volatile LONG g_DeviceCount = -1;

/* RK3576 UART register bases, from rk3576.dtsi. */
static const ULONG g_UartBase[] = {
    0x2ad40000, /* UART0: firmware console */
    0x27310000, /* UART1 */
    0x2ad50000, /* UART2 */
    0x2ad60000, /* UART3 */
    0x2ad70000, /* UART4 */
    0x2ad80000, /* UART5 */
    0x2ad90000, /* UART6 */
    0x2ada0000, /* UART7 */
    0x2adb0000, /* UART8 */
    0x2adc0000, /* UART9 */
    0x2afc0000, /* UART10 */
    0x2afd0000, /* UART11 */
};
#define RKU_NO_INDEX    0xFFFFFFFF

/*
 * One integer property from _DSD (UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301),
 * through ACPI's IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA.
 */
static NTSTATUS
RkuDsdInteger(_In_ WDFDEVICE Device, _In_z_ const CHAR *Name, _Out_ PULONG Value)
{
    static const GUID dsdUuid = {
        0xdaffd814, 0x6eba, 0x4d8c, { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 }
    };
    UCHAR inBuf[sizeof(ACPI_GET_DEVICE_SPECIFIC_DATA) + 32];
    UCHAR outBuf[sizeof(ACPI_EVAL_OUTPUT_BUFFER_V1) + 16];
    PACPI_GET_DEVICE_SPECIFIC_DATA in = (PACPI_GET_DEVICE_SPECIFIC_DATA)inBuf;
    PACPI_EVAL_OUTPUT_BUFFER_V1 out = (PACPI_EVAL_OUTPUT_BUFFER_V1)outBuf;
    WDF_MEMORY_DESCRIPTOR inDesc;
    WDF_MEMORY_DESCRIPTOR outDesc;
    size_t nameLen = strlen(Name) + 1;
    NTSTATUS status;

    *Value = 0;
    if (nameLen > 32) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(inBuf, sizeof(inBuf));
    RtlZeroMemory(outBuf, sizeof(outBuf));
    in->Signature = IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE;
    in->Section = dsdUuid;
    in->PropertyNameLength = (ULONG)nameLen;
    RtlCopyMemory(in->PropertyName, Name, nameLen);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, inBuf,
        (ULONG)(FIELD_OFFSET(ACPI_GET_DEVICE_SPECIFIC_DATA, PropertyName) + nameLen));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, outBuf, sizeof(outBuf));

    status = WdfIoTargetSendInternalIoctlSynchronously(
        WdfDeviceGetIoTarget(Device), NULL, IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA,
        &inDesc, &outDesc, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 || out->Count < 1 ||
        out->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER ||
        out->Argument[0].DataLength < sizeof(ULONG)) {
        return STATUS_ACPI_INVALID_DATA;
    }
    *Value = out->Argument[0].Argument;
    return STATUS_SUCCESS;
}

static VOID
RkuReadPortName(_In_ PRKU_DEVICE Dev)
{
    DECLARE_CONST_UNICODE_STRING(valueName, L"PortName");
    UNICODE_STRING value;
    WDFKEY key;
    NTSTATUS status;

    RtlZeroMemory(Dev->PortName, sizeof(Dev->PortName));
    if (Dev->UartIndex != RKU_NO_INDEX) {
        (VOID)RtlStringCchPrintfW(Dev->PortName, ARRAYSIZE(Dev->PortName), L"COM%lu", Dev->UartIndex);
    }

    status = WdfDeviceOpenRegistryKey(Dev->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                      WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return;
    }
    value.Buffer = Dev->PortName;
    value.Length = 0;
    value.MaximumLength = sizeof(Dev->PortName) - sizeof(WCHAR);
    status = WdfRegistryQueryUnicodeString(key, &valueName, NULL, &value);
    if (NT_SUCCESS(status)) {
        Dev->PortName[value.Length / sizeof(WCHAR)] = L'\0';
    } else if (status != STATUS_OBJECT_NAME_NOT_FOUND) {
        RKU_ERR("PortName unreadable (0x%08lx), keeping %ws", status, Dev->PortName);
    }
    WdfRegistryClose(key);
}

static VOID
RkuCreatePortLink(_In_ PRKU_DEVICE Dev)
{
    WCHAR linkBuf[40];
    UNICODE_STRING link;
    UNICODE_STRING target;
    NTSTATUS status;

    if (Dev->PortName[0] == L'\0') {
        return;
    }
    (VOID)RtlStringCchPrintfW(linkBuf, ARRAYSIZE(linkBuf), L"\\DosDevices\\%ws", Dev->PortName);
    RtlInitUnicodeString(&link, linkBuf);
    RtlInitUnicodeString(&target, Dev->DeviceName);

    status = IoCreateSymbolicLink(&link, &target);
    if (!NT_SUCCESS(status)) {
        /* another driver owns the name: the device interface still works */
        RKU_ERR("UART%lu: %ws not created (0x%08lx)", Dev->UartIndex, Dev->PortName, status);
        return;
    }
    Dev->PortLinkCreated = TRUE;

    (VOID)RtlCreateRegistryKey(RTL_REGISTRY_DEVICEMAP, L"SERIALCOMM");
    status = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP, L"SERIALCOMM", Dev->DeviceName,
                                   REG_SZ, Dev->PortName,
                                   (ULONG)((wcslen(Dev->PortName) + 1) * sizeof(WCHAR)));
    Dev->SerialCommWritten = NT_SUCCESS(status);
    RKU_LOG("UART%lu is %ws (%ws)", Dev->UartIndex, Dev->PortName, Dev->DeviceName);
}

static VOID
RkuDeletePortLink(_In_ PRKU_DEVICE Dev)
{
    WCHAR linkBuf[40];
    UNICODE_STRING link;

    if (Dev->SerialCommWritten) {
        (VOID)RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP, L"SERIALCOMM", Dev->DeviceName);
        Dev->SerialCommWritten = FALSE;
    }
    if (Dev->PortLinkCreated) {
        (VOID)RtlStringCchPrintfW(linkBuf, ARRAYSIZE(linkBuf), L"\\DosDevices\\%ws", Dev->PortName);
        RtlInitUnicodeString(&link, linkBuf);
        (VOID)IoDeleteSymbolicLink(&link);
        Dev->PortLinkCreated = FALSE;
    }
}

_Use_decl_annotations_
static NTSTATUS
RkuEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    ULONG count;
    ULONG i;
    ULONG v;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i++) {
        d = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (d->Type == CmResourceTypeMemory && dev->Regs == NULL) {
            dev->RegsPhys = d->u.Memory.Start;
            dev->RegsLength = d->u.Memory.Length;
            dev->Regs = (volatile UCHAR *)MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
        }
    }
    if (dev->Regs == NULL || dev->RegsLength < 0x100) {
        RKU_ERR("no register window");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    dev->UartIndex = RKU_NO_INDEX;
    for (i = 0; i < ARRAYSIZE(g_UartBase); i++) {
        if (dev->RegsPhys.QuadPart == g_UartBase[i]) {
            dev->UartIndex = i;
        }
    }
    if (dev->UartIndex == 0) {
        RKU_ERR("refusing UART0 at %I64x: it is the firmware console and the debugger's port",
                dev->RegsPhys.QuadPart);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    dev->ClockHz = RKU_DEFAULT_CLOCK_HZ;
    if (NT_SUCCESS(RkuDsdInteger(Device, "clock-frequency", &v)) && v != 0) {
        dev->ClockHz = v;
    }
    dev->FifoDepth = RKU_DEFAULT_FIFO;

    RkuReadPortName(dev);
    RkuCreatePortLink(dev);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS
RkuEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    RkuDeletePortLink(dev);
    if (dev->Regs != NULL) {
        MmUnmapIoSpace((PVOID)dev->Regs, dev->RegsLength);
        dev->Regs = NULL;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS
RkuEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    RkuHwInit(RkuGetDevice(Device));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS
RkuEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    RkuHwQuiesce(RkuGetDevice(Device));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS
RkuEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    /* SYSTEM and administrators all, users read and write (unprivileged apps) */
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;UD)(A;;GRGW;;;BU)");
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_INTERRUPT_CONFIG icfg;
    WDF_TIMER_CONFIG tcfg;
    SERCX2_CONFIG scfg;
    SERCX2_PIO_RECEIVE_CONFIG rxcfg;
    SERCX2_PIO_TRANSMIT_CONFIG txcfg;
    WCHAR nameBuf[32];
    UNICODE_STRING name;
    WDFDEVICE device;
    PRKU_DEVICE dev;
    LONG n;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    status = SerCx2InitializeDeviceInit(DeviceInit);
    if (!NT_SUCCESS(status)) {
        RKU_ERR("SerCx2InitializeDeviceInit 0x%08lx", status);
        return status;
    }
    status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* a name, so that \DosDevices\COMn has something to point at */
    n = InterlockedIncrement(&g_DeviceCount);
    (VOID)RtlStringCchPrintfW(nameBuf, ARRAYSIZE(nameBuf), L"\\Device\\RkUart%ld", n);
    RtlInitUnicodeString(&name, nameBuf);
    status = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = RkuEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = RkuEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = RkuEvtD0Entry;
    pnp.EvtDeviceD0Exit = RkuEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, RKU_DEVICE);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        RKU_ERR("WdfDeviceCreate 0x%08lx", status);
        return status;
    }

    dev = RkuGetDevice(device);
    dev->Device = device;
    (VOID)RtlStringCchCopyW(dev->DeviceName, ARRAYSIZE(dev->DeviceName), nameBuf);
    dev->ClockHz = RKU_DEFAULT_CLOCK_HZ;
    dev->FifoDepth = RKU_DEFAULT_FIFO;
    dev->UartIndex = RKU_NO_INDEX;
    dev->Baud = RKU_DEFAULT_BAUD;
    dev->LineControl.WordLength = 8;
    dev->LineControl.StopBits = STOP_BIT_1;
    dev->LineControl.Parity = NO_PARITY;
    dev->Lcr = 0x03;
    dev->Mcr = MCR_DTR | MCR_RTS;
    dev->Handflow.ControlHandShake = SERIAL_DTR_CONTROL;
    dev->Handflow.FlowReplace = SERIAL_RTS_CONTROL;

    WDF_INTERRUPT_CONFIG_INIT(&icfg, RkuEvtIsr, RkuEvtDpc);
    status = WdfInterruptCreate(device, &icfg, WDF_NO_OBJECT_ATTRIBUTES, &dev->Interrupt);
    if (!NT_SUCCESS(status)) {
        RKU_ERR("WdfInterruptCreate 0x%08lx", status);
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&tcfg, RkuEvtDrainTimer);
    tcfg.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    status = WdfTimerCreate(&tcfg, &attr, &dev->DrainTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    SERCX2_CONFIG_INIT(&scfg, RkuEvtApplyConfig, RkuEvtControl, RkuEvtPurgeFifos);
    scfg.EvtSerCx2FileOpen = RkuEvtFileOpen;
    scfg.EvtSerCx2FileClose = RkuEvtFileClose;
    scfg.EvtSerCx2SetWaitMask = RkuEvtSetWaitMask;
    status = SerCx2InitializeDevice(device, &scfg);
    if (!NT_SUCCESS(status)) {
        RKU_ERR("SerCx2InitializeDevice 0x%08lx", status);
        return status;
    }

    SERCX2_PIO_RECEIVE_CONFIG_INIT(&rxcfg, RkuEvtRxReadBuffer, RkuEvtRxEnableReady,
                                   RkuEvtRxCancelReady);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, RKU_PIO_CONTEXT);
    status = SerCx2PioReceiveCreate(device, &rxcfg, &attr, &dev->PioReceive);
    if (!NT_SUCCESS(status)) {
        RKU_ERR("SerCx2PioReceiveCreate 0x%08lx", status);
        return status;
    }
    RkuGetPioContext(dev->PioReceive)->Dev = dev;

    SERCX2_PIO_TRANSMIT_CONFIG_INIT(&txcfg, RkuEvtTxWriteBuffer, RkuEvtTxEnableReady,
                                    RkuEvtTxCancelReady);
    txcfg.EvtSerCx2PioTransmitDrainFifo = RkuEvtTxDrainFifo;
    txcfg.EvtSerCx2PioTransmitCancelDrainFifo = RkuEvtTxCancelDrainFifo;
    txcfg.EvtSerCx2PioTransmitPurgeFifo = RkuEvtTxPurgeFifo;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, RKU_PIO_CONTEXT);
    status = SerCx2PioTransmitCreate(device, &txcfg, &attr, &dev->PioTransmit);
    if (!NT_SUCCESS(status)) {
        RKU_ERR("SerCx2PioTransmitCreate 0x%08lx", status);
        return status;
    }
    RkuGetPioContext(dev->PioTransmit)->Dev = dev;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, RkuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
}
