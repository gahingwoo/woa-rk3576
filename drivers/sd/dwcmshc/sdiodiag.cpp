/*++

Copyright (c) 2026 Jiaxing Hu <gahing@gahingwoo.com>

SPDX-License-Identifier: GPL-2.0-only

Module Name:

    sdiodiag.cpp

Abstract:

    Registry breadcrumbs for the SDIO Wi-Fi bring-up (docs/WIFI.md, W2).

    Only the RK3576 SDIO controller (sdio: mmc@2a320000) records them; the SD
    slot never calls in here.  The counters are updated from the request,
    response, interrupt and bus-operation paths, at any IRQL up to DIRQL for
    the interlocked ones.  Updates are queued only at DISPATCH_LEVEL or
    below (MshcSdioDiagKick refuses above it).  A work item copies them to

        HKLM\SYSTEM\CurrentControlSet\Services\dwcmshc\SdioDiag

    At most one work item is in flight, and it waits a second before taking
    its snapshot, so a burst of commands costs one write.  The key is flushed
    to disk on the first write, at cleanup, and otherwise at most once every
    ten seconds: during boot the registry's lazy flush is held off, so every
    ZwFlushKey writes the SYSTEM hive out and blocks other writers to it
    (PnP, starting devices) while it does.  Version 2 flushed up to five
    times a second for as long as the card was busy.

Environment:

    Kernel mode only.

--*/

#include "precomp.h"
#pragma hdrstop

#include "trace.h"

#include "dwcmshc.h"

#define SDIO_DIAG_KEY_PATH  L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\dwcmshc\\SdioDiag"

//
// Bumped whenever the set of values changes, so a reading can be matched to
// the build that wrote it.
//
#define SDIO_DIAG_VERSION   3

//
// Coalescing delay of the update work item, and the shortest interval
// between two flushes of the key (100 ns units).
//
#define SDIO_DIAG_COALESCE_MS       1000
#define SDIO_DIAG_FLUSH_INTERVAL    (10LL * 1000LL * 10000LL)

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
SdioDiagSetUs(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ LONG64 Microseconds
    )
{
    //
    // A DWORD of microseconds runs out after 71 minutes; past that, say so.
    //
    SdioDiagSetDword(Key, Name, (Microseconds > 0xFFFFFFFFLL) ? 0xFFFFFFFFUL : (ULONG)Microseconds);
}

