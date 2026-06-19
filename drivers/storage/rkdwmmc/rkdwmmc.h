/*++

Module Name:

    rkdwmmc.h

Abstract:

    Internal definitions for the RK3576 SD card (dw_mmc) sdport miniport. The
    driver plugs into Windows' SD port driver (sdport.sys): DriverEntry hands
    SdPortInitialize a table of SDPORT_* callbacks; sdport owns PnP/power, the
    interrupt, and the SD command protocol, while this miniport pokes the dw_mmc
    registers.

    Layering:
      hw.c       - dw_mmc register engine (verified against drivers/mmc/host/dw_mmc.c)
      miniport.c - SDPORT_* callbacks mapping sdport requests onto hw.c

    NOTE: the dw_mmc hardware layer is kernel-verified. The sdport integration in
    miniport.c follows the documented sdport miniport model and Microsoft's
    "sdhc" sample; the exact SDPORT_* struct/field names and the event/DPC flow
    are owned by the WDK <sdport.h> and must be validated when first built.

Environment:

    Kernel mode.

--*/

#pragma once

#include <ntddk.h>
#include <sdport.h>

#include "rkdwmmc_regs.h"

#define RKDWMMC_POOL_TAG        'mMkR'   // "RkMm"

//
// dw_mmc card (CIU) input clock. As with I2C there is no Windows clock
// framework; we assume the rate the firmware programs. RK3576 SD CIU is
// typically 150 MHz. Biased so the computed divider never overclocks the card.
//
// TODO: source from an ACPI _DSD property once the EDK2 port exports it.
//
#define RKDWMMC_CIU_CLOCK_HZ    150000000UL

//
// Per-slot private extension (SDPORT_INITIALIZATION_DATA.PrivateExtensionSize).
//
typedef struct _RKDWMMC_SLOT {
    volatile UCHAR  *Regs;
    PHYSICAL_ADDRESS RegsPhysical;
    ULONG            RegsLength;

    ULONG            FifoOffset;   // 0x100 or 0x200, by VERID
    ULONG            FifoDepth;    // words

    ULONG            CiuClockHz;   // input clock to the divider
    ULONG            CurrentClockHz;

    //
    // PIO state for the in-flight data command (sdport single-block / multi-
    // block). Buffer is the sdport-provided data buffer for the current request.
    //
    PUCHAR           DataBuffer;
    ULONG            DataLength;
    ULONG            DataTransferred;
    BOOLEAN          DataWrite;

} RKDWMMC_SLOT, *PRKDWMMC_SLOT;

//
// Tracing.
//
#define RK_DBG_ERROR            DPFLTR_ERROR_LEVEL
#define RK_DBG_INFO             DPFLTR_INFO_LEVEL
#define RK_DBG_TRACE            DPFLTR_TRACE_LEVEL

#define RkLog(_Level, ...)                                                    \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (_Level), "rkdwmmc: " __VA_ARGS__)

//
// hw.c — dw_mmc register engine (verified).
//
ULONG    DwmmcFifoOffset(_In_ volatile UCHAR *Regs);
NTSTATUS DwmmcResetAll(_In_ PRKDWMMC_SLOT Slot);
VOID     DwmmcInitController(_In_ PRKDWMMC_SLOT Slot);
NTSTATUS DwmmcUpdateClockRegs(_In_ PRKDWMMC_SLOT Slot);
NTSTATUS DwmmcSetClock(_In_ PRKDWMMC_SLOT Slot, _In_ ULONG FrequencyHz);
VOID     DwmmcSetBusWidth(_In_ PRKDWMMC_SLOT Slot, _In_ ULONG WidthBits);
VOID     DwmmcSetBlockConfig(_In_ PRKDWMMC_SLOT Slot, _In_ ULONG BlockSize, _In_ ULONG BlockCount);

NTSTATUS DwmmcSendCommand(
    _In_ PRKDWMMC_SLOT Slot,
    _In_ ULONG CmdRegister,   // pre-built CMD bits incl. index (no CMD_START)
    _In_ ULONG Argument
    );

ULONG    DwmmcFifoCount(_In_ PRKDWMMC_SLOT Slot);
ULONG    DwmmcReadFifo(_In_ PRKDWMMC_SLOT Slot, _Out_writes_bytes_(Bytes) PUCHAR Dst, _In_ ULONG Bytes);   // returns bytes read
ULONG    DwmmcWriteFifo(_In_ PRKDWMMC_SLOT Slot, _In_reads_bytes_(Bytes) PUCHAR Src, _In_ ULONG Bytes);    // returns bytes written

//
// miniport.c — SDPORT callbacks. (Signatures per the sdport miniport model.)
//
SDPORT_GET_SLOT_COUNT           RkdwmmcGetSlotCount;
SDPORT_GET_SLOT_CAPABILITIES    RkdwmmcGetSlotCapabilities;
SDPORT_INITIALIZE               RkdwmmcSlotInitialize;
SDPORT_ISSUE_BUS_OPERATION      RkdwmmcIssueBusOperation;
SDPORT_GET_CARD_DETECT_STATE    RkdwmmcGetCardDetectState;
SDPORT_GET_WRITE_PROTECT_STATE  RkdwmmcGetWriteProtectState;
SDPORT_ISSUE_REQUEST            RkdwmmcIssueRequest;
SDPORT_GET_RESPONSE             RkdwmmcGetResponse;
SDPORT_INTERRUPT                RkdwmmcInterrupt;
SDPORT_REQUEST_DPC              RkdwmmcRequestDpc;
SDPORT_TOGGLE_EVENTS            RkdwmmcToggleEvents;
SDPORT_CLEAR_EVENTS             RkdwmmcClearEvents;
SDPORT_SAVE_CONTEXT             RkdwmmcSaveContext;
SDPORT_RESTORE_CONTEXT          RkdwmmcRestoreContext;
SDPORT_CLEANUP                  RkdwmmcCleanup;
