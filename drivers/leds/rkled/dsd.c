/*
 * dsd.c - which LED shows what, from the firmware's _DSD and the registry
 *
 * Device properties (UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301) of
 * \_SB.LEDS, written by each board's Leds.asl in edk2-rk3576, one entry per
 * GpioIo in _CRS, in order:
 *
 *   rockchip,led-active-low   Package: 1 = the LED is on when the pin is low
 *                             (the device tree's GPIO_ACTIVE_LOW)
 *   rockchip,led-trigger      Package: 0 off, 1 on, 2 heartbeat,
 *                             3 disk activity
 *
 * Overrides, REG_DWORD in the device's "Device Parameters" key
 * (HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP1ED0\0\Device Parameters),
 * read at start: Trigger0, Trigger1, ... with the values above.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rkled.h"
#include <acpiioct.h>
#include <initguid.h>

DEFINE_GUID(RKLED_GUID_ACPI_DSD,
    0xdaffd814, 0x6eba, 0x4d8c, 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01);

#define RKLED_DSD_MAX_BYTES     2048

static
BOOLEAN
DsdNameIs(
    _In_ PACPI_METHOD_ARGUMENT name,
    _In_ PCSTR str
    )
{
    size_t len = strlen(str);

    return name->Type == ACPI_METHOD_ARGUMENT_STRING &&
           name->DataLength == len + 1 &&
           RtlCompareMemory(name->Data, str, len) == len;
}

/* A Package of Integers, or one Integer.  Returns how many were stored. */
static
ULONG
DsdIntegers(
    _In_ PACPI_METHOD_ARGUMENT value,
    _Out_writes_(max) PULONG out,
    _In_ ULONG max
    )
{
    PACPI_METHOD_ARGUMENT item;
    PUCHAR end;
    ULONG n = 0;

    if (value->Type == ACPI_METHOD_ARGUMENT_INTEGER) {
        if (max == 0) {
            return 0;
        }
        out[0] = 0;
        RtlCopyMemory(&out[0], value->Data, min(value->DataLength, sizeof(ULONG)));
        return 1;
    }
    if (value->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return 0;
    }
    end = value->Data + value->DataLength;
    for (item = (PACPI_METHOD_ARGUMENT)value->Data;
         (PUCHAR)item + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) <= end && n < max;
         item = ACPI_METHOD_NEXT_ARGUMENT(item)) {
        if ((PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(item) > end ||
            item->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
            break;
        }
        out[n] = 0;
        RtlCopyMemory(&out[n], item->Data, min(item->DataLength, sizeof(ULONG)));
        n++;
    }
    return n;
}

