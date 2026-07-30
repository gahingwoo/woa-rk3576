/*++

Module Name:

    dwmac.h

Abstract:

    Internal definitions for the RK3576 GMAC (Synopsys DWMAC-4.20a) NetAdapterCx
    driver. The DWMAC hardware engine (hw.c) is verified against the kernel
    stmmac driver; the NetAdapterCx integration (netadapter.c) is compiled
    against NetAdapter 2.1 and follows the same shape as the RK3588 dwc_eqos
    driver.

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

//
// The fragment virtual-address extension. A bounce-buffer driver reaches
// packet data through this, not through NET_FRAGMENT itself.
//
#include <net/virtualaddress.h>

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

//
// DMA tuning. Every field here has a counterpart in the device tree that
// mainline's stmmac reads, and the same names are published by our firmware
// through _DSD and the AXIC package it points at. The defaults below are
// stmmac's defaults for a property that is absent, so a firmware that publishes
// nothing still lands on the behaviour Linux would have.
//
typedef struct _DWMAC_DMA_CFG {
    ULONG   Pbl;                 // snps,pbl        (default DWMAC_DEFAULT_PBL)
    ULONG   TxPbl;               // snps,txpbl      (0 = use Pbl)
    ULONG   RxPbl;               // snps,rxpbl      (0 = use Pbl)
    BOOLEAN PblX8;               // !snps,no-pbl-x8 (default TRUE)

    BOOLEAN FixedBurst;          // snps,fixed-burst
    BOOLEAN MixedBurst;          // snps,mixed-burst
    BOOLEAN Aal;                 // snps,aal
    BOOLEAN Eame;                // not from firmware; from the core's own
                                 // GMAC_HW_FEATURE1 address-width capability

    //
    // From the package named by snps,axi-config. Only applied when
    // AxiConfigFound; otherwise the AXI fields keep their reset values, which
    // is exactly what stmmac does when the phandle is absent.
    //
    BOOLEAN AxiConfigFound;
    BOOLEAN AxiLpiEn;            // snps,lpi_en
    BOOLEAN AxiXitFrm;           // snps,xit_frm
    ULONG   AxiWrOsrLmt;         // snps,wr_osr_lmt (default 1)
    ULONG   AxiRdOsrLmt;         // snps,rd_osr_lmt (default 1)
    ULONG   AxiBlenMask;         // snps,blen, already positioned in [7:1]
} DWMAC_DMA_CFG, *PDWMAC_DMA_CFG;

//
// stmmac's DEFAULT_DMA_PBL.
//
#define DWMAC_DEFAULT_PBL       8

typedef struct _DWMAC_ADAPTER {
    WDFDEVICE        Device;
    NETADAPTER       Adapter;

    volatile UCHAR  *Mac;            // MAC/DMA register window (ACPI _CRS)
    PHYSICAL_ADDRESS MacPhys;
    ULONG            MacLen;


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

    DWMAC_DMA_CFG    DmaCfg;

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
// DWMAC_ADAPTER hangs off the WDFDEVICE. NetCx offers no way to get from a
// NETADAPTER or a NETPACKETQUEUE back to it, so both of those objects carry a
// small context of their own that points at it -- the same arrangement the
// RK3588 dwc_eqos driver uses.
//
typedef struct _DWMAC_ADAPTER_REF {
    PDWMAC_ADAPTER   Adapter;
} DWMAC_ADAPTER_REF, *PDWMAC_ADAPTER_REF;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DWMAC_ADAPTER_REF, DwmacGetAdapterRef)

typedef struct _DWMAC_QUEUE {
    PDWMAC_ADAPTER   Adapter;

    //
    // Queried once at queue creation; NetExtensionGetFragmentVirtualAddress*
    // needs it to resolve a fragment index to a mapped buffer.
    //
    NET_EXTENSION    FragmentVa;
} DWMAC_QUEUE, *PDWMAC_QUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DWMAC_QUEUE, DwmacGetQueueContext)

//
// hw.c — DWMAC engine (verified).
//
NTSTATUS DwmacSwReset(_In_ PDWMAC_ADAPTER A);
VOID     DwmacInitMacDma(_In_ PDWMAC_ADAPTER A);
VOID     DwmacSetMacAddress(_In_ PDWMAC_ADAPTER A);
NTSTATUS DwmacMdioRead(_In_ PDWMAC_ADAPTER A, _In_ ULONG Reg, _Out_ PUSHORT Value);
NTSTATUS DwmacMdioWrite(_In_ PDWMAC_ADAPTER A, _In_ ULONG Reg, _In_ USHORT Value);
NTSTATUS DwmacPhyDetect(_In_ PDWMAC_ADAPTER A);
VOID     DwmacUpdateLink(_In_ PDWMAC_ADAPTER A);      // read PHY, set TX clock + MAC speed

//
// acpi_dsm.c — firmware-owned TX clock selection.
//
NTSTATUS DwmacSetTxClockDsm(_In_ PDWMAC_ADAPTER A, _In_ ULONG SpeedMbps);

//
// Synchronous IOCTL to the ACPI PDO. Lives in acpi_dsm.c; acpi_dsd.c uses it
// too.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
DwmacSendAcpiIoctl(
    _In_ PDEVICE_OBJECT Pdo,
    _In_reads_bytes_(InputSize) PVOID Input,
    _In_ ULONG InputSize,
    _Out_writes_bytes_(OutputSize) PVOID Output,
    _In_ ULONG OutputSize,
    _Out_ PULONG BytesReturned
    );

//
// acpi_dsd.c — DMA tuning published by firmware. Fills Cfg with the stmmac
// defaults first, so the caller may ignore the return value and still get a
// usable configuration if firmware publishes nothing.
//
VOID     DwmacReadDmaCfgFromFirmware(_In_ PDWMAC_ADAPTER A, _Out_ PDWMAC_DMA_CFG Cfg);
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
