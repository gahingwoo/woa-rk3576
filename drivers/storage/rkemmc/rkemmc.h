/*++

Module Name:

    rkemmc.h

Abstract:

    Internal definitions for the RK3576 eMMC (DWCMSHC) sdport miniport.

    Why this driver exists at all: the controller is SDHCI register compatible,
    Windows has an inbox SDHCI miniport, and the firmware's ACPI node carries
    a _CID of PNP0D40 so that driver binds.  It binds, it starts -- and no card
    ever appears.  Measured on CM5-IO 2026-09-19: ACPI\RKCP0D40\3 reaches
    Started on sdbus.inf with no child device.

    The reason is three Rockchip vendor bits that an SDHCI SW_RST_ALL clears:

      EMMC_MISC_CON[1]  MISC_INTCLK_EN   internal clock off -> every cmd times out
      EMMC_CTRL[2]      EMMC_RST_N       eMMC held in hardware reset -> CMD0 times out
      EMMC_CTRL[0]      CARD_IS_EMMC     HS400 data strobe does not work

    No inbox driver restores them, and no ACPI change can: the reset is an MMIO
    write from inside the host driver.  So the host driver has to be ours.

    Layering:
      hw.c       - SDHCI engine plus the Rockchip vendor hooks
      miniport.c - SDPORT_* callbacks mapping sdport requests onto hw.c

    Related work in this project's firmware, which hit the same hardware from
    the other side: Silicon/Rockchip/Drivers/DwcSdhciDxe.  Two findings there
    are reproduced here because they are properties of the silicon, not of
    EDK2.  First, the card clock comes from the CRU and the SDHCI divider is
    non-functional.  Second -- and this one cost a five-minute boot -- after
    changing the CRU rate the internal clock must be restarted and waited for,
    or the next command goes out while the controller has not relocked and
    fails with a command timeout or a data CRC error.

Environment:

    Kernel mode.

--*/

#pragma once

#include <ntddk.h>
#include <sdport.h>

#include "rkemmc_regs.h"

#define RKEMMC_POOL_TAG     'eMkR'   // "RkMe"

//
// Highest card clock this driver will ask for.  HS200/HS400 need the DLL
// tuned and a tuning pass neither written nor measured yet, so v1 stops at
// eMMC high speed.  The DLL path below 52 MHz is bypassed entirely, which is
// what makes that safe.
//
#define RKEMMC_MAX_CLOCK_HZ         52000000UL

//
// eMMC spec: at least 200 us between releasing RST_N and the first command.
//
#define RKEMMC_RST_N_SETTLE_US      200

//
// How many commands to keep.  eMMC identification is around twenty, so this
// holds the whole of it.
//
#define RKEMMC_TRACE_DEPTH          24

//
// Per-slot private extension (SDPORT_INITIALIZATION_DATA.PrivateExtensionSize).
//
typedef struct _RKEMMC_SLOT {
    volatile UCHAR      *Regs;
    PHYSICAL_ADDRESS     RegsPhysical;
    ULONG                RegsLength;

    //
    // CCLK_SRC_EMMC in the CRU.  One register, outside this device's window,
    // mapped here because an sdport miniport has no way to evaluate the ACPI
    // _DSM that does the same job for other operating systems.
    //
    volatile ULONG      *CruClkSel;

    ULONG                CurrentClockHz;
    ULONG                BusWidthBits;

    //
    // PIO state for the in-flight data command.  SDMA is a later step: the
    // point of v1 is to find out whether a card enumerates at all.
    //
    PUCHAR               DataBuffer;
    ULONG                DataLength;
    ULONG                DataTransferred;
    BOOLEAN              DataWrite;

} RKEMMC_SLOT, *PRKEMMC_SLOT;

//
// Tracing.  DbgPrint rather than DbgPrintEx on purpose: DPFLTR_IHVDRIVER_ID is
// filtered to ERROR level unless the target's Debug Print Filter mask is
// raised, which is a registry change we cannot make inside a WinPE image built
// offline.  See the same note in ../rkdwmmc/rkdwmmc.h.
//
#define RK_DBG_ERROR        DPFLTR_ERROR_LEVEL
#define RK_DBG_INFO         DPFLTR_INFO_LEVEL

#define RkLog(_Level, ...)                                                    \
    ((void)(_Level), DbgPrint("rkemmc: " __VA_ARGS__))

//
// Diagnostics, published to the registry rather than the debugger.
//
//   HKLM\SYSTEM\CurrentControlSet\Services\rkemmc\Diag
//
// The kernel debugger is not a usable instrument on this board -- with no
// listener attached the target retransmits forever and storage enumeration
// times out.  Several boots were spent learning that.  So the driver records
// what it did and collect.cmd reads it back afterwards.
//
// The interrupt handler runs above PASSIVE_LEVEL and cannot touch the
// registry, so everything accumulates in memory and is flushed from whichever
// passive-level callback runs next.
//
typedef struct _RKEMMC_DIAG {
    ULONG   ResetCalls;
    ULONG   VendorBitsBefore;     // EMMC_CTRL as found after a RESET_ALL
    ULONG   VendorBitsAfter;      // EMMC_CTRL once restored
    ULONG   MiscConAfter;         // EMMC_MISC_CON once restored
    ULONG   ClockSetCalls;
    ULONG   ClockRequestedHz;
    ULONG   ClockActualHz;
    ULONG   ClockCruValue;
    ULONG   ClockStableWaits;     // iterations spent waiting for INT_STABLE
    ULONG   ClockStableTimeouts;
    ULONG   BusOpCalls;
    ULONG   BusOpLastType;
    ULONG   RequestCalls;
    ULONG   LastCmdIndex;
    ULONG   LastCmdArg;
    ULONG   LastCmdReg;
    ULONG   LastCmdStatus;
    ULONG   InterruptCalls;
    ULONG   LastIntStatus;
    ULONG   SeenIntStatus;        // every normal-status bit ever raised
    ULONG   SeenErrStatus;        // every error-status bit ever raised
    ULONG   CmdErrors;
    ULONG   DataErrors;
    ULONG   BaseClockKhz;
    ULONG   HostVersion;
    ULONG   Capabilities;

    //
    // A ring of the last RKEMMC_TRACE_DEPTH commands.  The counters above say
    // how far the stack got; they cannot say where it stopped.  Sixteen
    // commands went out on 2026-09-20 and the snapshot could only name the
    // last one, which is not enough to tell a refused command from one that
    // was never issued.
    //
    // Each entry packs the command index, the outcome and the interrupt
    // status into one DWORD so it can be read with reg query and needs no
    // parsing on the board:
    //
    //   [31:24] sequence number   [23:16] command index
    //   [15:8]  error status low  [7:0]   normal status low
    //
    ULONG   TraceCount;
    ULONG   Trace[RKEMMC_TRACE_DEPTH];
} RKEMMC_DIAG, *PRKEMMC_DIAG;

