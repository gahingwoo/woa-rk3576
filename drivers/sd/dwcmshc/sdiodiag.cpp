/*++

Copyright (c) 2026 Jiaxing Hu <gahing@gahingwoo.com>

SPDX-License-Identifier: GPL-2.0-only

Module Name:

    sdiodiag.cpp

Abstract:

    Registry breadcrumbs.

    The RK3576 SDIO controller (sdio: mmc@2a320000) records the Wi-Fi
    bring-up counters (docs/WIFI.md, W2) and the transfer counters in

        HKLM\SYSTEM\CurrentControlSet\Services\dwcmshc\SdioDiag

    and flushes the key after each update, so the values survive a hang or a
    bugcheck on a full Windows install.  The SD slot records the bus setup
    and the transfer counters (DMA or PIO, request sizes, time on the
    controller) in

        HKLM\SYSTEM\CurrentControlSet\Services\dwcmshc\SdDiag

    at most once a second and without the flush, so a benchmark pays a few
    registry writes a second and no hive flushes.

    The counters are updated from the request, response, interrupt and
    bus-operation paths, at any IRQL up to DIRQL for the interlocked ones.
    Updates are queued only at DISPATCH_LEVEL or below (MshcSdioDiagKick
    refuses above it).  At most one work item is in flight, and it waits
    before taking its snapshot, so a burst of commands costs one write.

Environment:

    Kernel mode only.

--*/

#include "precomp.h"
#pragma hdrstop

#include "trace.h"

#include "dwcmshc.h"

#define SDIO_DIAG_KEY_PATH  L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\dwcmshc\\SdioDiag"
#define SD_DIAG_KEY_PATH    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\dwcmshc\\SdDiag"

//
// Bumped whenever the set of values changes, so a reading can be matched to
// the build that wrote it.
//
#define SDIO_DIAG_VERSION   3

static
VOID
SdioDiagSetQword(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG64 Value
    )
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    (VOID)ZwSetValueKey(Key, &ValueName, 0, REG_QWORD, &Value, sizeof(Value));
}

static
VOID
SdioDiagSetDword(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Value
    )
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    (VOID)ZwSetValueKey(Key, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
}

