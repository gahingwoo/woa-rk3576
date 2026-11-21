/*
 * hci.c - H4 framing, the BCM4345C5 bring-up (power, reset, patchram,
 * speed, address), and the BthX DDI the in-box BthMini talks to.
 *
 * The bring-up follows what Broadcom's UART controllers need and what the
 * Linux hci_bcm and btbcm drivers do, written here from that description
 * (no code taken):
 *
 *   1. BT_REG_ON and BT_WAKE high, 100 ms.
 *   2. At 115200: HCI_Reset, Read Local Version, Read Local Name.
 *   3. If the patchram file is there: Download Minidriver (0xFC2E), 50 ms,
 *      every record of the .hcd as an HCI command, each one waited for,
 *      250 ms after the final Launch RAM. The controller is back at 115200.
 *      HCI_Reset again, 100 ms.
 *   4. Update UART Baud Rate (0xFC18) to the operating speed, then the
 *      host side follows.
 *   5. Read BD_ADDR. If it is the BCM4345C5's placeholder address, write a
 *      per-board one (0xFC01): the registry value BdAddress if set, else a
 *      random one that is then stored there so it stays the same.
 *
 * Without the patchram the controller runs its ROM firmware. Basic HCI
 * works on it; Broadcom's ROM images have known bugs, which is why the
 * patchram exists, and the driver says so in the registry (FirmwareLoaded).
 *
 * The BthX DDI (bthxddi.h, documented by Microsoft as the Bluetooth
 * extensible transport) is implemented from that documentation. Microsoft's
 * SerialHCIBus sample (MS-PL, which cannot be combined with GPL code) was
 * read for the shape of the interface; none of its code is used here.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "bcm4345bt.h"

/* Packet types BthMini may write and may ask to read. */
#define BT_VALID_WRITE_TYPE(t)  ((ULONG)(t) == (ULONG)HciPacketCommand || (ULONG)(t) == (ULONG)HciPacketAclData)
#define BT_VALID_READ_TYPE(t)   ((ULONG)(t) == (ULONG)HciPacketEvent || (ULONG)(t) == (ULONG)HciPacketAclData)

/* btbcm.c's BDADDR_BCM4345C5, 43:45:C5:00:1F:AC, in HCI byte order. */
static const UCHAR BtPlaceholderAddr[6] = { 0xac, 0x1f, 0x00, 0xc5, 0x45, 0x43 };

/* ------------------------------------------------------------ packets */

static VOID
FifoInit(_Out_ BT_PACKET_FIFO *F, _In_ BT_PACKET_SLOT *Slots, _In_ ULONG Capacity)
{
    F->Head = 0;
    F->Count = 0;
    F->Capacity = Capacity;
    F->Dropped = 0;
    F->Slots = Slots;
}

/* PacketLock held. Keeps the newest packets: on overflow the oldest goes. */
static VOID
FifoPush(_Inout_ BT_PACKET_FIFO *F, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    BT_PACKET_SLOT *s;

    if (F->Count == F->Capacity) {
        F->Head = (F->Head + 1) % F->Capacity;
        F->Count--;
        F->Dropped++;
    }
    s = &F->Slots[(F->Head + F->Count) % F->Capacity];
    s->Length = Length;
    RtlCopyMemory(s->Data, Data, Length);
    F->Count++;
}

/* Bring-up: is this the answer to the command we are waiting for? */
static BOOLEAN
SyncMatch(_In_ PBT_DEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Ev, _In_ ULONG Length)
{
    USHORT op;

    if (Ctx->SyncOpcode == 0 || Length < 2) {
        return FALSE;
    }
    if (Ev[0] == HCI_EV_CMD_COMPLETE && Length >= 5) {
        op = (USHORT)(Ev[3] | (Ev[4] << 8));
    } else if (Ev[0] == HCI_EV_CMD_STATUS && Length >= 6) {
        op = (USHORT)(Ev[4] | (Ev[5] << 8));
    } else {
        return FALSE;
    }
    return op == Ctx->SyncOpcode;
}

