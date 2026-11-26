/*
 * bcmwlsdio.h - KMDF function driver for the Broadcom BCM43456 Wi-Fi
 * (AMPAK AP6256, TaishanPi 3M) and BCM43752 Wi-Fi (BW3752-50B1, ArmSoM
 * CM5) on the RK3576 SDIO controller.
 *
 * Stages 0 to 2 (W3 of docs/dev/wifi.md): open the SD bus driver's SDBUS
 * interface, talk CMD52/CMD53 to the card, identify the chip, download the
 * firmware, NVRAM and CLM blob, start the firmware and read its version.
 * Stage 3 (W4): a WiFiCx 1.0 Wi-Fi adapter for Windows' own Wi-Fi
 * (wificx.cpp, netadapter.c), when installed with bcmwlwifi.inf. Everything
 * it learns goes to the device's registry key (Device Parameters), flushed,
 * so a board session can read it afterwards.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <ntddsd.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>

EXTERN_C_START

#include "bwl.h"

struct _BWL_WIFI;                   /* wificx.cpp */

#define BWL_POOL_TAG    'lWcB'

#define BWL_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "bcmwlsdio: " fmt "\n", __VA_ARGS__)

/*
 * Stage (DWORD under Device Parameters, set by both INFs to 0, kept across
 * updates):
 *   0  open the SDBUS interface and record what the bus driver says; no
 *      access to the card
 *   1  also identify: CCCR, chip ID, cores, RAM size (bwfm's attach)
 *   2  also download the firmware and start it, then read its version
 *   3  also the Wi-Fi adapter: firmware set-up, then WiFiCx (scan, connect,
 *      keys, data). Only on function 1, and only with bcmwlwifi.inf (Class
 *      Net); under bcmwlsdio.inf (Class System) 3 runs as 2. Read when the
 *      device is added, so a change needs a restart of the device.
 */
#define BWL_STAGE_OPEN      0
#define BWL_STAGE_IDENTIFY  1
#define BWL_STAGE_FIRMWARE  2
#define BWL_STAGE_NETWORK   3

#define BWL_DEFAULT_FW_DIR  L"\\SystemRoot\\System32\\drivers\\brcm"
#define BWL_FW_MAX_BYTES    (1024u * 1024u)
#define BWL_NVRAM_MAX_BYTES (16u * 1024u)
#define BWL_CLM_MAX_BYTES   (64u * 1024u)

typedef struct _BWL_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFWORKITEM BringUp;
    WDFMEMORY BounceMemory;
    PUCHAR Bounce;

    SDBUS_INTERFACE_STANDARD Sd;
    BOOLEAN SdOpen;
    UCHAR Function;             /* SDP_FUNCTION_NUMBER of this PDO */
    volatile LONG Stopping;

    ULONG Stage;
    ULONG MaxBlocks;
    ULONG F2BlockMode;
    ULONG Verify;

    /* Counters (CMD52/CMD53 through sdbus) */
    ULONG Cmd52;
    ULONG Cmd53;
    ULONG Cmd52Errors;
    ULONG Cmd53Errors;
    NTSTATUS LastSdStatus;
    ULONG LastSdArg;
    ULONG LastR5Flags;

    LONG StepSeq;

    /* Stage 3: decided in DeviceAdd (Class Net, function 1, Stage 3) */
    BOOLEAN WifiMode;
    BOOLEAN FwReady;                /* firmware up and BwlWlanInit done */
    UCHAR AddFunction;              /* the function number DeviceAdd found, 0xFF if none */
    CHAR FwVer[64];                 /* the running firmware's version number */

    /* the worker thread (stage 3): WDI requests, the poller, TX */
    PKTHREAD Worker;
    volatile LONG WorkerStop;
    KEVENT WakeEvent;
    ULONG PollMs, IdlePollMs;

    /* the network adapter (netadapter.c) */
    NETADAPTER Adapter;
    KSPIN_LOCK DataLock;            /* the queues and both FIFOs */
    NETPACKETQUEUE TxQueue, RxQueue;
    BOOLEAN TxNotify, RxNotify;
    BOOLEAN RxHold;                 /* received frames wait until the association is indicated */
    bwl_frameq *TxQ, *RxQ;
    UCHAR TxGather[1700];           /* one packet's fragments (the TX queue serialises) */
    UCHAR TxEth[1700];              /* the gathered 802.11 frame as Ethernet */
    ULONG TxFrames, TxDrops, TxErrors, RxFrames, RxDrops;

    struct _BWL_WIFI *Wifi;         /* wificx.cpp, from DeviceAdd on (stage 3 only) */

    bwl_dev Bwl;
} BWL_DEVICE_CONTEXT, *PBWL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BWL_DEVICE_CONTEXT, BwlGetContext)

