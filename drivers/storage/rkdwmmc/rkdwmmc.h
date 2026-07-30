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
// The Rockchip mmc clock path has a fixed divide-by-two between the CRU clock
// (cclk_src_sdmmc0) and the card clock, so to obtain a card clock of F the CRU
// must be programmed to 2*F. This is RK3288_CLKGEN_DIV in the Linux driver
// (drivers/mmc/host/dw_mmc-rockchip.c), applied on RK3576 too via
// dw_mci_rk3288_set_ios.
//
#define RKDWMMC_CLKGEN_DIV      2UL

//
// Fallback CIU rate, used only if the SiP clock service is unavailable (an
// older BL31) and we have to fall back to the controller's own divider.
//
#define RKDWMMC_CIU_CLOCK_FALLBACK_HZ   150000000UL

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
    // Set once probing shows BL31 answers the SiP SD/MMC clock service. When
    // clear we fall back to the controller's own divider, which cannot reach
    // the higher speed modes on this SoC but does let a card enumerate.
    //
    BOOLEAN          SipClockAvailable;

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
NTSTATUS DwmmcSetPhase(_In_ PRKDWMMC_SLOT Slot, _In_ BOOLEAN Sample, _In_ ULONG Degrees);
ULONG    DwmmcGetPhase(_In_ PRKDWMMC_SLOT Slot, _In_ BOOLEAN Sample);
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