_IRQL_requires_(PASSIVE_LEVEL)
static
NTSTATUS
DsdRead(
    _In_ PRKLED_CONTEXT ctx,
    _Out_writes_(RKLED_MAX_LEDS) PULONG activeLow,
    _Out_writes_(RKLED_MAX_LEDS) PULONG trigger
    )
{
    ACPI_EVAL_INPUT_BUFFER_EX input;
    ACPI_EVAL_OUTPUT_BUFFER sizeOnly;
    WDF_MEMORY_DESCRIPTOR inDesc, sizeDesc, outDesc;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFMEMORY outMem = NULL;
    PACPI_EVAL_OUTPUT_BUFFER out;
    PACPI_METHOD_ARGUMENT arg;
    PUCHAR end;
    size_t outSize;
    BOOLEAN found = FALSE;
    NTSTATUS status;
    ULONG i;

    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
    status = RtlStringCchCopyA(input.MethodName, sizeof(input.MethodName), "_DSD");
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&sizeOnly, sizeof(sizeOnly));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, &input, (ULONG)sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&sizeDesc, &sizeOnly, (ULONG)sizeof(sizeOnly));
    status = WdfIoTargetSendInternalIoctlSynchronously(WdfDeviceGetIoTarget(ctx->Device), NULL,
                                                       IOCTL_ACPI_EVAL_METHOD_EX,
                                                       &inDesc, &sizeDesc, NULL, NULL);
    if (status != STATUS_BUFFER_OVERFLOW) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }
    if (sizeOnly.Length > RKLED_DSD_MAX_BYTES) {
        return STATUS_ACPI_INVALID_DATA;
    }
    outSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + sizeOnly.Length;
    if (outSize < sizeof(ACPI_EVAL_OUTPUT_BUFFER)) {
        outSize = sizeof(ACPI_EVAL_OUTPUT_BUFFER);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = ctx->Device;
    status = WdfMemoryCreate(&attr, NonPagedPoolNx, RKLED_POOL_TAG, outSize, &outMem, (PVOID *)&out);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlZeroMemory(out, outSize);
    WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outDesc, outMem, NULL);
    status = WdfIoTargetSendInternalIoctlSynchronously(WdfDeviceGetIoTarget(ctx->Device), NULL,
                                                       IOCTL_ACPI_EVAL_METHOD_EX,
                                                       &inDesc, &outDesc, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }
    if (out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || (out->Count % 2) != 0) {
        status = STATUS_ACPI_INVALID_DATA;
        goto exit;
    }

    end = (PUCHAR)out + outSize;
    arg = &out->Argument[0];
    for (i = 0; i < out->Count; i += 2) {
        PACPI_METHOD_ARGUMENT uuidArg = arg;
        PACPI_METHOD_ARGUMENT pkgArg = ACPI_METHOD_NEXT_ARGUMENT(uuidArg);
        PACPI_METHOD_ARGUMENT prop;
        PUCHAR pkgEnd;

        arg = ACPI_METHOD_NEXT_ARGUMENT(pkgArg);
        if ((PUCHAR)arg > end ||
            uuidArg->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
            uuidArg->DataLength != sizeof(GUID) ||
            pkgArg->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
            break;
        }
        if (RtlCompareMemory(uuidArg->Data, &RKLED_GUID_ACPI_DSD, sizeof(GUID)) != sizeof(GUID)) {
            continue;
        }
        found = TRUE;

        pkgEnd = pkgArg->Data + pkgArg->DataLength;
        for (prop = (PACPI_METHOD_ARGUMENT)pkgArg->Data;
             (PUCHAR)prop < pkgEnd;
             prop = ACPI_METHOD_NEXT_ARGUMENT(prop)) {
            PACPI_METHOD_ARGUMENT name, value;

            if ((PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(prop) > pkgEnd) {
                break;
            }
            if (prop->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
                continue;
            }
            name = (PACPI_METHOD_ARGUMENT)prop->Data;
            value = ACPI_METHOD_NEXT_ARGUMENT(name);
            if ((PUCHAR)value >= prop->Data + prop->DataLength ||
                (PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(value) > prop->Data + prop->DataLength) {
                continue;
            }
            if (DsdNameIs(name, "rockchip,led-active-low")) {
                (void)DsdIntegers(value, activeLow, RKLED_MAX_LEDS);
            } else if (DsdNameIs(name, "rockchip,led-trigger")) {
                (void)DsdIntegers(value, trigger, RKLED_MAX_LEDS);
            }
        }
    }
    status = found ? STATUS_SUCCESS : STATUS_NOT_FOUND;

exit:
    WdfObjectDelete(outMem);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
RkLedReadConfig(
    _In_ PRKLED_CONTEXT Ctx
    )
{
    ULONG activeLow[RKLED_MAX_LEDS] = { 0 };
    ULONG trigger[RKLED_MAX_LEDS];
    WDFKEY key = NULL;
    NTSTATUS status;
    ULONG i;

    for (i = 0; i < RKLED_MAX_LEDS; i++) {
        trigger[i] = RKLED_TRIGGER_HEARTBEAT;
    }

    status = DsdRead(Ctx, activeLow, trigger);
    if (!NT_SUCCESS(status)) {
        RKLED_LOG("no usable _DSD (0x%08X): every LED active high, heartbeat", status);
    }

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE,
                                             KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        key = NULL;
    }

    Ctx->WantDisk = FALSE;
    for (i = 0; i < Ctx->Count; i++) {
        DECLARE_UNICODE_STRING_SIZE(name, 16);
        ULONG v;

        Ctx->Led[i].ActiveLow = activeLow[i] ? 1 : 0;
        Ctx->Led[i].Trigger = trigger[i] <= RKLED_TRIGGER_MAX ? trigger[i] : RKLED_TRIGGER_HEARTBEAT;
        if (key != NULL &&
            NT_SUCCESS(RtlUnicodeStringPrintf(&name, L"Trigger%u", i)) &&
            NT_SUCCESS(WdfRegistryQueryULong(key, &name, &v)) &&
            v <= RKLED_TRIGGER_MAX) {
            Ctx->Led[i].Trigger = v;
        }
        if (Ctx->Led[i].Trigger == RKLED_TRIGGER_DISK) {
            Ctx->WantDisk = TRUE;
        }
        RKLED_LOG("LED %u: active %s, trigger %u", i,
                  Ctx->Led[i].ActiveLow ? "low" : "high", Ctx->Led[i].Trigger);
    }

    if (key != NULL) {
        WdfRegistryClose(key);
    }
}
