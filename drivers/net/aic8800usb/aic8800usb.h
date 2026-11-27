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
#include <netadaptercx.h>
#include <net/virtualaddress.h>

EXTERN_C_START

#include "aic.h"
#include "aic_proto.h"

#define AIC_POOL_TAG    'u8cA'

struct _AIC_WIFI;                   /* wificx.cpp */

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
 *   4  runtime: also pass frames between the network adapter and the link,
 *      and report the link to Windows (DHCP, browsing)
 *
 * Two services run this one binary: aic8800ldr (aic8800ldr.inf, Class
 * System) for the loader, aic8800usb (aic8800usb.inf, Class Net, a
 * NetAdapterCx adapter) for the runtime. Settings for both are read from
 * Services\aic8800usb\Parameters.
 */
#define AIC_STAGE_OPEN      0
#define AIC_STAGE_FIRMWARE  1
#define AIC_STAGE_SCAN      2
#define AIC_STAGE_CONNECT   3
#define AIC_STAGE_ADAPTER   4

/*
 * Mode (DWORD under the runtime device's Device Parameters, written by its
 * INF; read once when the device is added):
 *   0  Ethernet-style adapter (aic8800usb.inf): the driver joins the
 *      network named by Ssid with its own WPA2 supplicant and Windows sees
 *      a wired-looking adapter
 *   1  WiFiCx (aic8800wifi.inf): Windows' own Wi-Fi (network list, its
 *      supplicant, profiles); the driver scans, associates and installs the
 *      keys Windows gives it, frames cross as native 802.11 data frames.
 *      Needs Stage 2 or more (the firmware init); Ssid and Passphrase are
 *      not used.
 * The two INFs bind the same hardware ID with the matching network
 * keywords (*MediaType 0 or 16), so the mode is chosen by which one is
 * installed; docs/dev/wificx.md has the switch.
 */
#define AIC_MODE_ETHERNET   0
#define AIC_MODE_WIFICX     1

#define AIC_PARAMS_PATH     L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\aic8800usb\\Parameters"

#define AIC_DEFAULT_FW_DIR  L"\\SystemRoot\\System32\\drivers\\aic8800"
#define AIC_FW_MAX_BYTES    (1024u * 1024u)

#define AIC_RING_SLOTS      64
#define AIC_RING_SLOT_SIZE  4096        /* also the continuous reader's transfer length */
#define AIC_MAX_IN_PIPES    2
#define AIC_MAX_OUT_PIPES   2
#define AIC_TX_SLOTS        32          /* USB writes in flight on the data pipe */
#define AIC_TX_SLOT_SIZE    2048

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
    ULONG Mode;                     /* AIC_MODE_*, from Device Parameters */
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

    /* the network adapter (runtime service only) */
    NETADAPTER Adapter;
    BOOLEAN FwReady;                /* runtime init done in PrepareHardware */
    BOOLEAN LinkReported;
    KSPIN_LOCK DataLock;            /* the queues, the FIFO, the TX slots */
    NETPACKETQUEUE TxQueue, RxQueue;
    BOOLEAN TxNotify, RxNotify;
    aic_frameq *RxQ;
    WDFREQUEST TxReq[AIC_TX_SLOTS];
    WDFMEMORY TxMem[AIC_TX_SLOTS];
    PUCHAR TxSlotBuf[AIC_TX_SLOTS];
    ULONG TxFreeMask;
    UCHAR TxGather[1600];           /* one packet's fragments, under the TX queue's serialisation */
    UCHAR TxEth[1600];              /* WiFiCx: the gathered 802.11 frame as Ethernet */
    ULONG TxFrames, TxDrops, TxErrors, RxFrames, RxDrops;
    BOOLEAN RxHold;                 /* WiFiCx: received frames wait in RxQ until the
                                       association is indicated to Windows */

    /* WiFiCx mode: the front end's state (wificx.cpp), from DeviceAdd on */
    struct _AIC_WIFI *Wifi;

    aic_dev Aic;
} AIC_DEVICE_CONTEXT, *PAIC_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIC_DEVICE_CONTEXT, AicGetContext)

typedef struct _AIC_ADAPTER_CONTEXT {
    WDFDEVICE Device;
} AIC_ADAPTER_CONTEXT, *PAIC_ADAPTER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIC_ADAPTER_CONTEXT, AicAdapterGetContext)
#define AicAdapterGetDevice(a) (&AicAdapterGetContext(a)->Device)

/* netadapter.c. In WiFiCx mode Init is the one WiFiCx hands to
 * EvtWifiDeviceCreateAdapter; in Ethernet mode NULL (allocated here). */
NTSTATUS AicNetCreateAdapter(_In_ PAIC_DEVICE_CONTEXT Ctx, _Inout_opt_ NETADAPTER_INIT *Init);
VOID AicNetRxRelease(_In_ PAIC_DEVICE_CONTEXT Ctx);
BOOLEAN AicNetTxIdle(_In_ PAIC_DEVICE_CONTEXT Ctx);
NTSTATUS AicNetStart(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicNetFree(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicNetUpdateLink(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicNetRxFrame(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_reads_(Len) const UCHAR *Frame, _In_ ULONG Len);

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
NTSTATUS AicOpenParams(_In_ ACCESS_MASK Access, _Out_ WDFKEY *Key);
VOID AicPutStats(_In_ PAIC_DEVICE_CONTEXT Ctx);

/* wificx.cpp: the WiFiCx front end (Mode 1) */
NTSTATUS AicWifiPreDeviceCreate(_Inout_ PWDFDEVICE_INIT DeviceInit);
NTSTATUS AicWifiPostDeviceCreate(_In_ PAIC_DEVICE_CONTEXT Ctx);
NTSTATUS AicWifiReportCapabilities(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicWifiRun(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicWifiScanBss(_In_ PAIC_DEVICE_CONTEXT Ctx, _In_ const aic_bss *Bss, _In_reads_(Len) const UCHAR *Body,
                    _In_ ULONG Len, _In_ int ProbeResp);
VOID AicWifiReleaseHardware(_In_ PAIC_DEVICE_CONTEXT Ctx);
/* Suspend (the worker has stopped): refuse and fail WDI requests; Restart: take them again. */
VOID AicWifiPause(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicWifiResume(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicWifiCleanup(_In_ PAIC_DEVICE_CONTEXT Ctx);

EXTERN_C_END
