/*
 * ioctl.c - \\.\RK3XSARADC, for adctest.exe
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xsaradc.h"

static
VOID
RkAdcFillInfoLocked(
    _In_ PRKADC_CONTEXT ctx,
    _Out_ PRK3X_SARADC_INFO info
    )
{
    ULONG i;

    RtlZeroMemory(info, sizeof(*info));
    info->Size = sizeof(*info);
    info->Version = RK3X_SARADC_INTERFACE_VERSION;
    info->Flags = (ctx->HaveKeys && ctx->VhfStarted ? RK3X_SARADC_FLAG_KEYS : 0) |
                  (ctx->HaveJack ? RK3X_SARADC_FLAG_JACK : 0) |
                  (ctx->Polling ? RK3X_SARADC_FLAG_POLLING : 0) |
                  (ctx->Stopped ? RK3X_SARADC_FLAG_STOPPED : 0);
    info->Channels = RKADC_CHANNELS;
    info->Bits = RKADC_BITS;
    info->VrefMicrovolts = ctx->VrefMicrovolts;
    info->PollIntervalMs = ctx->PollMs;
    info->KeysChannel = ctx->HaveKeys ? ctx->KeysChannel : RK3X_SARADC_NO_CHANNEL;
    info->KeyCount = ctx->HaveKeys ? ctx->Keymap.Count : 0;
    info->KeyupMicrovolts = ctx->Keymap.KeyupMicrovolts;
    for (i = 0; i < RK3X_SARADC_MAX_KEYS && i < info->KeyCount; i++) {
        info->KeyPressMicrovolts[i] = ctx->Keymap.PressMicrovolts[i];
        info->KeyCode[i] = ctx->KeyCode[i];
        info->KeyHidUsage[i] = ctx->KeyUsage[i];
    }
    info->KeyPressed = ctx->HaveKeys ? ctx->KeyDebounce.Stable : SARADC_NO_KEY;
    info->JackChannel = ctx->HaveJack ? ctx->JackChannel : RK3X_SARADC_NO_CHANNEL;
    info->JackPluggedBelowMicrovolts = ctx->Jack.PluggedBelowMicrovolts;
    info->JackUnpluggedAboveMicrovolts = ctx->Jack.UnpluggedAboveMicrovolts;
    info->JackDebounceMs = ctx->Jack.DebounceMs;
    info->JackConnected = ctx->Jack.Known ? (unsigned int)ctx->Jack.Connected : 0xFFFFFFFFu;
    for (i = 0; i < RKADC_CHANNELS; i++) {
        info->LastRaw[i] = ctx->LastRaw[i];
    }
    info->Conversions = ctx->Conversions;
    info->Timeouts = ctx->Timeouts;
    info->Notifications = ctx->Notifications;
    info->HidReports = ctx->HidReports;
    info->Resets = ctx->Resets;
    info->ResetAvailable = ctx->HaveReset;
}

VOID
RkAdcEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    PRKADC_CONTEXT ctx = RkAdcGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;
    size_t info = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_RK3X_SARADC_GET_INFO: {
        PRK3X_SARADC_INFO out;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            WdfWaitLockAcquire(ctx->Lock, NULL);
            RkAdcFillInfoLocked(ctx, out);
            WdfWaitLockRelease(ctx->Lock);
            info = sizeof(*out);
        }
        break;
    }

    case IOCTL_RK3X_SARADC_READ_RAW: {
        PRK3X_SARADC_READ_IN in;
        PRK3X_SARADC_READ_OUT out;
        ULONG channel, raw = 0;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }
        channel = in->Channel;          /* METHOD_BUFFERED: in and out share */
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (channel >= RKADC_CHANNELS) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        WdfWaitLockAcquire(ctx->Lock, NULL);
        status = RkAdcConvertLocked(ctx, channel, &raw);
        WdfWaitLockRelease(ctx->Lock);
        if (NT_SUCCESS(status)) {
            out->Channel = channel;
            out->Raw = raw;
            out->Microvolts = SaradcRawToMicrovolts(raw, ctx->VrefMicrovolts, RKADC_BITS);
            info = sizeof(*out);
        }
        break;
    }

    case IOCTL_RK3X_SARADC_GET_JACK: {
        PRK3X_SARADC_JACK_INFO out;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            WdfWaitLockAcquire(ctx->Lock, NULL);
            RkAdcJackInfo(ctx, out);
            WdfWaitLockRelease(ctx->Lock);
            info = sizeof(*out);
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}