typedef struct _BWL_ADAPTER_CONTEXT {
    PBWL_DEVICE_CONTEXT Device;
} BWL_ADAPTER_CONTEXT, *PBWL_ADAPTER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BWL_ADAPTER_CONTEXT, BwlAdapterGetContext)

/* sdbus.c */
extern const bwl_bus_ops BwlSdOps;
NTSTATUS BwlSdOpen(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlSdClose(_In_ PBWL_DEVICE_CONTEXT Ctx);
NTSTATUS BwlSdGetProperty(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SDBUS_PROPERTY Property,
                          _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);
NTSTATUS BwlSdSetProperty(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SDBUS_PROPERTY Property,
                          _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length);

/* files.c */
NTSTATUS BwlLoadFile(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG MaxBytes,
                     _Out_ WDFMEMORY *Memory, _Out_ PUCHAR *Data, _Out_ PULONG Length);

/* driver.c: breadcrumbs, PASSIVE_LEVEL */
VOID BwlCrumb(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCSTR Step);
VOID BwlPutULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value);
VOID BwlPutString(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value);
VOID BwlPutWString(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCWSTR Value);
ULONG BwlGetULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Default);
/* A REG_SZ setting as ASCII (other characters become '?'); 0 if absent. */
ULONG BwlGetAscii(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _Out_writes_(Cap) PCHAR Out, _In_ ULONG Cap);
VOID BwlPutCounters(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlDelayMs(_In_ ULONG Ms);

/* netadapter.c */
NTSTATUS BwlNetCreateAdapter(_In_ PBWL_DEVICE_CONTEXT Ctx, _Inout_ NETADAPTER_INIT *Init);
NTSTATUS BwlNetStart(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlNetFree(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlNetRxRelease(_In_ PBWL_DEVICE_CONTEXT Ctx);
/* The worker: send what the TX FIFO holds while the firmware takes it. TRUE if anything went out. */
BOOLEAN BwlNetTxPump(_In_ PBWL_DEVICE_CONTEXT Ctx);
/* Before a pairwise key: let what Windows queued (message 4) leave first. */
VOID BwlNetTxDrain(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ ULONG MaxMs);
VOID BwlNetFlush(_In_ PBWL_DEVICE_CONTEXT Ctx);
extern const bwl_net_ops BwlNetOps;

/* wificx.cpp: the WiFiCx front end (stage 3) */
NTSTATUS BwlWifiPreDeviceCreate(_Inout_ PWDFDEVICE_INIT DeviceInit);
NTSTATUS BwlWifiPostDeviceCreate(_In_ PBWL_DEVICE_CONTEXT Ctx);
NTSTATUS BwlWifiReportCapabilities(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlWifiRun(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlWifiScanBss(_In_ void *Ctx, _In_ const bwl_bss *Bss, _In_reads_(Len) const u8 *Body, _In_ u32 Len);
VOID BwlWifiReleaseHardware(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlWifiCleanup(_In_ PBWL_DEVICE_CONTEXT Ctx);

EXTERN_C_END
