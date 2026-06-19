/*++

Module Name:

    rk3xi2c.h

Abstract:

    Internal definitions for the Rockchip rk3x I2C controller driver. The driver
    is an SpbCx (SpbCx.sys) controller: SpbCx owns the I/O queue and the
    I2cSerialBus resource hub; this driver maps the controller, parses each
    target's address/speed, and runs byte transfers on the rk3x engine.

    v1 uses a *polled* transfer engine (interrupts left disabled, REG_IPD
    polled). SpbCx dispatches I/O sequentially at PASSIVE_LEVEL, so polling is
    safe and simple. An interrupt-driven engine is a later optimization.

Environment:

    Kernel mode.

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

//
// SpbCx controller API + ACPI/connection descriptor structs.
//
#include <SPBCx.h>
#include <reshub.h>

#include "rk3xi2c_regs.h"

#define RK3XI2C_POOL_TAG        'I2kR'   // "Rk2I"

//
// Functional input clock feeding the SCL divider. The rk3x SCL rate is
// input_clk / (8 * (CLKDIVL+1 + CLKDIVH+1)). Windows-on-ARM has no clock
// framework, so we assume the rate the EDK2 firmware leaves the I2C clock at.
// Assuming the *highest* plausible rate is the safe bias: if the real clock is
// lower the bus simply runs slower than requested, never faster.
//
// TODO: source this from an ACPI _DSD property once the EDK2 port exports it,
// instead of a compile-time constant.
//
#define RK3XI2C_INPUT_CLK_HZ    200000000UL

#define RK3XI2C_DEFAULT_BUS_HZ  100000UL   // standard mode fallback
#define RK3XI2C_FAST_BUS_HZ     400000UL

//
// Per-transfer timeout (microseconds) waiting on a chunk to finish.
//
#define RK3XI2C_XFER_TIMEOUT_US 100000UL   // 100 ms

//
// Tracing (DbgPrintEx; WPP avoided for build simplicity). Format string is the
// first variadic arg; include "\n" explicitly.
//
#define RK_DBG_ERROR            DPFLTR_ERROR_LEVEL
#define RK_DBG_INFO             DPFLTR_INFO_LEVEL
#define RK_DBG_TRACE            DPFLTR_TRACE_LEVEL

#define RkLog(_Level, ...)                                                    \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (_Level), "rk3xi2c: " __VA_ARGS__)

//
// Controller context (on the WDFDEVICE).
//
typedef struct _RK3XI2C_CONTEXT {
    volatile UCHAR  *Regs;
    PHYSICAL_ADDRESS RegsPhysical;
    ULONG            RegsLength;
} RK3XI2C_CONTEXT, *PRK3XI2C_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RK3XI2C_CONTEXT, GetControllerContext)

//
// Per-target context (allocated on each SPBTARGET in OnTargetConnect).
//
typedef struct _RK3XI2C_TARGET {
    USHORT  Address;          // 7-bit slave address
    BOOLEAN TenBitAddress;    // 10-bit addressing requested
    ULONG   ConnectionSpeed;  // Hz, from the I2cSerialBus descriptor
    ULONG   ClkDiv;           // precomputed REG_CLKDIV value
} RK3XI2C_TARGET, *PRK3XI2C_TARGET;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RK3XI2C_TARGET, GetTargetContext)

//
// One logical I2C message for the transfer engine.
//
typedef struct _RK3X_I2C_MSG {
    USHORT  Address;          // 7-bit slave address
    BOOLEAN Read;             // TRUE = read, FALSE = write
    PUCHAR  Buffer;
    ULONG   Length;
} RK3X_I2C_MSG, *PRK3X_I2C_MSG;

//
// driver.c
//
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD       Rk3xI2cEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Rk3xI2cEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Rk3xI2cEvtReleaseHardware;

//
// spb.c — SpbCx callbacks.
//
EVT_SPB_TARGET_CONNECT      Rk3xI2cEvtTargetConnect;
EVT_SPB_CONTROLLER_LOCK     Rk3xI2cEvtControllerLock;
EVT_SPB_CONTROLLER_UNLOCK   Rk3xI2cEvtControllerUnlock;
EVT_SPB_CONTROLLER_IO_READ      Rk3xI2cEvtIoRead;
EVT_SPB_CONTROLLER_IO_WRITE     Rk3xI2cEvtIoWrite;
EVT_SPB_CONTROLLER_IO_SEQUENCE  Rk3xI2cEvtIoSequence;

//
// hw.c — rk3x transfer engine.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
ULONG
Rk3xI2cComputeClkDiv(
    _In_ ULONG InputClockHz,
    _In_ ULONG BusClockHz
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Rk3xI2cTransfer(
    _In_ PRK3XI2C_CONTEXT Ctx,
    _In_ ULONG ClkDiv,
    _In_reads_(MsgCount) PRK3X_I2C_MSG Msgs,
    _In_ ULONG MsgCount,
    _Out_ PULONG BytesTransferred
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
Rk3xI2cHwInit(
    _In_ PRK3XI2C_CONTEXT Ctx
    );