/* PacketLock held. A whole packet, H4 type byte stripped. */
static VOID
DeliverPacket(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ UCHAR Type, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    if (Type == H4_EVENT && Ctx->BringUp) {
        if (SyncMatch(Ctx, Data, Length)) {
            ULONG n = min(Length, (ULONG)sizeof(Ctx->SyncReply));
            RtlCopyMemory(Ctx->SyncReply, Data, n);
            Ctx->SyncReplyLength = n;
            Ctx->SyncOpcode = 0;
            KeSetEvent(&Ctx->SyncEvent, IO_NO_INCREMENT, FALSE);
        }
        return;     /* nobody above us yet */
    }
    if (Type == H4_EVENT) {
        FifoPush(&Ctx->Events, Data, Length);
    } else if (Type == H4_ACL) {
        FifoPush(&Ctx->Acl, Data, Length);
    }
    /* SCO is not routed over HCI (ScoSupportNone); dropped if it comes. */
}

/* PacketLock held. Feeds the H4 state machine. */
VOID
BtParseBytes(_In_ PBT_DEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Bytes, _In_ ULONG Length)
{
    ULONG i = 0, take;

    while (i < Length) {
        switch (Ctx->RxState) {
        case RxType:
            Ctx->RxPacketType = Bytes[i++];
            Ctx->RxHave = 0;
            switch (Ctx->RxPacketType) {
            case H4_EVENT:  Ctx->RxNeed = 2; Ctx->RxState = RxHeader; break;
            case H4_ACL:    Ctx->RxNeed = 4; Ctx->RxState = RxHeader; break;
            case H4_SCO:    Ctx->RxNeed = 3; Ctx->RxState = RxHeader; break;
            default:        break;      /* lost sync: skip the byte */
            }
            break;

        case RxHeader:
        case RxPayload:
            take = min(Ctx->RxNeed - Ctx->RxHave, Length - i);
            RtlCopyMemory(Ctx->RxPacket + Ctx->RxHave, Bytes + i, take);
            Ctx->RxHave += take;
            i += take;
            if (Ctx->RxHave < Ctx->RxNeed) {
                break;
            }
            if (Ctx->RxState == RxHeader) {
                ULONG payload;
                if (Ctx->RxPacketType == H4_EVENT) {
                    payload = Ctx->RxPacket[1];
                } else if (Ctx->RxPacketType == H4_ACL) {
                    payload = Ctx->RxPacket[2] | (Ctx->RxPacket[3] << 8);
                } else {
                    payload = Ctx->RxPacket[2];
                }
                if (Ctx->RxNeed + payload > BT_MAX_FRAME) {
                    BT_LOG("RX: type %u length %lu too long, resync", Ctx->RxPacketType, payload);
                    Ctx->RxState = RxType;
                    break;
                }
                Ctx->RxNeed += payload;
                Ctx->RxState = RxPayload;
                if (payload != 0) {
                    break;
                }
            }
            DeliverPacket(Ctx, Ctx->RxPacketType, Ctx->RxPacket, Ctx->RxNeed);
            Ctx->RxState = RxType;
            break;
        }
    }
}

/* ------------------------------------------------------------- BthX I/O */

static NTSTATUS
CompleteRead(_In_ WDFREQUEST Request, _In_ UCHAR Type, _In_ const BT_PACKET_SLOT *Slot)
{
    PBTHX_HCI_READ_WRITE_CONTEXT out;
    size_t outLen;
    size_t need = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + Slot->Length;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, need, (PVOID *)&out, &outLen);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, 0);
        return status;
    }
    out->Type = Type;
    out->DataLen = Slot->Length;
    RtlCopyMemory(out->Data, Slot->Data, Slot->Length);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, need);
    return STATUS_SUCCESS;
}

