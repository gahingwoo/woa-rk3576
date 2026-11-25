/*
 * dsd.c - what is wired to the SARADC, from the firmware's _DSD
 *
 * Device properties (UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301) of
 * \_SB.ADC0, written by edk2-rk3576 Saradc.asl from each board's DSDT.
 * The names follow the Linux adc-keys and rockchip,saradc bindings where
 * there is one, flattened, because an ACPI device property cannot hold a
 * child node the way a device tree does:
 *
 *   rockchip,vref-microvolt                 vref-supply's voltage (1800000)
 *   rockchip,poll-interval-ms               adc-keys poll-interval (100)
 *
 *   rockchip,adc-keys-channel               io-channels = <&saradc N>
 *   rockchip,adc-keys-keyup-microvolt       keyup-threshold-microvolt
 *   rockchip,adc-keys-press-microvolt       Package: press-threshold-microvolt
 *                                           of each button
 *   rockchip,adc-keys-code                  Package: linux,code of each button
 *   rockchip,adc-keys-hid-usage             optional Package: HID usage
 *                                           (page << 16 | id) per button, 0 =
 *                                           derived from the code
 *
 *   rockchip,jack-channel                   the channel the jack is read on
 *   rockchip,jack-plugged-below-microvolt   plugged below this
 *   rockchip,jack-unplugged-above-microvolt unplugged above this
 *   rockchip,jack-debounce-ms               a new state must hold this long
 *
 * Overrides, REG_DWORD in the device's "Device Parameters" key, read at
 * start (HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0ADC\0\Device Parameters):
 *   Keys 0             no key reports
 *   Jack 0             no jack reports
 *   PollIntervalMs     20..2000
 *   KeySamples         samples a key state must be seen in a row (default 2)
 *   JackPluggedBelowMicrovolts, JackUnpluggedAboveMicrovolts, JackDebounceMs
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xsaradc.h"
#include <acpiioct.h>
#include <ntstrsafe.h>
#include <initguid.h>

DEFINE_GUID(RKADC_GUID_ACPI_DSD,
    0xdaffd814, 0x6eba, 0x4d8c, 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01);

#define RKADC_DSD_MAX_BYTES     4096

typedef struct _RKADC_DSD {
    ULONG Vref;
    ULONG PollMs;
    ULONG KeysChannel;
    ULONG Keyup;
    ULONG PressCount;
    ULONG Press[RK3X_SARADC_MAX_KEYS];
    ULONG CodeCount;
    ULONG Code[RK3X_SARADC_MAX_KEYS];
    ULONG UsageCount;
    ULONG Usage[RK3X_SARADC_MAX_KEYS];
    ULONG JackChannel;
    ULONG JackBelow;
    ULONG JackAbove;
    ULONG JackDebounce;
} RKADC_DSD;

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

static
BOOLEAN
DsdInteger(
    _In_ PACPI_METHOD_ARGUMENT value,
    _Out_ PULONG out
    )
{
    *out = 0;
    if (value->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
        return FALSE;
    }
    RtlCopyMemory(out, value->Data, min(value->DataLength, sizeof(*out)));
    return TRUE;
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
        return (max > 0 && DsdInteger(value, &out[0])) ? 1 : 0;
    }
    if (value->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return 0;
    }
    end = value->Data + value->DataLength;
    for (item = (PACPI_METHOD_ARGUMENT)value->Data;
         (PUCHAR)item + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) <= end && n < max;
         item = ACPI_METHOD_NEXT_ARGUMENT(item)) {
        if ((PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(item) > end) {
            break;
        }
        if (!DsdInteger(item, &out[n])) {
            break;
        }
        n++;
    }
    return n;
}

static
VOID
DsdProperty(
    _Inout_ RKADC_DSD *d,
    _In_ PACPI_METHOD_ARGUMENT name,
    _In_ PACPI_METHOD_ARGUMENT value
    )
{
    ULONG v;

    if (DsdNameIs(name, "rockchip,vref-microvolt") && DsdInteger(value, &v)) {
        d->Vref = v;
    } else if (DsdNameIs(name, "rockchip,poll-interval-ms") && DsdInteger(value, &v)) {
        d->PollMs = v;
    } else if (DsdNameIs(name, "rockchip,adc-keys-channel") && DsdInteger(value, &v)) {
        d->KeysChannel = v;
    } else if (DsdNameIs(name, "rockchip,adc-keys-keyup-microvolt") && DsdInteger(value, &v)) {
        d->Keyup = v;
    } else if (DsdNameIs(name, "rockchip,adc-keys-press-microvolt")) {
        d->PressCount = DsdIntegers(value, d->Press, RK3X_SARADC_MAX_KEYS);
    } else if (DsdNameIs(name, "rockchip,adc-keys-code")) {
        d->CodeCount = DsdIntegers(value, d->Code, RK3X_SARADC_MAX_KEYS);
    } else if (DsdNameIs(name, "rockchip,adc-keys-hid-usage")) {
        d->UsageCount = DsdIntegers(value, d->Usage, RK3X_SARADC_MAX_KEYS);
    } else if (DsdNameIs(name, "rockchip,jack-channel") && DsdInteger(value, &v)) {
        d->JackChannel = v;
    } else if (DsdNameIs(name, "rockchip,jack-plugged-below-microvolt") && DsdInteger(value, &v)) {
        d->JackBelow = v;
    } else if (DsdNameIs(name, "rockchip,jack-unplugged-above-microvolt") && DsdInteger(value, &v)) {
        d->JackAbove = v;
    } else if (DsdNameIs(name, "rockchip,jack-debounce-ms") && DsdInteger(value, &v)) {
        d->JackDebounce = v;
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
static
NTSTATUS
DsdRead(
    _In_ PRKADC_CONTEXT ctx,
    _Inout_ RKADC_DSD *d
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
    if (sizeOnly.Length > RKADC_DSD_MAX_BYTES) {
        return STATUS_ACPI_INVALID_DATA;
    }
    outSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + sizeOnly.Length;
    if (outSize < sizeof(ACPI_EVAL_OUTPUT_BUFFER)) {
        outSize = sizeof(ACPI_EVAL_OUTPUT_BUFFER);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = ctx->Device;
    status = WdfMemoryCreate(&attr, NonPagedPoolNx, RKADC_POOL_TAG, outSize, &outMem, (PVOID *)&out);
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
        if (RtlCompareMemory(uuidArg->Data, &RKADC_GUID_ACPI_DSD, sizeof(GUID)) != sizeof(GUID)) {
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
            DsdProperty(d, name, value);
        }
    }
    status = found ? STATUS_SUCCESS : STATUS_NOT_FOUND;

exit:
    WdfObjectDelete(outMem);
    return status;
}

static
ULONG
RegDword(
    _In_opt_ WDFKEY key,
    _In_ PCWSTR name,
    _In_ ULONG dflt
    )
{
    UNICODE_STRING us;
    ULONG v;

    if (key == NULL) {
        return dflt;
    }
    RtlInitUnicodeString(&us, name);
    return NT_SUCCESS(WdfRegistryQueryULong(key, &us, &v)) ? v : dflt;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
RkAdcReadConfig(
    _In_ PRKADC_CONTEXT ctx
    )
{
    RKADC_DSD d;
    WDFKEY key = NULL;
    ULONG i, keys;

    RtlZeroMemory(&d, sizeof(d));
    d.Vref = RKADC_DEFAULT_VREF_UV;
    d.PollMs = RKADC_DEFAULT_POLL_MS;
    d.KeysChannel = RK3X_SARADC_NO_CHANNEL;
    d.JackChannel = RK3X_SARADC_NO_CHANNEL;
    d.JackDebounce = RKADC_DEFAULT_JACK_MS;

    ctx->DsdStatus = DsdRead(ctx, &d);

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(ctx->Device, PLUGPLAY_REGKEY_DEVICE,
                                             KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        key = NULL;
    }

    ctx->VrefMicrovolts = (d.Vref >= 1000000u && d.Vref <= 3300000u) ? d.Vref : RKADC_DEFAULT_VREF_UV;
    ctx->PollMs = RegDword(key, L"PollIntervalMs", d.PollMs);
    if (ctx->PollMs < RKADC_MIN_POLL_MS || ctx->PollMs > RKADC_MAX_POLL_MS) {
        ctx->PollMs = RKADC_DEFAULT_POLL_MS;
    }

    /* adc-keys: a channel, a key-up voltage and at least one button */
    keys = min(d.PressCount, d.CodeCount);
    ctx->HaveKeys = (BOOLEAN)(d.KeysChannel < RKADC_CHANNELS && keys > 0 && d.Keyup > 0 &&
                              RegDword(key, L"Keys", 1) != 0);
    if (ctx->HaveKeys) {
        ctx->KeysChannel = d.KeysChannel;
        ctx->Keymap.Count = keys;
        ctx->Keymap.KeyupMicrovolts = d.Keyup;
        for (i = 0; i < keys; i++) {
            ULONG usage = (i < d.UsageCount) ? d.Usage[i] : 0;

            ctx->Keymap.PressMicrovolts[i] = d.Press[i];
            ctx->KeyCode[i] = d.Code[i];
            if (usage == 0) {
                usage = SaradcLinuxCodeToHidUsage(d.Code[i]);
            }
            if (usage == 0 || !SaradcHidUsageSupported(usage)) {
                /* Recovery, Maskrom, KEY_VENDOR: a vendor-page button,
                 * one per key, for a program to pick up. */
                usage = (SARADC_HID_PAGE_VENDOR << 16) | (i + 1);
            }
            ctx->KeyUsage[i] = usage;
        }
        ctx->KeySamples = RegDword(key, L"KeySamples", RKADC_DEFAULT_KEY_SAMPLES);
        if (ctx->KeySamples > 20u) {
            ctx->KeySamples = RKADC_DEFAULT_KEY_SAMPLES;
        }
        SaradcDebounceInit(&ctx->KeyDebounce, SARADC_NO_KEY, ctx->KeySamples);
    }

    /* jack: a channel and a plugged-below threshold */
    d.JackBelow = RegDword(key, L"JackPluggedBelowMicrovolts", d.JackBelow);
    d.JackAbove = RegDword(key, L"JackUnpluggedAboveMicrovolts", d.JackAbove);
    d.JackDebounce = RegDword(key, L"JackDebounceMs", d.JackDebounce);
    ctx->HaveJack = (BOOLEAN)(d.JackChannel < RKADC_CHANNELS && d.JackBelow > 0 &&
                              RegDword(key, L"Jack", 1) != 0);
    if (ctx->HaveJack) {
        ctx->JackChannel = d.JackChannel;
        if (d.JackAbove == 0) {
            d.JackAbove = d.JackBelow;
        }
        SaradcJackInit(&ctx->Jack, d.JackBelow, d.JackAbove,
                       min(d.JackDebounce, RKADC_MAX_JACK_MS));
    }

    if (key != NULL) {
        WdfRegistryClose(key);
    }

    RKADC_LOG("_DSD 0x%08X vref %u uV, poll %u ms, keys %s ch %d x%u, jack %s ch %d",
              ctx->DsdStatus, ctx->VrefMicrovolts, ctx->PollMs,
              ctx->HaveKeys ? "on" : "off", (int)ctx->KeysChannel, ctx->Keymap.Count,
              ctx->HaveJack ? "on" : "off", (int)ctx->JackChannel);
}
