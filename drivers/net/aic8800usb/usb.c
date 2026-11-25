/*
 * usb.c - the USB side: configuration, pipes, the continuous readers that
 * feed the core, and the core's bus operations (bulk OUT, wait, delay).
 *
 * Pipes, as AICSemi's driver picks them (aicwf_parse_usb): the first bulk
 * IN and OUT endpoints carry data; on the running firmware (PID_8D81) a
 * second bulk OUT takes the control messages and a second bulk IN brings
 * them back. The ROM loader (PID_8D80) uses only the first pair.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "aic8800usb.h"

static EVT_WDF_USB_READER_COMPLETION_ROUTINE AicEvtReadComplete;
static EVT_WDF_USB_READERS_FAILED AicEvtReadersFailed;

static PAIC_DEVICE_CONTEXT
PipeContext(_In_ WDFUSBPIPE Pipe)
{
    return AicGetContext(WdfIoTargetGetDevice(WdfUsbTargetPipeGetIoTarget(Pipe)));
}

static VOID
AicEvtReadComplete(_In_ WDFUSBPIPE Pipe, _In_ WDFMEMORY Buffer, _In_ size_t NumBytesTransferred,
                   _In_ WDFCONTEXT Context)
{
    PAIC_DEVICE_CONTEXT ctx = PipeContext(Pipe);
    PUCHAR data;
    KIRQL irql;
    ULONG next;

    UNREFERENCED_PARAMETER(Context);
    if (NumBytesTransferred == 0) {
        return;
    }
    data = (PUCHAR)WdfMemoryGetBuffer(Buffer, NULL);
    KeAcquireSpinLock(&ctx->RingLock, &irql);
    next = (ctx->RingHead + 1) % AIC_RING_SLOTS;
    if (next == ctx->RingTail || NumBytesTransferred > AIC_RING_SLOT_SIZE) {
        ctx->RingDrops++;
    } else {
        RtlCopyMemory(ctx->Ring + (SIZE_T)ctx->RingHead * AIC_RING_SLOT_SIZE, data, NumBytesTransferred);
        ctx->RingLen[ctx->RingHead] = (ULONG)NumBytesTransferred;
        ctx->RingHead = next;
    }
    KeReleaseSpinLock(&ctx->RingLock, irql);
    KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);
}

static BOOLEAN
AicEvtReadersFailed(_In_ WDFUSBPIPE Pipe, _In_ NTSTATUS Status, _In_ USBD_STATUS UsbdStatus)
{
    PAIC_DEVICE_CONTEXT ctx = PipeContext(Pipe);

    InterlockedIncrement((volatile LONG *)&ctx->ReaderErrors);
    AIC_LOG("reader failed 0x%08X usbd 0x%08X", Status, UsbdStatus);
    return TRUE;    /* let WDF reset the pipe and restart the reader */
}