/* Hand buffered packets to pended reads. IRQL <= DISPATCH_LEVEL. */
static VOID
ServiceOne(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ WDFQUEUE Queue, _In_ BT_PACKET_FIFO *F, _In_ UCHAR Type)
{
    WDFREQUEST req;

    for (;;) {
        WdfSpinLockAcquire(Ctx->PacketLock);
        if (F->Count == 0 || !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &req))) {
            WdfSpinLockRelease(Ctx->PacketLock);
            return;
        }
        /* Complete from the slot under the lock, then retire the slot. */
        (VOID)CompleteRead(req, Type, &F->Slots[F->Head]);
        F->Head = (F->Head + 1) % F->Capacity;
        F->Count--;
        WdfSpinLockRelease(Ctx->PacketLock);
    }
}

VOID
BtServiceReads(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    ServiceOne(Ctx, Ctx->EventReadQueue, &Ctx->Events, (UCHAR)HciPacketEvent);
    ServiceOne(Ctx, Ctx->AclReadQueue, &Ctx->Acl, (UCHAR)HciPacketAclData);
}

/* TxLock held. TRUE when the request is finished with (sent or failed). */
static BOOLEAN
TryWrite(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request)
{
    PBTHX_HCI_READ_WRITE_CONTEXT in;
    PULONG out;
    size_t inLen, outLen;
    const size_t hdr = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data);

    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, hdr, (PVOID *)&in, &inLen)) ||
        !NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID *)&out, &outLen)) ||
        inLen < hdr + (size_t)in->DataLen || in->DataLen > BT_MAX_FRAME ||
        !BT_VALID_WRITE_TYPE(in->Type)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return TRUE;
    }
    if (!UartQueueFrame(Ctx, in->Type, in->Data, in->DataLen)) {
        return FALSE;   /* ring full, try again when it drains */
    }
    *out = in->DataLen;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(ULONG));
    return TRUE;
}

/*
 * Every write goes through TxWaitQueue, in arrival order, and TxLock makes
 * taking one off the queue and putting it into the ring a single step, so
 * HCI commands and ACL packets reach the controller in the order BthMini
 * sent them. IRQL <= DISPATCH_LEVEL.
 */
VOID
BtServiceWrites(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    WDFREQUEST req;

    WdfSpinLockAcquire(Ctx->TxLock);
    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->TxWaitQueue, &req))) {
        if (!TryWrite(Ctx, req)) {
            if (!NT_SUCCESS(WdfRequestRequeue(req))) {
                WdfRequestComplete(req, STATUS_INSUFFICIENT_RESOURCES);
            }
            break;
        }
    }
    WdfSpinLockRelease(Ctx->TxLock);
}

VOID
BtEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                     _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PBT_DEVICE_CONTEXT ctx = BtGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PVOID buf;
    size_t len;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_BTHX_GET_VERSION:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_VERSION), &buf, &len);
        if (NT_SUCCESS(status)) {
            *(BTHX_VERSION *)buf = Microsoft_BTHX_DDI_Version;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_VERSION));
            return;
        }
        break;

    case IOCTL_BTHX_SET_VERSION:
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(BTHX_VERSION), &buf, &len);
        if (NT_SUCCESS(status)) {
            BT_LOG("BthMini speaks BthX version 0x%lx", ((BTHX_VERSION *)buf)->Version);
        }
        break;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_CAPABILITIES), &buf, &len);
        if (NT_SUCCESS(status)) {
            *(BTHX_CAPABILITIES *)buf = ctx->Caps;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_CAPABILITIES));
            return;
        }
        break;

    case IOCTL_BTHX_WRITE_HCI:
        if (!ctx->Ready) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        status = WdfRequestForwardToIoQueue(Request, ctx->TxWaitQueue);
        if (NT_SUCCESS(status)) {
            BtServiceWrites(ctx);
            return;
        }
        break;

    case IOCTL_BTHX_READ_HCI: {
        ULONG type;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &buf, &len);
        if (!NT_SUCCESS(status)) {
            break;
        }
        type = *(ULONG *)buf;
        if (!BT_VALID_READ_TYPE(type)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = WdfRequestForwardToIoQueue(Request,
                     type == (ULONG)HciPacketEvent ? ctx->EventReadQueue : ctx->AclReadQueue);
        if (NT_SUCCESS(status)) {
            BtServiceReads(ctx);
            return;
        }
        break;
    }

    default:
        break;
    }

    WdfRequestComplete(Request, status);
}