static
VOID
SdioDiagWrite(
    _In_ PMSHC_EXTENSION MshcExtension
    )
{
    UNICODE_STRING KeyPath;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key = NULL;
    NTSTATUS Status;

    RtlInitUnicodeString(&KeyPath, MshcExtension->IsSdio ? SDIO_DIAG_KEY_PATH : SD_DIAG_KEY_PATH);

    InitializeObjectAttributes(
        &Attributes,
        &KeyPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    Status = ZwCreateKey(&Key, KEY_WRITE, &Attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(Status)) {
        return;
    }

    SdioDiagSetDword(Key, L"Version", SDIO_DIAG_VERSION);
    SdioDiagSetDword(Key, L"PhysicalBase", (ULONG)(ULONG_PTR)MshcExtension->PhysicalAddress);
    SdioDiagSetDword(Key, L"CoreVersion", MshcExtension->CoreVersion);

    //
    // Both controllers: the bus and the data transfers.  BusSpeed and
    // BusWidth are SDPORT_BUS_SPEED / SDPORT_BUS_WIDTH values.
    //
    SdioDiagSetDword(Key, L"ClockKhz", MshcExtension->Diag.ClockKhz);
    SdioDiagSetDword(Key, L"CardClockHz", MshcExtension->RkCardClockHz);
    SdioDiagSetDword(Key, L"BusWidth", MshcExtension->Diag.BusWidth);
    SdioDiagSetDword(Key, L"BusSpeed", (ULONG)MshcExtension->BusSpeed);
    SdioDiagSetDword(Key, L"BusOps", (ULONG)MshcExtension->Diag.BusOps);
    SdioDiagSetDword(Key, L"DmaRequests", (ULONG)MshcExtension->Xfer.DmaRequests);
    SdioDiagSetQword(Key, L"DmaBytes", (ULONG64)MshcExtension->Xfer.DmaBytes);
    SdioDiagSetDword(Key, L"PioRequests", (ULONG)MshcExtension->Xfer.PioRequests);
    SdioDiagSetQword(Key, L"PioBytes", (ULONG64)MshcExtension->Xfer.PioBytes);
    SdioDiagSetDword(Key, L"MaxDmaLength", (ULONG)MshcExtension->Xfer.MaxDmaLength);
    SdioDiagSetDword(Key, L"MaxSgElements", (ULONG)MshcExtension->Xfer.MaxSgElements);
    SdioDiagSetDword(Key, L"MaxDescriptors", (ULONG)MshcExtension->Xfer.MaxDescriptors);
    SdioDiagSetDword(Key, L"BuildErrors", (ULONG)MshcExtension->Xfer.BuildErrors);
    SdioDiagSetDword(Key, L"LastBuildError", MshcExtension->Xfer.LastBuildError);
    SdioDiagSetDword(Key, L"IdmacErrors", (ULONG)MshcExtension->Xfer.IdmacErrors);
    SdioDiagSetDword(Key, L"LastIdmacIdsts", MshcExtension->Xfer.LastIdmacIdsts);
    SdioDiagSetDword(Key, L"OwnWaits", (ULONG)MshcExtension->Xfer.OwnWaits);
    SdioDiagSetDword(Key, L"OwnTimeouts", (ULONG)MshcExtension->Xfer.OwnTimeouts);
    SdioDiagSetDword(Key, L"XferErrors", (ULONG)MshcExtension->Xfer.DataErrors);
    SdioDiagSetQword(
        Key,
        L"DmaMicroseconds",
        (MshcExtension->Xfer.TickFrequency > 0) ?
            (ULONG64)(MshcExtension->Xfer.DmaTicks / MshcExtension->Xfer.TickFrequency) * 1000000ULL +
            (ULONG64)(MshcExtension->Xfer.DmaTicks % MshcExtension->Xfer.TickFrequency) * 1000000ULL /
                (ULONG64)MshcExtension->Xfer.TickFrequency :
            0);

    if (!MshcExtension->IsSdio) {
        ZwClose(Key);
        return;
    }

    SdioDiagSetDword(Key, L"LastBusOpType", MshcExtension->Diag.LastBusOpType);
    SdioDiagSetDword(Key, L"CardDetectCalls", (ULONG)MshcExtension->Diag.CardDetectCalls);
    SdioDiagSetDword(Key, L"Commands", (ULONG)MshcExtension->Diag.Commands);
    SdioDiagSetDword(Key, L"Cmd5Count", (ULONG)MshcExtension->Diag.Cmd5Count);
    SdioDiagSetDword(Key, L"Cmd5Resp", MshcExtension->Diag.Cmd5Resp);
    SdioDiagSetDword(Key, L"Cmd3Rca", MshcExtension->Diag.Cmd3Rca);
    SdioDiagSetDword(Key, L"Cmd52Count", (ULONG)MshcExtension->Diag.Cmd52Count);
    SdioDiagSetDword(Key, L"Cmd53Count", (ULONG)MshcExtension->Diag.Cmd53Count);
    SdioDiagSetDword(Key, L"LastCmd", MshcExtension->Diag.LastCmd);
    SdioDiagSetDword(Key, L"LastArg", MshcExtension->Diag.LastArg);
    SdioDiagSetDword(Key, L"LastResp0", MshcExtension->Diag.LastResp0);
    SdioDiagSetDword(Key, L"CmdErrors", (ULONG)MshcExtension->Diag.CmdErrors);
    SdioDiagSetDword(Key, L"DataErrors", (ULONG)MshcExtension->Diag.DataErrors);
    SdioDiagSetDword(Key, L"LastErrorRintsts", MshcExtension->Diag.LastErrorRintsts);
    SdioDiagSetDword(Key, L"SdioInterrupts", (ULONG)MshcExtension->Diag.SdioInterrupts);
    SdioDiagSetDword(Key, L"CardIntEnables", (ULONG)MshcExtension->Diag.CardIntEnables);
    SdioDiagSetDword(Key, L"CardIntDisables", (ULONG)MshcExtension->Diag.CardIntDisables);
    SdioDiagSetDword(Key, L"RefusedCompletions", (ULONG)MshcExtension->Diag.RefusedCompletions);
    SdioDiagSetDword(Key, L"LastRefusedCmd", MshcExtension->Diag.LastRefusedCmd);
    SdioDiagSetDword(Key, L"LastRefusedStatus", MshcExtension->Diag.LastRefusedStatus);
    SdioDiagSetDword(Key, L"LocalTimeouts", (ULONG)MshcExtension->Diag.LocalTimeouts);
    SdioDiagSetDword(Key, L"StaleLocalDpcs", (ULONG)MshcExtension->Diag.StaleLocalDpcs);
    SdioDiagSetDword(Key, L"DeferredKicks", (ULONG)MshcExtension->Diag.DeferredKicks);

    (VOID)ZwFlushKey(Key);
    ZwClose(Key);
}

static IO_WORKITEM_ROUTINE SdioDiagWorker;

static
VOID
SdioDiagWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context
    )
{
    PMSHC_EXTENSION MshcExtension = (PMSHC_EXTENSION)Context;
    LARGE_INTEGER Delay;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (MshcExtension == NULL) {
        return;
    }

    //
    // Coalesce: whatever happens in the next 200 ms (SDIO) or second (SD
    // slot) goes into this write.
    //
    Delay.QuadPart = (MshcExtension->IsSdio ? -200LL : -1000LL) * 10000LL;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    //
    // Clear the flag before the snapshot, so an update that lands while the
    // registry is being written queues the next one instead of being lost.
    //
    InterlockedExchange(&MshcExtension->Diag.FlushPending, 0);

    SdioDiagWrite(MshcExtension);

    //
    // Last touch of the extension: cleanup waits for this before it frees
    // the work item.
    //
    InterlockedDecrement(&MshcExtension->Diag.WorkersActive);
}

