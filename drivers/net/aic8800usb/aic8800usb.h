/*
 * aic8800usb.h - KMDF USB driver for the AICSemi AIC8800D80 Wi-Fi of the
 * Radxa ROCK 4D (and the Dragon Q6A), bring-up stages.
 *
 * One driver for both faces of the chip: the ROM loader (USB
 * VID_A69C&PID_8D80), which takes the firmware download and then leaves
 * the bus, and the running firmware (PID_8D81, whose interface 2 is the
 * WLAN function; interfaces 0 and 1 are a standard Bluetooth HCI that the
 * in-box BthUsb drives). Everything it learns goes to the device's
 * registry key (Device Parameters), flushed, so a board session can read
 * it afterwards; docs/dev/aic8800.md lists the values.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <ntstrsafe.h>
#include <bcrypt.h>

#include "aic.h"
#include "aic_proto.h"

#define AIC_POOL_TAG    'u8cA'

#define AIC_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "aic8800usb: " fmt "\n", __VA_ARGS__)

/*
 * Stage (DWORD under Device Parameters, set by the INF to 0):
 *   0  open: select the configuration, list the pipes, record the USB
 *      identity and speed; no transfer to the chip
 *   1  loader (PID_8D80): read the chip ID and download the firmware; the
 *      chip then comes back as PID_8D81. Runtime (PID_8D81): read the chip
 *      ID through the running firmware
 *   2  runtime: initialise the firmware (version, MAC, calibration), add a
 *      station interface and scan; results in ScanResults
 *   3  runtime: also join the network named by Ssid with the passphrase
 *      (Passphrase, replaced by Pmk on first use), run the WPA2 4-way
 *      handshake, install the keys, open the port, and keep the link
 *      (group rekeys, reconnect after a drop)
 */
#define AIC_STAGE_OPEN      0
#define AIC_STAGE_FIRMWARE  1
#define AIC_STAGE_SCAN      2
#define AIC_STAGE_CONNECT   3

#define AIC_DEFAULT_FW_DIR  L"\\SystemRoot\\System32\\drivers\\aic8800"
#define AIC_FW_MAX_BYTES    (1024u * 1024u)

#define AIC_RING_SLOTS      64
#define AIC_RING_SLOT_SIZE  4096        /* also the continuous reader's transfer length */
#define AIC_MAX_IN_PIPES    2
#define AIC_MAX_OUT_PIPES   2

typedef struct _AIC_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFUSBDEVICE UsbDevice;
    WDFUSBINTERFACE UsbInterface;
    WDFUSBPIPE InPipe[AIC_MAX_IN_PIPES];     /* data, then message (runtime) */
    WDFUSBPIPE OutPipe[AIC_MAX_OUT_PIPES];   /* data, then message (runtime) */
    ULONG InPipes, OutPipes;
    ULONG OutMaxPacket[AIC_MAX_OUT_PIPES];
    USHORT Vid, Pid;
    UCHAR InterfaceNumber;
    BOOLEAN Runtime;                /* PID_8D81 */
    BOOLEAN HighSpeed;
    BOOLEAN ReadersStarted;

    ULONG Stage;
    volatile LONG Stopping;
    LONG StepSeq;

    /* received transfers, from the continuous readers to the worker */
    KSPIN_LOCK RingLock;
    PUCHAR Ring;                    /* AIC_RING_SLOTS * AIC_RING_SLOT_SIZE */
    ULONG RingLen[AIC_RING_SLOTS];
    ULONG RingHead, RingTail;
    ULONG RingDrops;
    ULONG ReaderErrors;
    KEVENT RxEvent;

    /* the worker thread */
    PKTHREAD Worker;
    KEVENT StopEvent;

    /* bus statistics */
    ULONG Sends, SendErrors;
    NTSTATUS LastSendStatus;

    PUCHAR TxBuf;
    aic_dev Aic;
} AIC_DEVICE_CONTEXT, *PAIC_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIC_DEVICE_CONTEXT, AicGetContext)

/* usb.c */
extern const aic_bus_ops AicUsbOps;
NTSTATUS AicUsbPrepare(_In_ PAIC_DEVICE_CONTEXT Ctx);
NTSTATUS AicUsbStartReaders(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicUsbStopReaders(_In_ PAIC_DEVICE_CONTEXT Ctx);

/* files.c */
NTSTATUS AicLoadFile(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG MaxBytes,
                     _Out_ WDFMEMORY *Memory, _Out_ aic_file *File);

/* driver.c: registry, PASSIVE_LEVEL */
VOID AicCrumb(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCSTR Step);
VOID AicPutULong(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value);
VOID AicPutString(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value);
VOID AicPutBinary(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_reads_bytes_(Len) const VOID *Data,
                  _In_ ULONG Len);
ULONG AicGetULong(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default);