/* ------------------------------------------------------------- bring-up */

/*
 * One command, waited for. PASSIVE_LEVEL, bring-up only. Reply (the event
 * parameters, from the event code on) is copied to Reply when given.
 */
static NTSTATUS
HciCmd(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ USHORT Opcode,
       _In_reads_bytes_opt_(Length) const UCHAR *Params, _In_ UCHAR Length,
       _Out_writes_bytes_opt_(ReplyMax) UCHAR *Reply, _In_ ULONG ReplyMax, _Out_opt_ ULONG *ReplyLength)
{
    UCHAR frame[3 + 255];
    LARGE_INTEGER timeout;
    NTSTATUS status;
    ULONG n;

    frame[0] = (UCHAR)(Opcode & 0xFF);
    frame[1] = (UCHAR)(Opcode >> 8);
    frame[2] = Length;
    if (Length != 0 && Params != NULL) {
        RtlCopyMemory(frame + 3, Params, Length);
    }

    WdfSpinLockAcquire(Ctx->PacketLock);
    KeClearEvent(&Ctx->SyncEvent);
    Ctx->SyncReplyLength = 0;
    Ctx->SyncOpcode = Opcode;
    WdfSpinLockRelease(Ctx->PacketLock);

    if (!UartQueueFrame(Ctx, H4_COMMAND, frame, 3 + (ULONG)Length)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    timeout.QuadPart = -10000LL * BT_CMD_TIMEOUT_MS;
    status = KeWaitForSingleObject(&Ctx->SyncEvent, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        WdfSpinLockAcquire(Ctx->PacketLock);
        Ctx->SyncOpcode = 0;
        WdfSpinLockRelease(Ctx->PacketLock);
        BT_LOG("command 0x%04x: no answer in %u ms (line errors %lu, overflow %lu)",
               Opcode, BT_CMD_TIMEOUT_MS, Ctx->LineErrors, Ctx->RxOverflow);
        return STATUS_IO_TIMEOUT;
    }

    /* Command Complete: 0E len ncmd op op status...; Command Status: 0F len status ncmd op op */
    n = Ctx->SyncReplyLength;
    if (Ctx->SyncReply[0] == HCI_EV_CMD_COMPLETE) {
        if (n < 6 || Ctx->SyncReply[5] != 0) {
            BT_LOG("command 0x%04x: status 0x%02x", Opcode, n >= 6 ? Ctx->SyncReply[5] : 0xFF);
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
    } else if (Ctx->SyncReply[2] != 0) {
        BT_LOG("command 0x%04x: command status 0x%02x", Opcode, Ctx->SyncReply[2]);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (Reply != NULL) {
        ULONG c = min(n, ReplyMax);
        RtlCopyMemory(Reply, Ctx->SyncReply, c);
        if (ReplyLength != NULL) {
            *ReplyLength = c;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
HciReset(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    NTSTATUS status = HciCmd(Ctx, HCI_OP_RESET, NULL, 0, NULL, 0, NULL);

    if (NT_SUCCESS(status)) {
        BtDelayMs(100);
    }
    return status;
}

static VOID
LogVersion(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ PCWSTR Value)
{
    UCHAR r[16];
    ULONG n = 0;

    /* 0E 0C ncmd op op status hci_ver rev(2) lmp_ver manuf(2) lmp_subver(2) */
    if (NT_SUCCESS(HciCmd(Ctx, HCI_OP_READ_LOCAL_VERSION, NULL, 0, r, sizeof(r), &n)) && n >= 14) {
        ULONG rev = r[7] | (r[8] << 8);
        ULONG subver = r[12] | (r[13] << 8);
        BT_LOG("%ws: HCI %u rev 0x%04lx, LMP %u, manufacturer %u, subversion 0x%04lx",
               Value, r[6], rev, r[9], r[10] | (r[11] << 8), subver);
        BtRecordStatus(Ctx, Value, (rev << 16) | subver);
    }
}

static VOID
LogName(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    UCHAR r[2 + 4 + 248];
    ULONG n = 0;

    if (NT_SUCCESS(HciCmd(Ctx, HCI_OP_READ_LOCAL_NAME, NULL, 0, r, sizeof(r), &n)) && n > 6) {
        ULONG len = 0;
        while (6 + len < n && r[6 + len] != 0 && len < 64) {
            len++;
        }
        BT_LOG("local name \"%.*s\"", (int)len, (const char *)&r[6]);
        BtRecordString(Ctx, L"ChipName", (PCSTR)&r[6], len);
    }
}

static NTSTATUS
SetOperatingSpeed(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Baud)
{
    UCHAR p[6];
    NTSTATUS status;

    p[0] = 0;
    p[1] = 0;
    p[2] = (UCHAR)Baud;
    p[3] = (UCHAR)(Baud >> 8);
    p[4] = (UCHAR)(Baud >> 16);
    p[5] = (UCHAR)(Baud >> 24);
    status = HciCmd(Ctx, BCM_OP_UPDATE_BAUD, p, sizeof(p), NULL, 0, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    (VOID)UartWaitTxIdle(Ctx, 100);
    UartSetBaud(Ctx, Baud);
    BtDelayMs(10);
    return STATUS_SUCCESS;
}

/* Read the .hcd into paged memory. PASSIVE_LEVEL. */
static NTSTATUS
LoadFirmware(_In_ PBT_DEVICE_CONTEXT Ctx, _Out_ WDFMEMORY *Memory, _Out_ ULONG *Length)
{
    DECLARE_UNICODE_STRING_SIZE(path, 260);
    UNICODE_STRING valueName;
    WDFKEY key = NULL;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    HANDLE file = NULL;
    WDF_OBJECT_ATTRIBUTES attr;
    PVOID buf = NULL;
    NTSTATUS status;

    *Memory = NULL;
    *Length = 0;

    RtlUnicodeStringCopyString(&path, BT_DEFAULT_FIRMWARE);
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&valueName, L"FirmwarePath");
        if (!NT_SUCCESS(WdfRegistryQueryUnicodeString(key, &valueName, NULL, &path))) {
            RtlUnicodeStringCopyString(&path, BT_DEFAULT_FIRMWARE);
        }
        WdfRegistryClose(key);
    }

    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                          NULL, 0);
    if (!NT_SUCCESS(status)) {
        BT_LOG("patchram %wZ: open failed 0x%08X", &path, status);
        return status;
    }
    status = ZwQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status) &&
        (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > BT_FIRMWARE_MAX_BYTES)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (NT_SUCCESS(status)) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = Ctx->Device;
        status = WdfMemoryCreate(&attr, PagedPool, BT_POOL_TAG, (size_t)info.EndOfFile.QuadPart, Memory, &buf);
    }
    if (NT_SUCCESS(status)) {
        status = ZwReadFile(file, NULL, NULL, NULL, &iosb, buf, (ULONG)info.EndOfFile.QuadPart, NULL, NULL);
        if (NT_SUCCESS(status) && iosb.Information != (ULONG_PTR)info.EndOfFile.QuadPart) {
            status = STATUS_END_OF_FILE;
        }
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(*Memory);
            *Memory = NULL;
        } else {
            *Length = (ULONG)info.EndOfFile.QuadPart;
            BT_LOG("patchram %wZ: %lu bytes", &path, *Length);
        }
    }
    ZwClose(file);
    return status;
}

static NTSTATUS
DownloadPatchram(_In_ PBT_DEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Fw, _In_ ULONG Length)
{
    HCD_SUMMARY sum;
    HCD_RECORD rec;
    ULONG off = 0, done = 0;
    NTSTATUS status;
    int r = HcdValidate(Fw, Length, &sum);

    if (r != 0) {
        BT_LOG("patchram: not a valid .hcd (%d), not sent", r);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    BT_LOG("patchram: %lu records, %lu bytes of Write RAM, %lu other", sum.Records, sum.Bytes, sum.Other);

    status = HciCmd(Ctx, BCM_OP_DOWNLOAD_MINIDRV, NULL, 0, NULL, 0, NULL);
    if (!NT_SUCCESS(status)) {
        BT_LOG("Download Minidriver failed 0x%08X", status);
        return status;
    }
    BtDelayMs(50);

    while (HcdNext(Fw, Length, &off, &rec) == 1) {
        status = HciCmd(Ctx, rec.Opcode, rec.Params, rec.Length, NULL, 0, NULL);
        if (!NT_SUCCESS(status)) {
            BT_LOG("patchram record %lu (0x%04x) failed 0x%08X", done, rec.Opcode, status);
            return status;
        }
        done++;
    }
    BtDelayMs(250);
    BT_LOG("patchram: %lu records sent", done);
    return STATUS_SUCCESS;
}

static VOID
FixAddress(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    UCHAR r[16], addr[6];
    ULONG n = 0, len = 0, type = 0;
    UNICODE_STRING name;
    WDFKEY key = NULL;
    BOOLEAN have = FALSE;

    /* 0E 0A ncmd op op status addr[6] */
    if (!NT_SUCCESS(HciCmd(Ctx, HCI_OP_READ_BD_ADDR, NULL, 0, r, sizeof(r), &n)) || n < 12) {
        return;
    }
    BT_LOG("BD_ADDR %02X:%02X:%02X:%02X:%02X:%02X", r[11], r[10], r[9], r[8], r[7], r[6]);
    if (RtlCompareMemory(&r[6], BtPlaceholderAddr, 6) != 6) {
        BtRecordStatus(Ctx, L"AddressFromChip", 1);
        return;
    }

    RtlInitUnicodeString(&name, L"BdAddress");
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        if (NT_SUCCESS(WdfRegistryQueryValue(key, &name, sizeof(addr), addr, &len, &type)) &&
            type == REG_BINARY && len == sizeof(addr)) {
            have = TRUE;
        }
    }
    if (!have) {
        LARGE_INTEGER t = KeQueryPerformanceCounter(NULL);
        ULONG64 x = (ULONG64)t.QuadPart ^ KeQueryInterruptTime() ^ (ULONG64)(ULONG_PTR)Ctx;
        ULONG i;
        if (x == 0) {
            x = 0x9E3779B97F4A7C15ULL;
        }
        /* Not for secrets: only so two boards do not share an address. */
        for (i = 0; i < sizeof(addr); i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            addr[i] = (UCHAR)(x >> 24);
        }
        /* Stay clear of the reserved LAPs 0x9E8B00..0x9E8B3F. */
        if (addr[2] == 0x9E && addr[1] == 0x8B && addr[0] <= 0x3F) {
            addr[2] = 0x9D;
        }
        if (key != NULL) {
            (VOID)WdfRegistryAssignValue(key, &name, REG_BINARY, sizeof(addr), addr);
        }
    }
    if (key != NULL) {
        WdfRegistryClose(key);
    }

    /* HCI byte order: addr[0] is the least significant byte. */
    if (NT_SUCCESS(HciCmd(Ctx, BCM_OP_WRITE_BD_ADDR, addr, sizeof(addr), NULL, 0, NULL))) {
        BT_LOG("placeholder address replaced with %02X:%02X:%02X:%02X:%02X:%02X (%s)",
               addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], have ? "registry" : "new");
        BtRecordStatus(Ctx, L"AddressFromChip", 0);
    }
}

