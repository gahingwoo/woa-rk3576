/*++

Module Name:

    miniport.c

Abstract:

    sdport miniport glue for the RK3576 dw_mmc SD slot. DriverEntry registers
    the SDPORT_* callback table; each callback maps an sdport operation onto the
    verified dw_mmc engine in hw.c.

    !!! VERIFY-ON-BUILD !!!
    The dw_mmc register logic (hw.c) is kernel-verified. This file follows the
    documented sdport miniport model and Microsoft's "sdhc" sample, but the exact
    SDPORT_* struct field names, the bus-operation/response enums, and the
    generic event/error constants are owned by the WDK <sdport.h>. Aligning these
    to the WDK ABI at the flagged spots is in progress (WIP); the control flow is
    correct, the identifiers are being settled.

Environment:

    Kernel mode.

--*/

#include "rkdwmmc.h"

//
// dw_mmc RINTSTS bits that make up a "command complete" and "transfer complete"
// for the sdport event mapping below.
//
#define DWMMC_CMD_ERROR_BITS   (DWMMC_INT_RTO | DWMMC_INT_RCRC | DWMMC_INT_RESP_ERR)
#define DWMMC_DATA_ERROR_BITS  (DWMMC_INT_DRTO | DWMMC_INT_DCRC | DWMMC_INT_SBE | \
                                DWMMC_INT_EBE | DWMMC_INT_FRUN | DWMMC_INT_HTO)

_Use_decl_annotations_
ULONG
RkdwmmcGetSlotCount(
    PVOID Argument
    )
{
    UNREFERENCED_PARAMETER(Argument);
    return 1;   // one dw_mmc slot per controller instance
}

_Use_decl_annotations_
VOID
RkdwmmcGetSlotCapabilities(
    PVOID PrivateExtension,
    PSDPORT_CAPABILITIES Capabilities
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;

    RtlZeroMemory(Capabilities, sizeof(*Capabilities));

    //
    // Base clock and limits. The SD card slot is 4-bit, up to high-speed/UHS-I
    // depending on board signaling. Conservative v1 advertisement.
    //
    Capabilities->BaseClockFrequencyKhz = slot->CiuClockHz / 1000;
    Capabilities->MaximumOutstandingRequests = 1;
    Capabilities->MaximumBlockSize = 512;
    Capabilities->MaximumBlockCount = 0xFFFF;

    Capabilities->Supported.ScatterGatherDma = FALSE;   // PIO v1
    Capabilities->Supported.Address64Bit = FALSE;
    Capabilities->Supported.BusWidth8Bit = FALSE;
    Capabilities->Supported.HighSpeed = TRUE;
    Capabilities->Supported.SignalingVoltage18V = TRUE;

    //
    // Voltages: 3.3V always; 1.8V for UHS signaling.
    //
    Capabilities->Supported.Voltage33V = TRUE;
    Capabilities->Supported.Voltage18V = TRUE;
}