static
VOID
SdioDiagWrite(
    _In_ PMSHC_EXTENSION MshcExtension,
    _In_ BOOLEAN Flush
    )
{
    LONG64 Now = MSHC_DIAG_NOW();

    UNICODE_STRING KeyPath = RTL_CONSTANT_STRING(SDIO_DIAG_KEY_PATH);
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key = NULL;
    NTSTATUS Status;

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
    SdioDiagSetDword(Key, L"BusOps", (ULONG)MshcExtension->Diag.BusOps);
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
    SdioDiagSetDword(Key, L"ClockKhz", MshcExtension->Diag.ClockKhz);
    SdioDiagSetDword(Key, L"BusWidth", MshcExtension->Diag.BusWidth);
    SdioDiagSetDword(Key, L"CardClockHz", MshcExtension->RkCardClockHz);
    SdioDiagSetDword(Key, L"RefusedCompletions", (ULONG)MshcExtension->Diag.RefusedCompletions);
    SdioDiagSetDword(Key, L"LastRefusedCmd", MshcExtension->Diag.LastRefusedCmd);
    SdioDiagSetDword(Key, L"LastRefusedStatus", MshcExtension->Diag.LastRefusedStatus);
    SdioDiagSetDword(Key, L"LocalTimeouts", (ULONG)MshcExtension->Diag.LocalTimeouts);
    SdioDiagSetDword(Key, L"StaleLocalDpcs", (ULONG)MshcExtension->Diag.StaleLocalDpcs);
    SdioDiagSetDword(Key, L"DeferredKicks", (ULONG)MshcExtension->Diag.DeferredKicks);

    //
    // Version 3: timings (ms since boot) and the waits behind them.
    //
    SdioDiagSetDword(Key, L"SnapshotMs", MSHC_DIAG_MS(Now));
    SdioDiagSetDword(Key, L"InitMs", MshcExtension->Diag.InitMs);
    SdioDiagSetDword(Key, L"FirstCmdMs", MshcExtension->Diag.FirstCmdMs);
    SdioDiagSetDword(Key, L"LastCmdMs", MshcExtension->Diag.LastCmdMs);
    SdioDiagSetDword(Key, L"Cmd7Ms", MshcExtension->Diag.Cmd7Ms);
    SdioDiagSetDword(Key, L"Bus4BitMs", MshcExtension->Diag.Bus4BitMs);
    SdioDiagSetDword(Key, L"IorxReads", (ULONG)MshcExtension->Diag.IorxReads);
    SdioDiagSetDword(Key, L"IorxFirstMs", MshcExtension->Diag.IorxFirstMs);
    SdioDiagSetDword(Key, L"IorxLastMs", MshcExtension->Diag.IorxLastMs);
    SdioDiagSetDword(Key, L"IoexWrites", (ULONG)MshcExtension->Diag.IoexWrites);
    SdioDiagSetDword(Key, L"IoexLastMs", MshcExtension->Diag.IoexLastMs);
    SdioDiagSetDword(Key, L"LastIoexValue", MshcExtension->Diag.LastIoexValue);
    SdioDiagSetDword(Key, L"CmdTimeouts", (ULONG)MshcExtension->Diag.CmdTimeouts);
    SdioDiagSetDword(Key, L"DataTimeouts", (ULONG)MshcExtension->Diag.DataTimeouts);
    SdioDiagSetDword(Key, L"ResetsAll", (ULONG)MshcExtension->Diag.ResetsAll);
    SdioDiagSetDword(Key, L"ResetsCmd", (ULONG)MshcExtension->Diag.ResetsCmd);
    SdioDiagSetDword(Key, L"ResetsDat", (ULONG)MshcExtension->Diag.ResetsDat);
    SdioDiagSetDword(Key, L"AbandonedCmds", (ULONG)MshcExtension->Diag.AbandonedCmds);
    SdioDiagSetDword(Key, L"MaxAbandonedMs", MshcExtension->Diag.MaxAbandonedMs);
    SdioDiagSetUs(Key, L"TotalCmdUs", MshcExtension->Diag.TotalCmdUs);
    SdioDiagSetDword(Key, L"MaxCmdUs", MshcExtension->Diag.MaxCmdUs);
    SdioDiagSetDword(Key, L"MaxCmdIndex", MshcExtension->Diag.MaxCmdIndex);
    SdioDiagSetDword(Key, L"SlowCmds", (ULONG)MshcExtension->Diag.SlowCmds);
    SdioDiagSetDword(Key, L"LastSlowCmd", MshcExtension->Diag.LastSlowCmd);
    SdioDiagSetUs(Key, L"LocalWaitUs", MshcExtension->Diag.LocalWaitUs);
    SdioDiagSetUs(Key, L"BusyWaitUs", MshcExtension->Diag.BusyWaitUs);
    SdioDiagSetDword(Key, L"Writes", (ULONG)InterlockedIncrement(&MshcExtension->Diag.Writes));

    if (Flush || (Now - MshcExtension->Diag.LastFlushTime >= SDIO_DIAG_FLUSH_INTERVAL)) {
        MshcExtension->Diag.LastFlushTime = Now;
        SdioDiagSetDword(Key, L"Flushes", (ULONG)InterlockedIncrement(&MshcExtension->Diag.Flushes));
        (VOID)ZwFlushKey(Key);
    }

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
    // Coalesce: whatever happens in the next second goes into this write.
    //
    Delay.QuadPart = -(LONG64)SDIO_DIAG_COALESCE_MS * 10000LL;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    //
    // Clear the flag before the snapshot, so an update that lands while the
    // registry is being written queues the next one instead of being lost.
    //
    InterlockedExchange(&MshcExtension->Diag.FlushPending, 0);

    SdioDiagWrite(MshcExtension, FALSE);

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
    SdioDiagWrite(MshcExtension, TRUE);
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
    // then wait for one already queued to finish: it sleeps a second, so
    // three seconds is plenty.  If it somehow has not, leak the work item rather
    // than free it under a running worker.
    //
    MshcExtension->Diag.WorkItem = NULL;
    Delay.QuadPart = -50LL * 10000LL;
    for (Waits = 0; Waits < 60; Waits++) {
        if (InterlockedCompareExchange(&MshcExtension->Diag.WorkersActive, 0, 0) == 0) {
            break;
        }
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    SdioDiagWrite(MshcExtension, TRUE);

    if (InterlockedCompareExchange(&MshcExtension->Diag.WorkersActive, 0, 0) == 0) {
        IoFreeWorkItem(WorkItem);
    }
}

_Use_decl_annotations_
VOID
MshcDiagRequestDone(
    PMSHC_EXTENSION MshcExtension,
    PSDPORT_REQUEST Request
    )
{
    LONG64 Issued;
    ULONG64 Us;

    //
    // Called just before an sdport request goes back to sdport: issue to
    // completion, from the time MshcSlotIssueRequest stored.  A data
    // request's transfer phase comes back here too, after the command phase
    // took the time, and finds none.
    //
    Issued = InterlockedExchange64(&MshcExtension->Diag.CmdIssueTime, 0);
    if (Issued == 0) {
        return;
    }

    Us = MSHC_DIAG_US(MSHC_DIAG_NOW() - Issued);
    InterlockedAdd64(&MshcExtension->Diag.TotalCmdUs, (LONG64)Us);

    if (Us > MshcExtension->Diag.MaxCmdUs) {
        MshcExtension->Diag.MaxCmdUs = (Us > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (ULONG)Us;
        MshcExtension->Diag.MaxCmdIndex = Request->Command.Index;
    }

    if (Us >= 100000) {
        InterlockedIncrement(&MshcExtension->Diag.SlowCmds);
        MshcExtension->Diag.LastSlowCmd = Request->Command.Index;
    }
}