NTSTATUS
BtBringUp(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    WDFMEMORY fw = NULL;
    ULONG fwLen = 0;
    NTSTATUS status;
    ULONG attempt;

    Ctx->Ready = FALSE;
    WdfSpinLockAcquire(Ctx->PacketLock);
    Ctx->BringUp = TRUE;
    Ctx->RxState = RxType;
    WdfSpinLockRelease(Ctx->PacketLock);

    BtPower(Ctx, FALSE);
    BtDelayMs(20);
    UartFlush(Ctx);
    UartSetBaud(Ctx, BT_INIT_BAUD);
    BtPower(Ctx, TRUE);
    BtDelayMs(100);

    status = STATUS_IO_TIMEOUT;
    for (attempt = 0; attempt < 3 && !NT_SUCCESS(status); attempt++) {
        status = HciReset(Ctx);
    }
    BtRecordStatus(Ctx, L"ResetStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        BT_LOG("no answer to HCI_Reset at %u baud: module off, no 32 kHz clock, or UART wiring", BT_INIT_BAUD);
        goto out;
    }
    LogVersion(Ctx, L"RomVersion");
    LogName(Ctx);

    BtRecordStatus(Ctx, L"FirmwareLoaded", 0);
    if (NT_SUCCESS(LoadFirmware(Ctx, &fw, &fwLen))) {
        status = DownloadPatchram(Ctx, (const UCHAR *)WdfMemoryGetBuffer(fw, NULL), fwLen);
        WdfObjectDelete(fw);
        /* After Launch RAM, or after a failed download, it is at 115200 again. */
        UartFlush(Ctx);
        UartSetBaud(Ctx, BT_INIT_BAUD);
        if (NT_SUCCESS(status)) {
            status = HciReset(Ctx);
            BtRecordStatus(Ctx, L"FirmwareLoaded", NT_SUCCESS(status) ? 1 : 0);
        }
        if (!NT_SUCCESS(status)) {
            /* Start over on ROM firmware rather than leave a half-patched chip. */
            BT_LOG("patchram failed 0x%08X, falling back to the ROM firmware", status);
            BtPower(Ctx, FALSE);
            BtDelayMs(20);
            UartFlush(Ctx);
            BtPower(Ctx, TRUE);
            BtDelayMs(100);
            status = HciReset(Ctx);
            if (!NT_SUCCESS(status)) {
                goto out;
            }
        }
        LogVersion(Ctx, L"PatchedVersion");
        LogName(Ctx);
    } else {
        BT_LOG("no patchram: running the ROM firmware%s", "");
    }

    if (Ctx->OperBaud != BT_INIT_BAUD) {
        NTSTATUS s = SetOperatingSpeed(Ctx, Ctx->OperBaud);
        if (NT_SUCCESS(s)) {
            s = HciCmd(Ctx, HCI_OP_READ_LOCAL_VERSION, NULL, 0, NULL, 0, NULL);
        }
        if (!NT_SUCCESS(s)) {
            BT_LOG("%lu baud did not work (0x%08X), staying at %u", Ctx->OperBaud, s, BT_INIT_BAUD);
            UartSetBaud(Ctx, BT_INIT_BAUD);
            BtPower(Ctx, FALSE);
            BtDelayMs(20);
            UartFlush(Ctx);
            BtPower(Ctx, TRUE);
            BtDelayMs(100);
            status = HciReset(Ctx);
            if (!NT_SUCCESS(status)) {
                goto out;
            }
            BtRecordStatus(Ctx, L"FirmwareLoaded", 0);
            BtRecordStatus(Ctx, L"Baud", BT_INIT_BAUD);
        } else {
            BtRecordStatus(Ctx, L"Baud", Ctx->OperBaud);
        }
    }

    FixAddress(Ctx);
    status = STATUS_SUCCESS;

out:
    WdfSpinLockAcquire(Ctx->PacketLock);
    Ctx->BringUp = FALSE;
    Ctx->Events.Count = 0;
    Ctx->Acl.Count = 0;
    WdfSpinLockRelease(Ctx->PacketLock);
    BtRecordStatus(Ctx, L"BringUpStatus", (ULONG)status);
    if (NT_SUCCESS(status)) {
        Ctx->Ready = TRUE;
    } else {
        BtPower(Ctx, FALSE);
    }
    return status;
}

VOID
BtInitQueuesAndFifos(_In_ PBT_DEVICE_CONTEXT Ctx)
{
    FifoInit(&Ctx->Events, Ctx->EventSlots, BT_EVENT_SLOTS);
    FifoInit(&Ctx->Acl, Ctx->AclSlots, BT_ACL_SLOTS);
}