_Use_decl_annotations_
NTSTATUS
RkdwmmcSlotInitialize(
    PVOID PrivateExtension,
    PHYSICAL_ADDRESS PhysicalBase,
    PVOID VirtualBase,
    ULONG Length,
    BOOLEAN CrashdumpMode
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;

    UNREFERENCED_PARAMETER(CrashdumpMode);

    RtlZeroMemory(slot, sizeof(*slot));
    slot->Regs = (volatile UCHAR *)VirtualBase;
    slot->RegsPhysical = PhysicalBase;
    slot->RegsLength = Length;
    slot->CiuClockHz = RKDWMMC_CIU_CLOCK_HZ;

    DwmmcInitController(slot);

    RkLog(RK_DBG_INFO, "SlotInitialize @ 0x%llx fifo@0x%x\n",
          PhysicalBase.QuadPart, slot->FifoOffset);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkdwmmcIssueBusOperation(
    PVOID PrivateExtension,
    PSDPORT_BUS_OPERATION BusOperation
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;

    switch (BusOperation->Type) {
    case SdResetHost:
        return DwmmcResetAll(slot);

    case SdSetClock:
        //
        // sdport passes the target frequency in kHz.
        //
        return DwmmcSetClock(slot, BusOperation->Parameters.FrequencyKhz * 1000);

    case SdSetVoltage:
        //
        // VDD/VCCQ switching is handled by the board PMIC/regulator path, not
        // the dw_mmc core. Accept and let firmware/PMIC own it.
        //
        return STATUS_SUCCESS;

    case SdSetBusWidth:
        DwmmcSetBusWidth(slot, BusOperation->Parameters.BusWidth);
        return STATUS_SUCCESS;

    case SdSetBusSpeed:
        //
        // Speed mode (HS/SDR/DDR) tuning beyond the clock divider is not done in
        // v1; the divider was already set via SdSetClock.
        //
        return STATUS_SUCCESS;

    case SdSetSignalingVoltage:
        return STATUS_SUCCESS;

    default:
        //
        // Unimplemented operations (drive strength, preset, tuning) are accepted
        // as no-ops so bring-up proceeds.
        //
        return STATUS_SUCCESS;
    }
}

_Use_decl_annotations_
BOOLEAN
RkdwmmcGetCardDetectState(
    PVOID PrivateExtension
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;

    //
    // The ACPI _CRS routes card-detect through a GPIO (GpioInt on \_SB.GPI0),
    // so sdport may track presence itself. As a fallback we read the dw_mmc
    // CDETECT register: bit0 == 0 means a card is present.
    //
    return (DwmmcRead(slot->Regs, DWMMC_CDETECT) & 1u) == 0;
}

_Use_decl_annotations_
BOOLEAN
RkdwmmcGetWriteProtectState(
    PVOID PrivateExtension
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;

    //
    // WRTPRT bit0 == 1 means write-protected.
    //
    return (DwmmcRead(slot->Regs, DWMMC_WRTPRT) & 1u) != 0;
}

//
// Build the dw_mmc CMD register bits from an sdport command descriptor.
//
static
ULONG
RkdwmmcBuildCmd(
    _In_ PSDPORT_COMMAND Command
    )
{
    ULONG cmd = DWMMC_CMD_INDX(Command->Index) | DWMMC_CMD_PRV_DAT_WAIT;

    //
    // Response.
    //
    switch (Command->ResponseType) {
    case SdResponseTypeNone:
        break;
    case SdResponseTypeR2:
        cmd |= DWMMC_CMD_RESP_EXP | DWMMC_CMD_RESP_LONG | DWMMC_CMD_RESP_CRC;
        break;
    case SdResponseTypeR3:
    case SdResponseTypeR4:
        cmd |= DWMMC_CMD_RESP_EXP;            // no CRC
        break;
    default:                                   // R1, R1B, R5, R6, R7
        cmd |= DWMMC_CMD_RESP_EXP | DWMMC_CMD_RESP_CRC;
        break;
    }

    //
    // Data.
    //
    if (Command->TransferType != SdTransferTypeNone) {
        cmd |= DWMMC_CMD_DAT_EXP;
        if (Command->TransferDirection == SdTransferDirectionWrite) {
            cmd |= DWMMC_CMD_DAT_WR;
        }
        if (Command->TransferType == SdTransferTypeMultiBlock) {
            cmd |= DWMMC_CMD_SEND_STOP;        // auto CMD12
        }
    }

    //
    // CMD0 needs the 80-clock init sequence.
    //
    if (Command->Index == 0) {
        cmd |= DWMMC_CMD_INIT;
    }

    return cmd;
}

_Use_decl_annotations_
NTSTATUS
RkdwmmcIssueRequest(
    PVOID PrivateExtension,
    PSDPORT_REQUEST Request
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    PSDPORT_COMMAND command = &Request->Command;
    ULONG cmd;

    //
    // Program the data transfer if this command moves a payload, and stash the
    // PIO buffer/length for the interrupt pump.
    //
    if (command->TransferType != SdTransferTypeNone) {
        ULONG blockSize = command->BlockSize;
        ULONG blockCount = command->BlockCount;

        DwmmcWrite(slot->Regs, DWMMC_RINTSTS, 0xFFFFFFFF);
        DwmmcSetBlockConfig(slot, blockSize, blockCount);

        slot->DataBuffer = (PUCHAR)command->DataBuffer;
        slot->DataLength = blockSize * blockCount;
        slot->DataTransferred = 0;
        slot->DataWrite = (command->TransferDirection == SdTransferDirectionWrite);

        //
        // Pre-load the TX FIFO for writes so the controller has data to clock;
        // the rest is pushed on TXDR interrupts.
        //
        if (slot->DataWrite) {
            slot->DataTransferred =
                DwmmcWriteFifo(slot, slot->DataBuffer, slot->DataLength);
        }
    } else {
        slot->DataBuffer = NULL;
        slot->DataLength = 0;
    }

    cmd = RkdwmmcBuildCmd(command);
    return DwmmcSendCommand(slot, cmd, command->Argument);
}

_Use_decl_annotations_
VOID
RkdwmmcGetResponse(
    PVOID PrivateExtension,
    PSDPORT_COMMAND Command,
    PVOID ResponseBuffer
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    PULONG resp = (PULONG)ResponseBuffer;

    if (Command->ResponseType == SdResponseTypeR2) {
        //
        // 136-bit response. dw_mmc RESP0..3 hold bits [31:0]..[127:96].
        // sdport's R2 buffer convention may require a one-byte shift (the
        // SDHCI/SD spec drops the CRC) — verify against a known CID/CSD on
        // hardware. VERIFY-ON-BUILD.
        //
        resp[0] = DwmmcRead(slot->Regs, DWMMC_RESP0);
        resp[1] = DwmmcRead(slot->Regs, DWMMC_RESP1);
        resp[2] = DwmmcRead(slot->Regs, DWMMC_RESP2);
        resp[3] = DwmmcRead(slot->Regs, DWMMC_RESP3);
    } else if (Command->ResponseType != SdResponseTypeNone) {
        resp[0] = DwmmcRead(slot->Regs, DWMMC_RESP0);
    }
}

_Use_decl_annotations_
BOOLEAN
RkdwmmcInterrupt(
    PVOID PrivateExtension,
    PULONG Events,
    PULONG Errors,
    PBOOLEAN NotifyCardChange,
    PBOOLEAN NotifySdioInterrupt,
    PBOOLEAN NotifyTuning
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    ULONG status = DwmmcRead(slot->Regs, DWMMC_MINTSTS);
    ULONG events = 0;
    ULONG errors = 0;

    *NotifySdioInterrupt = FALSE;
    *NotifyTuning = FALSE;
    *NotifyCardChange = FALSE;

    if (status == 0) {
        return FALSE;   // not ours
    }

    //
    // ---- dw_mmc RINTSTS  ->  sdport event/error mapping ----
    // VERIFY-ON-BUILD: the SDPORT_EVENT_* / SDPORT_ERROR_* names below are the
    // generic sdport set; confirm spelling/values against your <sdport.h>.
    //
    if (status & DWMMC_INT_CD) {
        *NotifyCardChange = TRUE;
        DwmmcWrite(slot->Regs, DWMMC_RINTSTS, DWMMC_INT_CD);
    }

    if (status & DWMMC_CMD_ERROR_BITS) {
        errors |= SDPORT_ERROR_CMD_TIMEOUT;     // map RTO/RCRC/RESP_ERR
    }
    if (status & DWMMC_DATA_ERROR_BITS) {
        errors |= SDPORT_ERROR_DATA_TIMEOUT;    // map data CRC/timeout/under/over
    }

    if (status & DWMMC_INT_CMD_DONE) {
        events |= SDPORT_EVENT_CARD_RESPONSE;
    }

    //
    // PIO pump. Read or write the FIFO as the watermark interrupts fire, and on
    // DATA_OVER drain whatever remains.
    //
    if ((status & DWMMC_INT_RXDR) || (status & DWMMC_INT_DATA_OVER)) {
        if (slot->DataBuffer != NULL && !slot->DataWrite &&
            slot->DataTransferred < slot->DataLength) {
            ULONG remaining = slot->DataLength - slot->DataTransferred;
            slot->DataTransferred +=
                DwmmcReadFifo(slot, slot->DataBuffer + slot->DataTransferred, remaining);
        }
    }
    if (status & DWMMC_INT_TXDR) {
        if (slot->DataBuffer != NULL && slot->DataWrite &&
            slot->DataTransferred < slot->DataLength) {
            ULONG remaining = slot->DataLength - slot->DataTransferred;
            slot->DataTransferred +=
                DwmmcWriteFifo(slot, slot->DataBuffer + slot->DataTransferred, remaining);
        }
        events |= SDPORT_EVENT_BUFFER_WRITE_READY;
    }

    if (status & DWMMC_INT_DATA_OVER) {
        events |= SDPORT_EVENT_CARD_RW_END;
    }

    //
    // Acknowledge everything we examined.
    //
    DwmmcWrite(slot->Regs, DWMMC_RINTSTS, status);

    *Events = events;
    *Errors = errors;
    return TRUE;
}

_Use_decl_annotations_
VOID
RkdwmmcRequestDpc(
    PVOID PrivateExtension,
    PSDPORT_REQUEST Request,
    ULONG Events,
    ULONG Errors
    )
{
    //
    // sdport drives request progress from the events/errors collected by the
    // Interrupt callback. With the simple PIO model the heavy lifting already
    // happened in the ISR; here we let sdport complete the request.
    //
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Events);
    UNREFERENCED_PARAMETER(Errors);
}

