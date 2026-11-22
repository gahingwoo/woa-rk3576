/*
 * btgpio.c - BT_REG_ON and BT_WAKE through GpioClx (rk3xgpio)
 *
 * Both are GpioIo output resources in the device's _CRS, in that order:
 * BT_REG_ON powers the Bluetooth half of the module (high = on), BT_WAKE
 * keeps it awake (high = awake). The driver does not use the module's
 * sleep mode, so BT_WAKE simply follows BT_REG_ON.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcm4345bt.h"

static NTSTATUS
OpenOne(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ LARGE_INTEGER Id, _Out_ WDFIOTARGET *Target)
{
    DECLARE_UNICODE_STRING_SIZE(path, RESOURCE_HUB_PATH_SIZE);
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_IO_TARGET_OPEN_PARAMS open;
    NTSTATUS status;

    *Target = NULL;
    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, Id.LowPart, Id.HighPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Ctx->Device;
    status = WdfIoTargetCreate(Ctx->Device, &attr, Target);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &path, GENERIC_READ | GENERIC_WRITE);
    status = WdfIoTargetOpen(*Target, &open);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return status;
}

NTSTATUS
BtGpioOpen(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    if (!Ctx->HaveRegOn) {
        BT_LOG("no BT_REG_ON GpioIo in _CRS (%u GPIO resources)", 0u);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    status = OpenOne(Ctx, Ctx->RegOnConnection, &Ctx->RegOnTarget);
    if (!NT_SUCCESS(status)) {
        BT_LOG("BT_REG_ON: open failed 0x%08X (is rk3xgpio installed?)", status);
        return status;
    }
    if (Ctx->HaveWake) {
        status = OpenOne(Ctx, Ctx->WakeConnection, &Ctx->WakeTarget);
        if (!NT_SUCCESS(status)) {
            BT_LOG("BT_WAKE: open failed 0x%08X, going on without it", status);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
BtGpioSet(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ WDFIOTARGET Target, _In_ BOOLEAN High)
{
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_REQUEST_SEND_OPTIONS opts;
    UCHAR value = High ? 1 : 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Ctx);
    if (Target == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    /* The GPIO client documentation passes the same buffer both ways. */
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &value, sizeof(value));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &value, sizeof(value));
    /* Bounded: on the stop path the GPIO controller may already be gone. */
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_MS(200));
    status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_GPIO_WRITE_PINS,
                                               &in, &out, &opts, NULL);
    return status;
}

VOID
BtPower(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ BOOLEAN On)
{
    NTSTATUS a, b = STATUS_SUCCESS;

    a = BtGpioSet(Ctx, Ctx->RegOnTarget, On);
    if (Ctx->WakeTarget != NULL) {
        b = BtGpioSet(Ctx, Ctx->WakeTarget, On);
    }
    if (!NT_SUCCESS(a) || !NT_SUCCESS(b)) {
        BT_LOG("power %s: BT_REG_ON 0x%08X, BT_WAKE 0x%08X", On ? "on" : "off", a, b);
    }
}
