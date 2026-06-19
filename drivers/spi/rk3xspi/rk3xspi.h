/*++

Module Name:

    rk3xspi.h

Abstract:

    Internal definitions for the Rockchip SPI controller driver (SpbCx). Same
    framework shape as the rk3x I2C driver: SpbCx owns the queue and the
    SpiSerialBus resource hub; this driver maps the controller, parses each
    target's CS/speed/mode, and runs full-duplex transfers on the rockchip SPI
    engine. Polled, since SpbCx dispatches I/O sequentially at PASSIVE_LEVEL.

Environment:

    Kernel mode.

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

#include <SPBCx.h>
#include <reshub.h>

#include "rk3xspi_regs.h"

#define RK3XSPI_POOL_TAG        'SPkR'   // "RkPS"

//
// SPI functional input clock (the divider source). From the EDK2 _DSD
// "clock-frequency" = 200 MHz. Biased so the computed BAUDR never overclocks.
// TODO: read from _DSD once exported as a discrete property.
//
#define RK3XSPI_INPUT_CLK_HZ    200000000UL
#define RK3XSPI_DEFAULT_HZ      1000000UL

#define RK3XSPI_XFER_TIMEOUT_US 1000000UL    // 1 s per transfer

#define RK_DBG_ERROR            DPFLTR_ERROR_LEVEL
#define RK_DBG_INFO             DPFLTR_INFO_LEVEL

#define RkLog(_Level, ...)                                                    \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (_Level), "rk3xspi: " __VA_ARGS__)

//
// Controller context (on the WDFDEVICE).
//
typedef struct _RK3XSPI_CONTEXT {
    volatile UCHAR  *Regs;
    PHYSICAL_ADDRESS RegsPhysical;
    ULONG            RegsLength;
} RK3XSPI_CONTEXT, *PRK3XSPI_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RK3XSPI_CONTEXT, GetControllerContext)

//
// Per-target context (on each SPBTARGET).
//
typedef struct _RK3XSPI_TARGET {
    ULONG   ChipSelect;       // CS index (SER bit)
    ULONG   ConnectionSpeed;  // Hz
    ULONG   Baudr;            // even divider for BAUDR
    BOOLEAN CpolHigh;         // clock idle high
    BOOLEAN CphaSecond;       // sample on second edge
} RK3XSPI_TARGET, *PRK3XSPI_TARGET;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RK3XSPI_TARGET, GetTargetContext)

//
// driver.c
//
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD       Rk3xSpiEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Rk3xSpiEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Rk3xSpiEvtReleaseHardware;

//
// spb.c
//
EVT_SPB_TARGET_CONNECT          Rk3xSpiEvtTargetConnect;
EVT_SPB_CONTROLLER_LOCK         Rk3xSpiEvtControllerLock;
EVT_SPB_CONTROLLER_UNLOCK       Rk3xSpiEvtControllerUnlock;
EVT_SPB_CONTROLLER_IO_READ      Rk3xSpiEvtIoRead;
EVT_SPB_CONTROLLER_IO_WRITE     Rk3xSpiEvtIoWrite;
EVT_SPB_CONTROLLER_IO_SEQUENCE  Rk3xSpiEvtIoSequence;
EVT_SPB_CONTROLLER_IO_OTHER     Rk3xSpiEvtIoOther;

//
// hw.c — rockchip SPI engine.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
ULONG
Rk3xSpiComputeBaudr(
    _In_ ULONG InputClockHz,
    _In_ ULONG TargetHz
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
Rk3xSpiHwInit(
    _In_ PRK3XSPI_CONTEXT Ctx
    );

//
// One full-duplex (TR) transfer of Length bytes. Tx or Rx may be NULL (zeros
// are clocked out / received bytes discarded respectively).
//
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Rk3xSpiTransfer(
    _In_ PRK3XSPI_CONTEXT Ctx,
    _In_ PRK3XSPI_TARGET Target,
    _In_reads_bytes_opt_(Length) PUCHAR Tx,
    _Out_writes_bytes_opt_(Length) PUCHAR Rx,
    _In_ ULONG Length
    );