_Use_decl_annotations_
VOID
RkdwmmcToggleEvents(
    PVOID PrivateExtension,
    ULONG EventMask,
    BOOLEAN Enable
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    ULONG mask = DwmmcRead(slot->Regs, DWMMC_INTMASK);

    //
    // sdport asks us to enable/disable classes of events. v1 keeps a broad,
    // always-on dw_mmc INTMASK (command, data, error, RX/TX, CD) and treats the
    // sdport mask as advisory. VERIFY-ON-BUILD if finer gating is needed.
    //
    UNREFERENCED_PARAMETER(EventMask);

    if (Enable) {
        mask |= (DWMMC_INT_CMD_DONE | DWMMC_INT_DATA_OVER | DWMMC_INT_RXDR |
                 DWMMC_INT_TXDR | DWMMC_INT_CD | DWMMC_INT_ERROR);
    }
    DwmmcWrite(slot->Regs, DWMMC_INTMASK, mask);
}

_Use_decl_annotations_
VOID
RkdwmmcClearEvents(
    PVOID PrivateExtension,
    ULONG EventMask
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    UNREFERENCED_PARAMETER(EventMask);

    //
    // RINTSTS is write-1-to-clear; clear everything we may have latched.
    //
    DwmmcWrite(slot->Regs, DWMMC_RINTSTS, 0xFFFFFFFF);
}

