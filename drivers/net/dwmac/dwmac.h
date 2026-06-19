/*++

Module Name:

    dwmac.h

Abstract:

    Internal definitions for the RK3576 GMAC (Synopsys DWMAC-4.20a) NetAdapterCx
    driver. The DWMAC hardware engine (hw.c) is verified against the kernel
    stmmac driver. The NetAdapterCx framework integration (netadapter.c) follows
    the documented NetCx model; NetCx struct/field names are owned by the WDK
    <netadaptercx.h> and are marked VERIFY-ON-BUILD where used.

    Datapath model (v1): bounce buffers. The TX/RX descriptor rings and a pool
    of fixed DMA buffers live in WDF common buffers; the framework's packet data
    is copied to/from those buffers. Zero-copy scatter-gather DMA is a later
    optimization.

Environment:

    Kernel mode.

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>

#include "dwmac_regs.h"

#define DWMAC_POOL_TAG          'cMwD'   // "DwMc"

#define DWMAC_TX_RING           64
#define DWMAC_RX_RING           64
#define DWMAC_BUF_SIZE          2048     // per descriptor bounce buffer

#define DWMAC_MDIO_TIMEOUT_US   100000

#define RK_DBG_ERROR            DPFLTR_ERROR_LEVEL
#define RK_DBG_INFO             DPFLTR_INFO_LEVEL

#define RkLog(_Level, ...)                                                    \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (_Level), "dwmac: " __VA_ARGS__)

typedef struct _DWMAC_ADAPTER {
    WDFDEVICE        Device;
    NETADAPTER       Adapter;

    volatile UCHAR  *Mac;            // MAC/DMA register window (ACPI _CRS)
    PHYSICAL_ADDRESS MacPhys;
    ULONG            MacLen;

    volatile UCHAR  *Grf;            // SDGMAC_GRF clock-select window (fixed addr)

    //
    // DMA: descriptor rings and bounce buffers as physically-contiguous common
    // buffers.
    //
    WDFDMAENABLER    DmaEnabler;
    WDFCOMMONBUFFER  TxDescCb;
    PDWMAC_DESC      TxDesc;
    PHYSICAL_ADDRESS TxDescPa;
    WDFCOMMONBUFFER  RxDescCb;
    PDWMAC_DESC      RxDesc;
    PHYSICAL_ADDRESS RxDescPa;
    WDFCOMMONBUFFER  TxBufCb;
    PUCHAR           TxBuf;
    PHYSICAL_ADDRESS TxBufPa;
    WDFCOMMONBUFFER  RxBufCb;
    PUCHAR           RxBuf;
    PHYSICAL_ADDRESS RxBufPa;

    ULONG            TxHead;          // next desc to fill
    ULONG            TxTail;          // next desc to reclaim
    ULONG            RxIndex;         // next rx desc to inspect

    ULONG            PhyAddr;         // MDIO PHY address
    UCHAR            MacAddress[6];

    BOOLEAN          LinkUp;
    ULONG            LinkSpeedMbps;
    BOOLEAN          FullDuplex;

    NETPACKETQUEUE   TxQueue;
    NETPACKETQUEUE   RxQueue;

    //
    // Interrupt + queue-notification state. The ISR latches the DMA channel
    // status; the DPC wakes whichever queues the framework is waiting on.
    //
    WDFINTERRUPT     Interrupt;
    volatile LONG    IrqStatus;
    BOOLEAN          TxNotify;
    BOOLEAN          RxNotify;
} DWMAC_ADAPTER, *PDWMAC_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DWMAC_ADAPTER, DwmacGetAdapterContext)

//
// hw.c — DWMAC engine (verified).
//
NTSTATUS DwmacSwReset(_In_ PDWMAC_ADAPTER A);
VOID     DwmacInitMacDma(_In_ PDWMAC_ADAPTER A);
VOID     DwmacSetMacAddress(_In_ PDWMAC_ADAPTER A);
NTSTATUS DwmacMdioRead(_In_ PDWMAC_ADAPTER A, _In_ ULONG Reg, _Out_ PUSHORT Value);
NTSTATUS DwmacMdioWrite(_In_ PDWMAC_ADAPTER A, _In_ ULONG Reg, _In_ USHORT Value);
NTSTATUS DwmacPhyDetect(_In_ PDWMAC_ADAPTER A);
VOID     DwmacUpdateLink(_In_ PDWMAC_ADAPTER A);      // read PHY, program GRF clock + MAC speed
VOID     DwmacInitRings(_In_ PDWMAC_ADAPTER A);
VOID     DwmacStart(_In_ PDWMAC_ADAPTER A);
VOID     DwmacStop(_In_ PDWMAC_ADAPTER A);

//
// Read the DMA channel interrupt status and write-1-clear the handled bits.
// Safe at DIRQL. Returns the raw status (test DMA_CHAN_STATUS_TI / _RI / _RBU).
//
ULONG    DwmacReadAndClearIrq(_In_ PDWMAC_ADAPTER A);

//
// TX: copy a frame into the next free descriptor's bounce buffer and hand it to
// the DMA. Returns FALSE if the ring is full.
//
BOOLEAN  DwmacTransmitFrame(_In_ PDWMAC_ADAPTER A, _In_reads_bytes_(Length) PUCHAR Frame, _In_ ULONG Length);

//
// TX reclaim: returns count of descriptors completed since last call.
//
ULONG    DwmacReclaimTx(_In_ PDWMAC_ADAPTER A);

//
// RX: if the current RX descriptor has a completed frame, copy it into Dst,
// re-arm the descriptor, advance, and return TRUE. Returns FALSE if none ready
// (or a descriptor error, which is recycled silently).
//
BOOLEAN  DwmacReceiveFrame(_In_ PDWMAC_ADAPTER A, _Out_writes_bytes_(DstCap) PUCHAR Dst, _In_ ULONG DstCap, _Out_ PULONG Length);

//
// netadapter.c
//
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD       DwmacEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE DwmacEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE DwmacEvtReleaseHardware;
