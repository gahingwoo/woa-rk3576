/*
 * sdbus.c - CMD52 and CMD53 through the SD bus driver (sdbus.sys), the
 * documented SDIO function-driver interface in ntddsd.h: SdBusOpenInterface
 * once, then SdBusSubmitRequest with SDRF_DEVICE_COMMAND for every command
 * and SDRF_GET/SET_PROPERTY for the bus driver's settings.
 *
 * Every call here is synchronous and at PASSIVE_LEVEL, from the bring-up
 * work item. These are the bwl_bus_ops the portable core runs on.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* sdbus.lib refers to GUID_SDBUS_INTERFACE_STANDARD; define it here, once. */
#include <ntddk.h>
#include <initguid.h>
#include <ntddsd.h>

#include "bcmwlsdio.h"

/*
 * R5 response, bits 15:8: COM_CRC_ERROR 0x80, ILLEGAL_COMMAND 0x40,
 * IO_CURRENT_STATE 0x30, ERROR 0x08, FUNCTION_NUMBER 0x02, OUT_OF_RANGE 0x01.
 */
#define R5_ERRORS   0xCBu

NTSTATUS
BwlSdOpen(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    SDBUS_INTERFACE_PARAMETERS p;
    NTSTATUS status;

    RtlZeroMemory(&Ctx->Sd, sizeof(Ctx->Sd));
    /* the WDI miniport keeps the device objects NDIS handed it */
    status = SdBusOpenInterface(Ctx->Pdo != NULL ? Ctx->Pdo : WdfDeviceWdmGetPhysicalDevice(Ctx->Device), &Ctx->Sd,
                                sizeof(SDBUS_INTERFACE_STANDARD), SDBUS_INTERFACE_VERSION);
    if (!NT_SUCCESS(status)) {
        BWL_LOG("SdBusOpenInterface failed 0x%08X", status);
        return status;
    }
    Ctx->SdOpen = TRUE;

    RtlZeroMemory(&p, sizeof(p));
    p.Size = sizeof(p);
    p.TargetObject = Ctx->NextDevice != NULL ? Ctx->NextDevice : WdfDeviceWdmGetAttachedDevice(Ctx->Device);
    /*
     * W3 polls. No card interrupt callback yet, and the driver sequences
     * the function enables itself (F2 must stay off until the firmware
     * runs), so sdbus is told to leave IOEx alone.
     */
    p.DeviceGeneratesInterrupts = FALSE;
    p.CallbackAtDpcLevel = FALSE;
    p.CallbackRoutine = NULL;
    p.CallbackRoutineContext = Ctx;
#ifdef SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE
    p.SdioFlags = SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE;
#endif
    status = Ctx->Sd.InitializeInterface(Ctx->Sd.Context, &p);
    if (!NT_SUCCESS(status)) {
        BWL_LOG("SDBUS InitializeInterface failed 0x%08X", status);
        BwlSdClose(Ctx);
    }
    return status;
}

VOID
BwlSdClose(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    if (Ctx->SdOpen) {
        Ctx->SdOpen = FALSE;
        if (Ctx->Sd.InterfaceDereference != NULL) {
            Ctx->Sd.InterfaceDereference(Ctx->Sd.Context);
        }
    }
}

static NTSTATUS
Property(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SD_REQUEST_FUNCTION Fn, _In_ SDBUS_PROPERTY Prop,
         _Inout_updates_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    SDBUS_REQUEST_PACKET pkt;

    if (!Ctx->SdOpen) {
        return STATUS_DEVICE_NOT_READY;
    }
    RtlZeroMemory(&pkt, sizeof(pkt));
    pkt.RequestFunction = Fn;
    pkt.Parameters.GetSetProperty.Property = Prop;
    pkt.Parameters.GetSetProperty.Buffer = Buffer;
    pkt.Parameters.GetSetProperty.Length = Length;
    return SdBusSubmitRequest(Ctx->Sd.Context, &pkt);
}

NTSTATUS
BwlSdGetProperty(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SDBUS_PROPERTY Prop,
                 _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    RtlZeroMemory(Buffer, Length);
    return Property(Ctx, SDRF_GET_PROPERTY, Prop, Buffer, Length);
}

NTSTATUS
BwlSdSetProperty(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SDBUS_PROPERTY Prop,
                 _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    return Property(Ctx, SDRF_SET_PROPERTY, Prop, Buffer, Length);
}

static BOOLEAN
IoAllowed(_In_ PBWL_DEVICE_CONTEXT Ctx)
{
    return Ctx->SdOpen && !Ctx->Gone && (Ctx->Stopping == 0 || Ctx->Stopping == 2);
}