_Use_decl_annotations_
VOID
MshcSdioDiagInit(
    PMSHC_EXTENSION MshcExtension,
    PDEVICE_OBJECT MiniportFdo
    )
{
    if (gCrashdumpMode || (MiniportFdo == NULL)) {
        return;
    }

    MshcExtension->Diag.WorkItem = IoAllocateWorkItem(MiniportFdo);
    MshcExtension->Diag.FlushPending = 0;

    //
    // First write straight away: the key existing at all says the driver
    // bound the SDIO controller.
    //
    SdioDiagWrite(MshcExtension);
}

_Use_decl_annotations_
VOID
MshcSdioDiagKick(
    PMSHC_EXTENSION MshcExtension
    )
{
    if (gCrashdumpMode || (MshcExtension->Diag.WorkItem == NULL)) {
        return;
    }

    //
    // IoQueueWorkItem is DISPATCH_LEVEL at most.  sdport calls some
    // miniport callbacks (ToggleEvents, from its ISR and through
    // KeSynchronizeExecution) at DIRQL; the counters they bump are written
    // by the next update from a lower IRQL.
    //
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        InterlockedIncrement(&MshcExtension->Diag.DeferredKicks);
        return;
    }

    if (InterlockedCompareExchange(&MshcExtension->Diag.FlushPending, 1, 0) != 0) {
        return;
    }

    InterlockedIncrement(&MshcExtension->Diag.WorkersActive);
    IoQueueWorkItem(
        MshcExtension->Diag.WorkItem,
        SdioDiagWorker,
        DelayedWorkQueue,
        MshcExtension);
}

_Use_decl_annotations_
VOID
MshcSdioDiagCleanup(
    PMSHC_EXTENSION MshcExtension
    )
{
    PIO_WORKITEM WorkItem;
    LARGE_INTEGER Delay;
    ULONG Waits;

    WorkItem = MshcExtension->Diag.WorkItem;
    if (WorkItem == NULL) {
        return;
    }

    //
    // Stop new work items (sdport has stopped calling the miniport by now),
    // then wait for one already queued to finish: it sleeps a second at most, so two
    // seconds is plenty.  If it somehow has not, leak the work item rather
    // than free it under a running worker.
    //
    MshcExtension->Diag.WorkItem = NULL;
    Delay.QuadPart = -50LL * 10000LL;
    for (Waits = 0; Waits < 40; Waits++) {
        if (InterlockedCompareExchange(&MshcExtension->Diag.WorkersActive, 0, 0) == 0) {
            break;
        }
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    SdioDiagWrite(MshcExtension);

    if (InterlockedCompareExchange(&MshcExtension->Diag.WorkersActive, 0, 0) == 0) {
        IoFreeWorkItem(WorkItem);
    }
}
