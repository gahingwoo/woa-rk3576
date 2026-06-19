/*++

Module Name:

    rk3576gpio.h

Abstract:

    Internal definitions for the RK3576 GPIO controller miniport. The driver is
    a GpioClx (msgpioclx.sys) client: it implements the CLIENT_* register-level
    callbacks while GpioClx owns PnP, power, interrupt connection and the
    GpioIo/GpioInt resource hub that other ACPI devices reference.

Environment:

    Kernel mode.

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

//
// GpioClx miniport API. Provided by the WDK (gpioclx.h, link gpioclx.lib).
// All of the CLIENT_CONTROLLER_BASIC_INFORMATION / GPIO_*_PARAMETERS types and
// the GPIO_CLX_* helpers used below come from this header — we do not redefine
// them, so the build-time ABI is whatever the installed WDK declares.
//
#include <gpioclx.h>

#include "rk3576gpio_regs.h"

#define RK3576GPIO_POOL_TAG             'GkrR'   // "RkG" (reversed for the dump)

#define RK3576GPIO_PINS_PER_BANK        32

//
// Tracing. WPP is intentionally avoided to keep the out-of-tree build simple;
// a verbosity-gated DbgPrintEx wrapper is used instead. Bump the level with the
// usual kernel debugger filter (e.g. 'ed nt!Kd_IHVDRIVER_Mask 0xf').
//
#define RK_DBG_ERROR                    DPFLTR_ERROR_LEVEL
#define RK_DBG_WARN                     DPFLTR_WARNING_LEVEL
#define RK_DBG_INFO                     DPFLTR_INFO_LEVEL
#define RK_DBG_TRACE                    DPFLTR_TRACE_LEVEL

// The format string is the first variadic argument so the macro works with or
// without trailing args under either MSVC preprocessor mode. Include "\n" in
// each call's format string explicitly.
#define RkLog(_Level, ...)                                                    \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (_Level), "rk3576gpio: " __VA_ARGS__)

//
// Per-controller (per-bank) device context. GpioClx allocates a buffer of
// GpioDeviceContextSize and hands it back as the 'Context' argument to every
// CLIENT_* callback.
//
typedef struct _RK3576GPIO_CONTEXT {

    //
    // Mapped MMIO window for this bank (from the ACPI _CRS memory resource).
    //
    volatile UCHAR *Regs;
    PHYSICAL_ADDRESS RegsPhysical;
    ULONG            RegsLength;

    //
    // Bank index decoded from the ACPI _UID (0..4); informational/diagnostic.
    //
    ULONG            BankNumber;

} RK3576GPIO_CONTEXT, *PRK3576GPIO_CONTEXT;

//
// driver.c
//
DRIVER_INITIALIZE       DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD   RkGpioEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD       RkGpioEvtDriverUnload;

//
// controller.c — GpioClx CLIENT_* callbacks.
//
GPIO_CLIENT_PREPARE_CONTROLLER                  RkGpioPrepareController;
GPIO_CLIENT_RELEASE_CONTROLLER                  RkGpioReleaseController;
GPIO_CLIENT_START_CONTROLLER                    RkGpioStartController;
GPIO_CLIENT_STOP_CONTROLLER                     RkGpioStopController;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION  RkGpioQueryControllerBasicInformation;

GPIO_CLIENT_CONNECT_IO_PINS                     RkGpioConnectIoPins;
GPIO_CLIENT_DISCONNECT_IO_PINS                  RkGpioDisconnectIoPins;
GPIO_CLIENT_READ_PINS_MASK                      RkGpioReadPinsUsingMask;
GPIO_CLIENT_WRITE_PINS_MASK                     RkGpioWritePinsUsingMask;

GPIO_CLIENT_ENABLE_INTERRUPT                    RkGpioEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT                   RkGpioDisableInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS                     RkGpioMaskInterrupts;
GPIO_CLIENT_UNMASK_INTERRUPT                    RkGpioUnmaskInterrupt;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS             RkGpioQueryActiveInterrupts;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS             RkGpioClearActiveInterrupts;
GPIO_CLIENT_RECONFIGURE_INTERRUPT               RkGpioReconfigureInterrupt;
GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS            RkGpioQueryEnabledInterrupts;