/* CMD52: R/W 31, function 30:28, RAW 27, address 25:9, data 7:0 */
static int
SdCmd52(void *C, unsigned Fn, u32 Addr, int Write, u8 *Val)
{
    static const SDCMD_DESCRIPTOR rd = { SDCMD_IO_RW_DIRECT, SDCC_STANDARD, SDTD_READ, SDTT_CMD_ONLY, SDRT_5 };
    static const SDCMD_DESCRIPTOR wr = { SDCMD_IO_RW_DIRECT, SDCC_STANDARD, SDTD_WRITE, SDTT_CMD_ONLY, SDRT_5 };
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)C;
    SDBUS_REQUEST_PACKET pkt;
    ULONG arg;
    NTSTATUS status;

    if (!IoAllowed(ctx)) {
        return -1;
    }
    arg = ((ULONG)(Fn & 7) << 28) | ((Addr & 0x1FFFF) << 9);
    if (Write) {
        arg |= (1UL << 31) | *Val;
    }
    RtlZeroMemory(&pkt, sizeof(pkt));
    pkt.RequestFunction = SDRF_DEVICE_COMMAND;
    pkt.Parameters.DeviceCommand.CmdDesc = Write ? wr : rd;
    pkt.Parameters.DeviceCommand.Argument = arg;
    status = SdBusSubmitRequest(ctx->Sd.Context, &pkt);
    ctx->Cmd52++;
    ctx->LastSdArg = arg;
    if (!NT_SUCCESS(status)) {
        ctx->Cmd52Errors++;
        ctx->LastSdStatus = status;
        return -1;
    }
    /* The R5 response: data in bits 7:0, flags in 15:8. */
    if (pkt.ResponseData.AsUCHAR[1] & R5_ERRORS) {
        ctx->Cmd52Errors++;
        ctx->LastR5Flags = pkt.ResponseData.AsUCHAR[1];
        return -1;
    }
    if (!Write) {
        *Val = pkt.ResponseData.AsUCHAR[0];
    }
    return 0;
}

/* CMD53: R/W 31, function 30:28, block mode 27, incrementing 26, address 25:9, count 8:0 */
static int
SdCmd53(void *C, unsigned Fn, u32 Addr, int Incr, int Write, int Block, u8 *Buf, u32 Len)
{
    PBWL_DEVICE_CONTEXT ctx = (PBWL_DEVICE_CONTEXT)C;
    SDBUS_REQUEST_PACKET pkt;
    SDCMD_DESCRIPTOR desc;
    ULONG arg, count, bs;
    PMDL mdl;
    NTSTATUS status;

    if (!IoAllowed(ctx) || Len == 0) {
        return -1;
    }
    /*
     * The core passes the bounce buffer, or a part of it, and nothing else
     * (bwl.h). Anything else would let the SD host DMA into memory this
     * driver does not own: refuse it and count it.
     */
    if (ctx->Bounce == NULL || Buf < ctx->Bounce || Len > BWL_BOUNCE_MIN ||
        (SIZE_T)(Buf - ctx->Bounce) > BWL_BOUNCE_MIN - Len) {
        ctx->Cmd53OutsideBounce++;
        ctx->Cmd53Errors++;
        return -1;
    }
    bs = Fn == 2 ? ctx->Bwl.F2BlockSize : ctx->Bwl.F1BlockSize;
    if (Block) {
        if (bs == 0 || Len % bs != 0 || Len / bs > 511) {
            return -1;
        }
        count = Len / bs;
    } else {
        if (Len > 512) {
            return -1;
        }
        count = Len == 512 ? 0 : Len;
    }
    arg = ((ULONG)(Fn & 7) << 28) | ((Addr & 0x1FFFF) << 9) | (count & 0x1FF);
    if (Write) {
        arg |= 1UL << 31;
    }
    if (Block) {
        arg |= 1UL << 27;
    }
    if (Incr) {
        arg |= 1UL << 26;
    }

    mdl = IoAllocateMdl(Buf, Len, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        ctx->Cmd53Errors++;
        ctx->LastSdStatus = STATUS_INSUFFICIENT_RESOURCES;
        return -1;
    }
    MmBuildMdlForNonPagedPool(mdl);

    desc.Cmd = SDCMD_IO_RW_EXTENDED;
    desc.CmdClass = SDCC_STANDARD;
    desc.TransferDirection = Write ? SDTD_WRITE : SDTD_READ;
    desc.TransferType = Block ? SDTT_MULTI_BLOCK_NO_CMD12 : SDTT_SINGLE_BLOCK;
    desc.ResponseType = SDRT_5;

    RtlZeroMemory(&pkt, sizeof(pkt));
    pkt.RequestFunction = SDRF_DEVICE_COMMAND;
    pkt.Parameters.DeviceCommand.CmdDesc = desc;
    pkt.Parameters.DeviceCommand.Argument = arg;
    pkt.Parameters.DeviceCommand.Mdl = mdl;
    pkt.Parameters.DeviceCommand.Length = Len;
    status = SdBusSubmitRequest(ctx->Sd.Context, &pkt);
    IoFreeMdl(mdl);

    ctx->Cmd53++;
    ctx->LastSdArg = arg;
    if (!NT_SUCCESS(status)) {
        ctx->Cmd53Errors++;
        ctx->LastSdStatus = status;
        return -1;
    }
    if (pkt.ResponseData.AsUCHAR[1] & R5_ERRORS) {
        ctx->Cmd53Errors++;
        ctx->LastR5Flags = pkt.ResponseData.AsUCHAR[1];
        return -1;
    }
    return 0;
}

static void
SdDelay(void *C, u32 Us)
{
    LARGE_INTEGER t;

    UNREFERENCED_PARAMETER(C);
    if (Us < 100) {
        KeStallExecutionProcessor(Us);
        return;
    }
    /* The bring-up raises the timer resolution to 1 ms while it runs. */
    t.QuadPart = -10LL * (LONGLONG)Us;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static void
SdLog(void *C, const char *Msg)
{
    UNREFERENCED_PARAMETER(C);
    BWL_LOG("%s", Msg);
}

const bwl_bus_ops BwlSdOps = {
    SdCmd52,
    SdCmd53,
    SdDelay,
    SdLog,
};