NTSTATUS
AicUsbPrepare(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    WDF_USB_DEVICE_CREATE_CONFIG createCfg;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selCfg;
    WDF_USB_DEVICE_INFORMATION info;
    USB_DEVICE_DESCRIPTOR dd;
    USB_INTERFACE_DESCRIPTOR id;
    NTSTATUS status;
    UCHAR i, n;

    if (Ctx->UsbDevice == NULL) {
        WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createCfg, USBD_CLIENT_CONTRACT_VERSION_602);
        status = WdfUsbTargetDeviceCreateWithParameters(Ctx->Device, &createCfg, WDF_NO_OBJECT_ATTRIBUTES,
                                                        &Ctx->UsbDevice);
        if (!NT_SUCCESS(status)) {
            AIC_LOG("WdfUsbTargetDeviceCreateWithParameters 0x%08X", status);
            return status;
        }
    }
    WdfUsbTargetDeviceGetDeviceDescriptor(Ctx->UsbDevice, &dd);
    Ctx->Vid = dd.idVendor;
    Ctx->Pid = dd.idProduct;
    Ctx->Runtime = (dd.idProduct == AIC_USB_PID_D80_RUNTIME);

    WDF_USB_DEVICE_INFORMATION_INIT(&info);
    if (NT_SUCCESS(WdfUsbTargetDeviceRetrieveInformation(Ctx->UsbDevice, &info))) {
        Ctx->HighSpeed = (info.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) != 0;
    }

    /* For PID_8D81 this device is usbccgp's child for interface 2, which
     * it presents as a single-interface device. */
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&selCfg);
    status = WdfUsbTargetDeviceSelectConfig(Ctx->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &selCfg);
    if (!NT_SUCCESS(status)) {
        AIC_LOG("WdfUsbTargetDeviceSelectConfig 0x%08X", status);
        return status;
    }
    Ctx->UsbInterface = selCfg.Types.SingleInterface.ConfiguredUsbInterface;
    WdfUsbInterfaceGetDescriptor(Ctx->UsbInterface, 0, &id);
    Ctx->InterfaceNumber = id.bInterfaceNumber;

    Ctx->InPipes = Ctx->OutPipes = 0;
    n = WdfUsbInterfaceGetNumConfiguredPipes(Ctx->UsbInterface);
    for (i = 0; i < n; i++) {
        WDF_USB_PIPE_INFORMATION pi;
        WDFUSBPIPE pipe;

        WDF_USB_PIPE_INFORMATION_INIT(&pi);
        pipe = WdfUsbInterfaceGetConfiguredPipe(Ctx->UsbInterface, i, &pi);
        if (pi.PipeType != WdfUsbPipeTypeBulk) {
            continue;
        }
        if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
            if (Ctx->InPipes < AIC_MAX_IN_PIPES) {
                WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);
                Ctx->InPipe[Ctx->InPipes++] = pipe;
            }
        } else if (Ctx->OutPipes < AIC_MAX_OUT_PIPES) {
            Ctx->OutMaxPacket[Ctx->OutPipes] = pi.MaximumPacketSize;
            Ctx->OutPipe[Ctx->OutPipes++] = pipe;
        }
    }
    AicPutULong(Ctx, L"UsbVid", Ctx->Vid);
    AicPutULong(Ctx, L"UsbPid", Ctx->Pid);
    AicPutULong(Ctx, L"UsbInterface", Ctx->InterfaceNumber);
    AicPutULong(Ctx, L"UsbHighSpeed", Ctx->HighSpeed);
    AicPutULong(Ctx, L"UsbBulkIn", Ctx->InPipes);
    AicPutULong(Ctx, L"UsbBulkOut", Ctx->OutPipes);
    AicPutULong(Ctx, L"UsbOutMaxPacket", Ctx->OutPipes ? Ctx->OutMaxPacket[0] : 0);
    if (Ctx->InPipes == 0 || Ctx->OutPipes == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (i = 0; i < Ctx->InPipes; i++) {
        WDF_USB_CONTINUOUS_READER_CONFIG rc;

        WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&rc, AicEvtReadComplete, Ctx, AIC_RING_SLOT_SIZE);
        rc.NumPendingReads = 4;
        rc.EvtUsbTargetPipeReadersFailed = AicEvtReadersFailed;
        status = WdfUsbTargetPipeConfigContinuousReader(Ctx->InPipe[i], &rc);
        if (!NT_SUCCESS(status)) {
            AIC_LOG("continuous reader %u: 0x%08X", i, status);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
AicUsbStartReaders(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    for (i = 0; i < Ctx->InPipes && NT_SUCCESS(status); i++) {
        status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Ctx->InPipe[i]));
    }
    Ctx->ReadersStarted = NT_SUCCESS(status);
    return status;
}

VOID
AicUsbStopReaders(_In_ PAIC_DEVICE_CONTEXT Ctx)
{
    ULONG i;

    if (!Ctx->ReadersStarted) {
        return;
    }
    for (i = 0; i < Ctx->InPipes; i++) {
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Ctx->InPipe[i]), WdfIoTargetCancelSentIo);
    }
    Ctx->ReadersStarted = FALSE;
}

/* ------------------------------------------------------------ bus ops */

