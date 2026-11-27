/*
 * jack.c - the headphone jack state, for es8323.sys
 *
 * The protocol is in rk3xsaradc_ioctl.h: the callback object
 * \Callback\Rk3xSaradcJack, Argument1 RK3X_SARADC_JACK_STATE with a
 * RK3X_SARADC_JACK_INFO, sent on every debounced change and in answer to
 * RK3X_SARADC_JACK_QUERY.  The answer comes from a work item, never from
 * inside the query, so a consumer that queries from its own start-up path
 * cannot recurse into the object.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xsaradc.h"

static CALLBACK_FUNCTION RkAdcJackCallback;

VOID
RkAdcJackInfo(
    _In_ PRKADC_CONTEXT ctx,
    _Out_ PRK3X_SARADC_JACK_INFO info
    )
{
    RtlZeroMemory(info, sizeof(*info));
    info->Size = sizeof(*info);
    info->Known = (unsigned int)ctx->Jack.Known;
    info->Connected = (unsigned int)ctx->Jack.Connected;
    info->Microvolts = ctx->JackMicrovolts;
    info->Sequence = ctx->JackSequence;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
RkAdcJackNotify(
    _In_ PRKADC_CONTEXT ctx
    )
{
    RK3X_SARADC_JACK_INFO info;
    BOOLEAN send;

    WdfWaitLockAcquire(ctx->Lock, NULL);
    send = (BOOLEAN)(ctx->JackCallback != NULL && ctx->Jack.Known);
    RkAdcJackInfo(ctx, &info);
    if (send) {
        ctx->Notifications++;
    }
    WdfWaitLockRelease(ctx->Lock);

    if (send) {
        RKADC_LOG("jack %s (%u uV), sequence %u",
                  info.Connected ? "plugged" : "unplugged", info.Microvolts, info.Sequence);
        ExNotifyCallback(ctx->JackCallback, RK3X_SARADC_JACK_STATE, &info);
    }
}

VOID
RkAdcJackWork(
    _In_ WDFWORKITEM WorkItem
    )
{
    RkAdcJackNotify(RkAdcGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem)));
}

static
VOID
RkAdcJackCallback(
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    PRKADC_CONTEXT ctx = (PRKADC_CONTEXT)CallbackContext;

    UNREFERENCED_PARAMETER(Argument2);

    if (ctx == NULL || Argument1 != RK3X_SARADC_JACK_QUERY) {
        return;
    }
    /* Any IRQL up to DISPATCH_LEVEL: only queue. */
    WdfWorkItemEnqueue(ctx->JackWork);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
RkAdcJackOpen(
    _In_ PRKADC_CONTEXT ctx
    )
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING name;
    NTSTATUS status;

    if (ctx->JackCallback != NULL) {
        return STATUS_SUCCESS;
    }
    RtlInitUnicodeString(&name, RK3X_SARADC_JACK_CALLBACK_NAME);
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
                               NULL, NULL);
    status = ExCreateCallback(&ctx->JackCallback, &oa, TRUE, TRUE);
    if (!NT_SUCCESS(status)) {
        ctx->JackCallback = NULL;
        return status;
    }
    ctx->JackCallbackReg = ExRegisterCallback(ctx->JackCallback, RkAdcJackCallback, ctx);
    if (ctx->JackCallbackReg == NULL) {
        ObDereferenceObject(ctx->JackCallback);
        ctx->JackCallback = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
RkAdcJackClose(
    _In_ PRKADC_CONTEXT ctx
    )
{
    PCALLBACK_OBJECT obj;

    if (ctx->JackCallbackReg != NULL) {
        ExUnregisterCallback(ctx->JackCallbackReg);
        ctx->JackCallbackReg = NULL;
    }
    /* A query that came in just before the unregister. */
    WdfWorkItemFlush(ctx->JackWork);

    WdfWaitLockAcquire(ctx->Lock, NULL);
    obj = ctx->JackCallback;
    ctx->JackCallback = NULL;
    WdfWaitLockRelease(ctx->Lock);
    if (obj != NULL) {
        ObDereferenceObject(obj);
    }
}