_Use_decl_annotations_
VOID
RkdwmmcSaveContext(
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
}

_Use_decl_annotations_
VOID
RkdwmmcRestoreContext(
    PVOID PrivateExtension
    )
{
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    DwmmcInitController(slot);   // re-establish controller state after D-state
}

_Use_decl_annotations_
VOID
RkdwmmcCleanup(
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    SDPORT_INITIALIZATION_DATA init;

    RkLog(RK_DBG_INFO, "DriverEntry\n");

    RtlZeroMemory(&init, sizeof(init));
    init.StructureSize = sizeof(init);

    init.GetSlotCount = RkdwmmcGetSlotCount;
    init.GetSlotCapabilities = RkdwmmcGetSlotCapabilities;
    init.Initialize = RkdwmmcSlotInitialize;
    init.IssueBusOperation = RkdwmmcIssueBusOperation;
    init.GetCardDetectState = RkdwmmcGetCardDetectState;
    init.GetWriteProtectState = RkdwmmcGetWriteProtectState;
    init.IssueRequest = RkdwmmcIssueRequest;
    init.GetResponse = RkdwmmcGetResponse;
    init.Interrupt = RkdwmmcInterrupt;
    init.RequestDpc = RkdwmmcRequestDpc;
    init.ToggleEvents = RkdwmmcToggleEvents;
    init.ClearEvents = RkdwmmcClearEvents;
    init.SaveContext = RkdwmmcSaveContext;
    init.RestoreContext = RkdwmmcRestoreContext;
    init.Cleanup = RkdwmmcCleanup;

    init.PrivateExtensionSize = sizeof(RKDWMMC_SLOT);
    init.CrashDumpSupported = FALSE;

    return SdPortInitialize(DriverObject, RegistryPath, &init);
}