static NTSTATUS
Write(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ WDFUSBPIPE Pipe, _In_reads_bytes_opt_(Len) const UCHAR *Buf,
      _In_ ULONG Len)
{
    WDF_MEMORY_DESCRIPTOR md;
    WDF_REQUEST_SEND_OPTIONS opt;
    ULONG done = 0;

    WDF_REQUEST_SEND_OPTIONS_INIT(&opt, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opt, WDF_REL_TIMEOUT_IN_MS(2000));
    if (Len == 0) {
        return WdfUsbTargetPipeWriteSynchronously(Pipe, NULL, &opt, NULL, NULL);
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, (PVOID)Buf, Len);
    UNREFERENCED_PARAMETER(Ctx);
    return WdfUsbTargetPipeWriteSynchronously(Pipe, NULL, &opt, &md, &done);
}

static int
AicOpSend(void *C, int Msg, const u8 *Buf, u32 Len)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)C;
    ULONG p = (Msg && ctx->Runtime && ctx->OutPipes > 1) ? 1 : 0;
    NTSTATUS status;

    if (ctx->Stopping) {
        return AIC_E_IO;
    }
    ctx->Sends++;
    status = Write(ctx, ctx->OutPipe[p], Buf, Len);
    /* Messages go out with URB_ZERO_PACKET on Linux: end a transfer that
     * fills whole packets with an empty one. (Data frames get one more
     * byte from the core instead.) */
    if (NT_SUCCESS(status) && Msg && ctx->OutMaxPacket[p] && (Len % ctx->OutMaxPacket[p]) == 0) {
        status = Write(ctx, ctx->OutPipe[p], NULL, 0);
    }
    if (!NT_SUCCESS(status)) {
        ctx->SendErrors++;
        ctx->LastSendStatus = status;
        AIC_LOG("bulk OUT %u bytes on pipe %u: 0x%08X", Len, p, status);
        return AIC_E_IO;
    }
    return AIC_OK;
}

static void
AicOpWait(void *C, u32 Ms)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)C;
    LARGE_INTEGER t;
    KIRQL irql;

    t.QuadPart = -(LONGLONG)Ms * 10000;
    if (ctx->RingHead == ctx->RingTail) {
        (VOID)KeWaitForSingleObject(&ctx->RxEvent, Executive, KernelMode, FALSE, &t);
    }
    for (;;) {
        ULONG slot;

        KeAcquireSpinLock(&ctx->RingLock, &irql);
        if (ctx->RingHead == ctx->RingTail) {
            KeClearEvent(&ctx->RxEvent);
            KeReleaseSpinLock(&ctx->RingLock, irql);
            break;
        }
        slot = ctx->RingTail;
        KeReleaseSpinLock(&ctx->RingLock, irql);
        /* The slot is the worker's until RingTail moves on. */
        aic_rx(&ctx->Aic, ctx->Ring + (SIZE_T)slot * AIC_RING_SLOT_SIZE, ctx->RingLen[slot]);
        KeAcquireSpinLock(&ctx->RingLock, &irql);
        ctx->RingTail = (slot + 1) % AIC_RING_SLOTS;
        KeReleaseSpinLock(&ctx->RingLock, irql);
    }
}

static void
AicOpDelay(void *C, u32 Ms)
{
    LARGE_INTEGER t;

    UNREFERENCED_PARAMETER(C);
    t.QuadPart = -(LONGLONG)Ms * 10000;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static void
AicOpLog(void *C, const char *Msg)
{
    UNREFERENCED_PARAMETER(C);
    AIC_LOG("%s", Msg);
}

static void
AicOpRxEth(void *C, const u8 *Frame, u32 Len)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)C;

    if (ctx->Adapter != NULL) {
        AicNetRxFrame(ctx, Frame, Len);
    }
}

static void
AicOpScanBss(void *C, const aic_bss *Bss, const u8 *Body, u32 BodyLen, int ProbeResp)
{
    PAIC_DEVICE_CONTEXT ctx = (PAIC_DEVICE_CONTEXT)C;

    if (ctx->Mode == AIC_MODE_WIFICX) {
        AicWifiScanBss(ctx, Bss, Body, BodyLen, ProbeResp);
    }
}

const aic_bus_ops AicUsbOps = {
    AicOpSend,
    AicOpWait,
    AicOpDelay,
    AicOpLog,
    AicOpRxEth,
    AicOpScanBss,
};