extern RKEMMC_DIAG g_RkDiag;

VOID RkemmcDiagFlush(VOID);

//
// hw.c
//
VOID     EmmcApplyVendorBits(_In_ PRKEMMC_SLOT Slot);
NTSTATUS EmmcResetAll(_In_ PRKEMMC_SLOT Slot);
NTSTATUS EmmcResetCmdDat(_In_ PRKEMMC_SLOT Slot, _In_ UCHAR Mask);
VOID     EmmcInitController(_In_ PRKEMMC_SLOT Slot);
NTSTATUS EmmcSetClock(_In_ PRKEMMC_SLOT Slot, _In_ ULONG FrequencyHz);
VOID     EmmcSetBusWidth(_In_ PRKEMMC_SLOT Slot, _In_ ULONG WidthBits);
VOID     EmmcSetPower(_In_ PRKEMMC_SLOT Slot, _In_ BOOLEAN On, _In_ BOOLEAN Voltage18);
VOID     EmmcSetUhsMode(_In_ PRKEMMC_SLOT Slot, _In_ USHORT Mode);
NTSTATUS EmmcSendCommand(_In_ PRKEMMC_SLOT Slot, _In_ USHORT CommandReg,
                         _In_ USHORT TransferMode, _In_ ULONG Argument,
                         _In_ BOOLEAN HasData);
ULONG    EmmcReadBuffer(_In_ PRKEMMC_SLOT Slot,
                        _Out_writes_bytes_(Bytes) PUCHAR Dst, _In_ ULONG Bytes);
ULONG    EmmcWriteBuffer(_In_ PRKEMMC_SLOT Slot,
                         _In_reads_bytes_(Bytes) PUCHAR Src, _In_ ULONG Bytes);

//
// Register accessors.  Width matters here: SDHCI packs 8- and 16-bit
// registers side by side, so a 32-bit write to CLOCK_CONTROL would also
// rewrite TIMEOUT_CONTROL and SOFTWARE_RESET.
//
FORCEINLINE UCHAR  EmmcRead8 (_In_ PRKEMMC_SLOT S, _In_ ULONG O)
    { return READ_REGISTER_UCHAR((PUCHAR)(S->Regs + O)); }
FORCEINLINE USHORT EmmcRead16(_In_ PRKEMMC_SLOT S, _In_ ULONG O)
    { return READ_REGISTER_USHORT((PUSHORT)(S->Regs + O)); }
FORCEINLINE ULONG  EmmcRead32(_In_ PRKEMMC_SLOT S, _In_ ULONG O)
    { return READ_REGISTER_ULONG((PULONG)(S->Regs + O)); }

FORCEINLINE VOID EmmcWrite8 (_In_ PRKEMMC_SLOT S, _In_ ULONG O, _In_ UCHAR V)
    { WRITE_REGISTER_UCHAR((PUCHAR)(S->Regs + O), V); }
FORCEINLINE VOID EmmcWrite16(_In_ PRKEMMC_SLOT S, _In_ ULONG O, _In_ USHORT V)
    { WRITE_REGISTER_USHORT((PUSHORT)(S->Regs + O), V); }
FORCEINLINE VOID EmmcWrite32(_In_ PRKEMMC_SLOT S, _In_ ULONG O, _In_ ULONG V)
    { WRITE_REGISTER_ULONG((PULONG)(S->Regs + O), V); }

//
// miniport.c -- SDPORT callbacks.
//
SDPORT_GET_SLOT_COUNT           RkemmcGetSlotCount;
SDPORT_GET_SLOT_CAPABILITIES    RkemmcGetSlotCapabilities;
SDPORT_INITIALIZE               RkemmcSlotInitialize;
SDPORT_ISSUE_BUS_OPERATION      RkemmcIssueBusOperation;
SDPORT_GET_CARD_DETECT_STATE    RkemmcGetCardDetectState;
SDPORT_GET_WRITE_PROTECT_STATE  RkemmcGetWriteProtectState;
SDPORT_ISSUE_REQUEST            RkemmcIssueRequest;
SDPORT_GET_RESPONSE             RkemmcGetResponse;
SDPORT_INTERRUPT                RkemmcInterrupt;
SDPORT_REQUEST_DPC              RkemmcRequestDpc;
SDPORT_TOGGLE_EVENTS            RkemmcToggleEvents;
SDPORT_CLEAR_EVENTS             RkemmcClearEvents;
SDPORT_SAVE_CONTEXT             RkemmcSaveContext;
SDPORT_RESTORE_CONTEXT          RkemmcRestoreContext;
SDPORT_CLEANUP                  RkemmcCleanup;
