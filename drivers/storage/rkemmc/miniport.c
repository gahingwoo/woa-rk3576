/*++

Module Name:

    miniport.c

Abstract:

    SDPORT_* callbacks for the RK3576 eMMC (DWCMSHC).  sdport.sys owns PnP,
    power, the interrupt and the SD/eMMC protocol; this file maps what it asks
    for onto the register engine in hw.c.

    The structure deliberately mirrors ../rkdwmmc/miniport.c, which is the SD
    slot's driver and the only sdport miniport in this project that has been
    built and loaded.  Where the two differ it is because the hardware differs:
    dw_mmc is not SDHCI, this controller is.

Environment:

    Kernel mode.

--*/

#include "rkemmc.h"
#include <ntstrsafe.h>

RKEMMC_DIAG g_RkDiag;

static HANDLE g_RkDiagKey = NULL;

/*++

Routine Description:

    Publish the diagnostic snapshot under the service key.

    PASSIVE_LEVEL only -- registry calls cannot be made above it.  Callers at
    raised IRQL accumulate into g_RkDiag and leave publishing to whichever
    passive-level callback runs next.  Flushing on every command was tried on
    the SD driver and is too expensive to leave in the request path, so this is
    called from the low-frequency callbacks only.

--*/
VOID
RkemmcDiagFlush(
    VOID
    )
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attrs;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    if (g_RkDiagKey == NULL) {
        RtlInitUnicodeString(
            &name,
            L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\rkemmc\\Diag");
        InitializeObjectAttributes(&attrs, &name,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        status = ZwCreateKey(&g_RkDiagKey, KEY_SET_VALUE, &attrs, 0, NULL,
                             REG_OPTION_NON_VOLATILE, NULL);
        if (!NT_SUCCESS(status)) {
            g_RkDiagKey = NULL;
            return;
        }
    }

#define RK_DIAG_PUT(_name, _field)                                            \
    do {                                                                      \
        UNICODE_STRING _v;                                                    \
        ULONG _d = g_RkDiag._field;                                           \
        RtlInitUnicodeString(&_v, L##_name);                                  \
        (VOID)ZwSetValueKey(g_RkDiagKey, &_v, 0, REG_DWORD, &_d, sizeof(_d)); \
    } while (0)

    RK_DIAG_PUT("ResetCalls",          ResetCalls);
    RK_DIAG_PUT("VendorBitsBefore",    VendorBitsBefore);
    RK_DIAG_PUT("VendorBitsAfter",     VendorBitsAfter);
    RK_DIAG_PUT("MiscConAfter",        MiscConAfter);
    RK_DIAG_PUT("ClockSetCalls",       ClockSetCalls);
    RK_DIAG_PUT("ClockRequestedHz",    ClockRequestedHz);
    RK_DIAG_PUT("ClockActualHz",       ClockActualHz);
    RK_DIAG_PUT("ClockCruValue",       ClockCruValue);
    RK_DIAG_PUT("ClockStableWaits",    ClockStableWaits);
    RK_DIAG_PUT("ClockStableTimeouts", ClockStableTimeouts);
    RK_DIAG_PUT("BusOpCalls",          BusOpCalls);
    RK_DIAG_PUT("BusOpLastType",       BusOpLastType);
    RK_DIAG_PUT("RequestCalls",        RequestCalls);
    RK_DIAG_PUT("LastCmdIndex",        LastCmdIndex);
    RK_DIAG_PUT("LastCmdArg",          LastCmdArg);
    RK_DIAG_PUT("LastCmdReg",          LastCmdReg);
    RK_DIAG_PUT("LastCmdStatus",       LastCmdStatus);
    RK_DIAG_PUT("InterruptCalls",      InterruptCalls);
    RK_DIAG_PUT("LastIntStatus",       LastIntStatus);
    RK_DIAG_PUT("SeenIntStatus",       SeenIntStatus);
    RK_DIAG_PUT("SeenErrStatus",       SeenErrStatus);
    RK_DIAG_PUT("CmdErrors",           CmdErrors);
    RK_DIAG_PUT("DataErrors",          DataErrors);
    RK_DIAG_PUT("BaseClockKhz",        BaseClockKhz);
    RK_DIAG_PUT("HostVersion",         HostVersion);
    RK_DIAG_PUT("Capabilities",        Capabilities);
    RK_DIAG_PUT("TraceCount",          TraceCount);

#undef RK_DIAG_PUT

    //
    // The ring, one value per command: Cmd00, Cmd01, ...  Written as separate
    // values rather than one blob so `reg query /s` prints them readably on a
    // board with no tools.
    //
    {
        ULONG i;
        ULONG n = (g_RkDiag.TraceCount < RKEMMC_TRACE_DEPTH)
                      ? g_RkDiag.TraceCount : RKEMMC_TRACE_DEPTH;

        for (i = 0; i < n; i++) {
            WCHAR          nameBuf[8];
            UNICODE_STRING valueName;
            ULONG          data = g_RkDiag.Trace[i];

            (VOID)RtlStringCchPrintfW(nameBuf, RTL_NUMBER_OF(nameBuf), L"Cmd%02u", i);
            RtlInitUnicodeString(&valueName, nameBuf);
            (VOID)ZwSetValueKey(g_RkDiagKey, &valueName, 0, REG_DWORD,
                                &data, sizeof(data));
        }
    }
}

_Use_decl_annotations_
NTSTATUS
RkemmcGetSlotCount(
    PSD_MINIPORT Miniport,
    PUCHAR SlotCount
    )
{
    UNREFERENCED_PARAMETER(Miniport);

    *SlotCount = 1;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
RkemmcGetSlotCapabilities(
    PVOID PrivateExtension,
    PSDPORT_CAPABILITIES Capabilities
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;

    RtlZeroMemory(Capabilities, sizeof(*Capabilities));

    //
    // Advertise the clock this driver will actually deliver, which is not what
    // the capability register reports.
    //
    // The register is not wrong -- CAPS0 reads 0x3A6DC881 on this board, whose
    // base-clock field is 0xC8 = 200 MHz, and that is a real number (mainline
    // sets SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN on every dwcmshc variant, which
    // is conservative here).  It is simply not this driver's limit: v1 stops
    // at eMMC high speed because it does not tune the DLL.  sdport will not
    // ask for more than what is reported, which is the point.
    Capabilities->BaseClockFrequencyKhz = RKEMMC_MAX_CLOCK_HZ / 1000;
    Capabilities->MaximumOutstandingRequests = 1;
    Capabilities->MaximumBlockSize = 512;
    Capabilities->MaximumBlockCount = 0xFFFF;

    Capabilities->Supported.ScatterGatherDma = FALSE;   // PIO in v1
    Capabilities->Supported.Address64Bit = FALSE;
    Capabilities->Supported.BusWidth8Bit = TRUE;        // eMMC is 8-bit here
    Capabilities->Supported.HighSpeed = TRUE;

    //
    // HS200/HS400 are deliberately not offered.  They need the DLL locked and
    // a tuning pass, and this driver bypasses the DLL below 52 MHz precisely
    // so that no tuning is required.  Claiming them would get the card
    // switched into a timing the host cannot sample.
    //
    //
    // 3.0 V only, which is what the controller claims: CAPS0 reads 0x3A6DC881
    // on this board and its voltage bits say 3.0 V supported, 3.3 V and 1.8 V
    // not.  Linux agrees -- it leaves POWER_CONTROL at 0x0D, 3.0 V with bus
    // power on.  Advertising 3.3 V, as this did, invites sdport to ask for a
    // voltage the hardware does not have.
    //
    Capabilities->Supported.SignalingVoltage18V = FALSE;
    Capabilities->Supported.Voltage33V = FALSE;
    Capabilities->Supported.Voltage30V = TRUE;
    Capabilities->Supported.Voltage18V = FALSE;

    g_RkDiag.BaseClockKhz = Capabilities->BaseClockFrequencyKhz;
    RkemmcDiagFlush();

    RkLog(RK_DBG_INFO, "Capabilities: base=%u kHz ver=0x%04x caps=0x%08x\n",
          Capabilities->BaseClockFrequencyKhz,
          g_RkDiag.HostVersion, g_RkDiag.Capabilities);

    UNREFERENCED_PARAMETER(slot);
}

_Use_decl_annotations_
NTSTATUS
RkemmcSlotInitialize(
    PVOID PrivateExtension,
    PHYSICAL_ADDRESS PhysicalBase,
    PVOID VirtualBase,
    ULONG Length,
    BOOLEAN CrashdumpMode
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;
    PHYSICAL_ADDRESS cru;

    UNREFERENCED_PARAMETER(CrashdumpMode);

    RtlZeroMemory(slot, sizeof(*slot));
    slot->Regs = (volatile UCHAR *)VirtualBase;
    slot->RegsPhysical = PhysicalBase;
    slot->RegsLength = Length;

    //
    // Map CCLK_SRC_EMMC.  It is one register in the CRU, outside this device's
    // _CRS, and there is no way to reach the ACPI _DSM that sets it from an
    // sdport miniport -- sdport hands out a mapped window and keeps the device
    // object to itself.
    //
    // If the mapping fails the driver still loads: the firmware leaves the
    // clock at a usable rate, so a card can still enumerate at whatever speed
    // it was left at.  That is worth having rather than failing to start.
    //
    cru.QuadPart = RK3576_CRU_CLKSEL_CON89;
    slot->CruClkSel = (volatile ULONG *)MmMapIoSpaceEx(cru, sizeof(ULONG),
                                                       PAGE_READWRITE | PAGE_NOCACHE);
    if (slot->CruClkSel == NULL) {
        RkLog(RK_DBG_ERROR, "could not map CRU 0x%08x; clock stays as firmware left it\n",
              RK3576_CRU_CLKSEL_CON89);
    }

    EmmcInitController(slot);

    RkLog(RK_DBG_INFO, "SlotInitialize @ 0x%llx len=0x%x ver=0x%04x\n",
          PhysicalBase.QuadPart, Length, g_RkDiag.HostVersion);

    //
    // Deliberately no RkemmcDiagFlush() here.  This is the earliest callback
    // and, for a boot-start driver, one of the earliest things that runs at
    // all; opening and writing a registry key from it is a risk for no gain,
    // because GetSlotCapabilities publishes the same snapshot moments later.
    // The SD driver does not flush here either.
    //
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkemmcIssueBusOperation(
    PVOID PrivateExtension,
    PSDPORT_BUS_OPERATION BusOperation
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;

    g_RkDiag.BusOpCalls++;
    g_RkDiag.BusOpLastType = BusOperation->Type;

    RkLog(RK_DBG_INFO, "BusOperation type=%u\n", BusOperation->Type);

    switch (BusOperation->Type) {
    case SdResetHost:
        //
        // The whole reason this driver exists.  EmmcResetAll does the SDHCI
        // reset and puts the three vendor bits back; the inbox driver does the
        // first half only, which is why a card never appears under it.
        //
        {
            NTSTATUS status = EmmcResetAll(slot);
            EmmcInitController(slot);
            RkemmcDiagFlush();
            return status;
        }

    case SdSetClock:
        {
            NTSTATUS status = EmmcSetClock(slot,
                                  BusOperation->Parameters.FrequencyKhz * 1000);
            RkemmcDiagFlush();
            return status;
        }

    case SdSetVoltage:
        //
        // The eMMC rail on CM5-IO is fixed and always on; there is nothing to
        // switch.  Drive the SDHCI power register anyway so the controller's
        // own state machine is where it expects to be.
        //
        EmmcSetPower(slot, TRUE,
                     BusOperation->Parameters.Voltage != SdBusVoltage33);
        return STATUS_SUCCESS;

    case SdSetBusWidth:
        EmmcSetBusWidth(slot, BusOperation->Parameters.BusWidth);
        return STATUS_SUCCESS;

    case SdSetBusSpeed:
        //
        // Rate was already applied by SdSetClock.  What is left is the timing
        // mode in HOST_CONTROL2 and the high-speed bit.
        //
        {
            UCHAR ctrl = EmmcRead8(slot, SDHCI_HOST_CONTROL);

            if (slot->CurrentClockHz > 26000000UL) {
                ctrl |= SDHCI_CTRL_HISPD;
            } else {
                ctrl &= (UCHAR)~SDHCI_CTRL_HISPD;
            }
            EmmcWrite8(slot, SDHCI_HOST_CONTROL, ctrl);

            EmmcSetUhsMode(slot, SDHCI_CTRL_UHS_SDR25);
        }
        return STATUS_SUCCESS;

    case SdSetSignalingVoltage:
        //
        // 1.8 V signalling is not offered in GetSlotCapabilities, so sdport
        // should not ask.  Refuse rather than silently succeed: a host that
        // reports a voltage switch it did not perform leaves the card and the
        // host disagreeing about the bus.
        //
        if (BusOperation->Parameters.SignalingVoltage == SdSignalingVoltage18) {
            return STATUS_NOT_SUPPORTED;
        }
        return STATUS_SUCCESS;

    default:
        //
        // Drive strength, presets and tuning are accepted as no-ops so
        // bring-up proceeds.
        //
        return STATUS_SUCCESS;
    }
}

_Use_decl_annotations_
BOOLEAN
RkemmcGetCardDetectState(
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);

    //
    // The eMMC is soldered down.  There is no detect line and the SDHCI
    // CARD_PRESENT bit is not wired to anything meaningful, so answering from
    // the register would be reading an undefined bit and calling it evidence.
    //
    return TRUE;
}

_Use_decl_annotations_
BOOLEAN
RkemmcGetWriteProtectState(
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    return FALSE;   // no write-protect line on a soldered eMMC
}

/*++

Routine Description:

    Build the SDHCI COMMAND and TRANSFER_MODE register values for one sdport
    request.

--*/
static
VOID
RkemmcBuildCmd(
    _In_ PSDPORT_COMMAND Command,
    _Out_ PUSHORT CommandReg,
    _Out_ PUSHORT TransferMode
    )
{
    USHORT flags = 0;
    USHORT mode = 0;

    switch (Command->ResponseType) {
    case SdResponseTypeNone:
        flags = SDHCI_CMD_RESP_NONE;
        break;
    case SdResponseTypeR2:
        flags = SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC;
        break;
    case SdResponseTypeR3:
    case SdResponseTypeR4:
        //
        // R3/R4 carry no CRC and no index; checking either rejects a good
        // response.
        //
        flags = SDHCI_CMD_RESP_SHORT;
        break;
    case SdResponseTypeR1B:
    case SdResponseTypeR5B:
        flags = SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX;
        break;
    default:
        flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX;
        break;
    }

    if (Command->TransferType != SdTransferTypeNone) {
        flags |= SDHCI_CMD_DATA;

        mode |= SDHCI_TRNS_BLK_CNT_EN;

        if (Command->TransferDirection == SdTransferDirectionRead) {
            mode |= SDHCI_TRNS_READ;
        }

        if (Command->BlockCount > 1) {
            mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
        }
    }

    *CommandReg = SDHCI_MAKE_CMD(Command->Index, flags);
    *TransferMode = mode;
}

_Use_decl_annotations_
NTSTATUS
RkemmcIssueRequest(
    PVOID PrivateExtension,
    PSDPORT_REQUEST Request
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;
    PSDPORT_COMMAND command = &Request->Command;
    USHORT cmdReg;
    USHORT mode;
    NTSTATUS status;

    RkemmcBuildCmd(command, &cmdReg, &mode);

    if (command->TransferType != SdTransferTypeNone) {
        ULONG blockSize = command->BlockSize;
        ULONG blockCount = command->BlockCount;

        EmmcWrite16(slot, SDHCI_BLOCK_SIZE, (USHORT)(blockSize & 0x0FFF));
        EmmcWrite16(slot, SDHCI_BLOCK_COUNT, (USHORT)blockCount);

        slot->DataBuffer = (PUCHAR)command->DataBuffer;
        slot->DataLength = blockSize * blockCount;
        slot->DataTransferred = 0;
        slot->DataWrite = (command->TransferDirection == SdTransferDirectionWrite);
    } else {
        slot->DataBuffer = NULL;
        slot->DataLength = 0;
        slot->DataTransferred = 0;
        slot->DataWrite = FALSE;
    }

    status = EmmcSendCommand(slot, cmdReg, mode, command->Argument,
                             command->TransferType != SdTransferTypeNone);

    g_RkDiag.RequestCalls++;
    g_RkDiag.LastCmdIndex  = command->Index;
    g_RkDiag.LastCmdArg    = command->Argument;
    g_RkDiag.LastCmdReg    = cmdReg;
    g_RkDiag.LastCmdStatus = (ULONG)status;

    //
    // Open a trace slot for this command.  The interrupt and the DPC fill in
    // what happened to it; a slot that stays at 0 in both status bytes is a
    // command that was issued and never answered, which is a different fault
    // from one that was never issued at all.
    //
    if (g_RkDiag.TraceCount < RKEMMC_TRACE_DEPTH) {
        g_RkDiag.Trace[g_RkDiag.TraceCount] =
            ((g_RkDiag.TraceCount & 0xFF) << 24) | ((command->Index & 0xFF) << 16);
    }

    g_RkDiag.TraceCount++;
    // No flush: this is the command path and the SD driver showed that a
    // registry write per command is far too expensive to sit here.

    RkLog(RK_DBG_INFO, "CMD%u arg=0x%08x resp=%u xfer=%u cmd=0x%04x mode=0x%04x -> 0x%08x\n",
          command->Index, command->Argument, command->ResponseType,
          command->TransferType, cmdReg, mode, status);

    return status;
}

_Use_decl_annotations_
VOID
RkemmcGetResponse(
    PVOID PrivateExtension,
    PSDPORT_COMMAND Command,
    PVOID ResponseBuffer
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;
    PUCHAR out = (PUCHAR)ResponseBuffer;

    if (Command->ResponseType == SdResponseTypeR2) {
        //
        // 136-bit response.  SDHCI presents it already CRC-stripped and
        // shifted right by 8 bits across RESPONSE[0..3], so the bytes come out
        // in order -- unlike dw_mmc, where the same question is still open in
        // the SD driver.  Copy 15 bytes and leave the top one zero, which is
        // the CRC slot the controller removed.
        //
        ULONG i;

        for (i = 0; i < 4; i++) {
            ULONG word = EmmcRead32(slot, SDHCI_RESPONSE + (i * 4));
            RtlCopyMemory(out + (i * 4), &word, sizeof(word));
        }
    } else if (Command->ResponseType != SdResponseTypeNone) {
        ULONG word = EmmcRead32(slot, SDHCI_RESPONSE);
        RtlCopyMemory(out, &word, sizeof(word));
    }
}

_Use_decl_annotations_
BOOLEAN
RkemmcInterrupt(
    PVOID PrivateExtension,
    PULONG Events,
    PULONG Errors,
    PBOOLEAN NotifyCardChange,
    PBOOLEAN NotifySdioInterrupt,
    PBOOLEAN NotifyTuning
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;
    USHORT intStatus = EmmcRead16(slot, SDHCI_INT_STATUS);
    USHORT errStatus = 0;
    ULONG events = 0;
    ULONG errors = 0;

    *NotifyCardChange = FALSE;
    *NotifySdioInterrupt = FALSE;
    *NotifyTuning = FALSE;

    if (intStatus == 0) {
        return FALSE;   // not ours
    }

    //
    // DIRQL here: accumulate only.  RkemmcDiagFlush is a no-op above
    // PASSIVE_LEVEL and the next passive callback publishes these.
    //
    g_RkDiag.InterruptCalls++;
    g_RkDiag.LastIntStatus = intStatus;
    g_RkDiag.SeenIntStatus |= intStatus;

    //
    // Fold what the controller reported into the slot the in-flight command
    // opened.  Only the low byte of each status register: the bits that
    // distinguish the outcomes all live there.
    //
    if ((g_RkDiag.TraceCount > 0) &&
        (g_RkDiag.TraceCount <= RKEMMC_TRACE_DEPTH))
    {
        g_RkDiag.Trace[g_RkDiag.TraceCount - 1] |= (intStatus & 0xFF);
    }

    if (intStatus & SDHCI_INT_ERROR) {
        errStatus = EmmcRead16(slot, SDHCI_ERR_INT_STATUS);
        g_RkDiag.SeenErrStatus |= errStatus;

        if ((g_RkDiag.TraceCount > 0) &&
            (g_RkDiag.TraceCount <= RKEMMC_TRACE_DEPTH))
        {
            g_RkDiag.Trace[g_RkDiag.TraceCount - 1] |= ((errStatus & 0xFF) << 8);
        }

        if (errStatus & SDHCI_ERR_CMD_MASK) {
            g_RkDiag.CmdErrors++;
            errors |= (errStatus & SDHCI_ERR_CMD_TIMEOUT)
                          ? SDPORT_ERROR_CMD_TIMEOUT
                          : SDPORT_ERROR_CMD_CRC_ERROR;
        }
        if (errStatus & SDHCI_ERR_DATA_MASK) {
            g_RkDiag.DataErrors++;
            errors |= (errStatus & SDHCI_ERR_DATA_TIMEOUT)
                          ? SDPORT_ERROR_DATA_TIMEOUT
                          : SDPORT_ERROR_DATA_CRC_ERROR;
        }

        //
        // The SD spec's recovery: reset the line that failed before anything
        // else is issued, or the next command inherits the stuck state.  Note
        // this is the CMD/DAT reset, not RESET_ALL, so the vendor bits are
        // not disturbed.
        //
        if (errStatus & SDHCI_ERR_CMD_MASK) {
            (VOID)EmmcResetCmdDat(slot, SDHCI_RESET_CMD);
        }
        if (errStatus & SDHCI_ERR_DATA_MASK) {
            (VOID)EmmcResetCmdDat(slot, SDHCI_RESET_DATA);
        }

        EmmcWrite16(slot, SDHCI_ERR_INT_STATUS, errStatus);
    }

    if (intStatus & SDHCI_INT_RESPONSE) {
        events |= SDPORT_EVENT_CARD_RESPONSE;
    }

    //
    // PIO pump.  sdport names its buffer events after the FIFO state:
    // BUFFER_FULL means data is waiting to be read, BUFFER_EMPTY means the
    // controller wants more.
    //
    if (intStatus & SDHCI_INT_DATA_AVAIL) {
        if (slot->DataBuffer != NULL && !slot->DataWrite &&
            slot->DataTransferred < slot->DataLength) {
            slot->DataTransferred +=
                EmmcReadBuffer(slot,
                               slot->DataBuffer + slot->DataTransferred,
                               slot->DataLength - slot->DataTransferred);
        }
        events |= SDPORT_EVENT_BUFFER_FULL;
    }

    if (intStatus & SDHCI_INT_SPACE_AVAIL) {
        if (slot->DataBuffer != NULL && slot->DataWrite &&
            slot->DataTransferred < slot->DataLength) {
            slot->DataTransferred +=
                EmmcWriteBuffer(slot,
                                slot->DataBuffer + slot->DataTransferred,
                                slot->DataLength - slot->DataTransferred);
        }
        events |= SDPORT_EVENT_BUFFER_EMPTY;
    }

    if (intStatus & SDHCI_INT_DATA_END) {
        //
        // Drain whatever the watermark interrupts did not cover.
        //
        if (slot->DataBuffer != NULL && !slot->DataWrite &&
            slot->DataTransferred < slot->DataLength) {
            slot->DataTransferred +=
                EmmcReadBuffer(slot,
                               slot->DataBuffer + slot->DataTransferred,
                               slot->DataLength - slot->DataTransferred);
        }
        events |= SDPORT_EVENT_CARD_RW_END;
    }

    if (intStatus & (SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE)) {
        //
        // Should never fire on a soldered eMMC, but acknowledge it so a stuck
        // bit cannot storm the line.
        //
        *NotifyCardChange = TRUE;
    }

    EmmcWrite16(slot, SDHCI_INT_STATUS, intStatus);

    *Events = events;
    *Errors = errors;
    return TRUE;
}

_Use_decl_annotations_
VOID
RkemmcRequestDpc(
    PVOID PrivateExtension,
    PSDPORT_REQUEST Request,
    ULONG Events,
    ULONG Errors
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;
    NTSTATUS status;

    //
    // This used to do nothing, on the assumption that sdport completes a
    // request from the events the ISR reports.  It does not: the miniport
    // owns the end of the request and has to call SdPortCompleteRequest.
    //
    // Measured on CM5-IO 2026-09-20, which is what found it.  The driver
    // issued exactly one command, CMD0.  The controller answered, the
    // interrupt arrived, SDPORT_EVENT_CARD_RESPONSE was reported, and
    // SeenErrStatus stayed 0 -- and sdport never asked for anything again.
    // A stack waiting forever on a request nobody finished looks exactly like
    // that.  The SD driver stops the same way three bus operations in.
    //
    if (Errors != 0) {
        //
        // The line that failed has already been reset in the ISR, per the SD
        // spec's error recovery.  Map to something the class driver can act
        // on: a timeout is a missing card or a wedged bus, a CRC error is
        // worth retrying.
        //
        status = ((Errors & (SDPORT_ERROR_CMD_TIMEOUT | SDPORT_ERROR_DATA_TIMEOUT)) != 0)
                     ? STATUS_IO_TIMEOUT
                     : STATUS_CRC_ERROR;

        g_RkDiag.LastCmdStatus = (ULONG)status;
        SdPortCompleteRequest(Request, status);
        return;
    }

    //
    // A command with no payload is finished once its response has arrived.
    //
    if (Request->Command.TransferType == SdTransferTypeNone) {
        if ((Events & SDPORT_EVENT_CARD_RESPONSE) != 0) {
            SdPortCompleteRequest(Request, STATUS_SUCCESS);
        }
        return;
    }

    //
    // A data command is finished when the transfer ends.  Check the byte count
    // as well as the event: the ISR drains whatever the watermark interrupts
    // did not cover, so a short transfer that ended early is a failure rather
    // than something to keep waiting on.
    //
    if ((Events & SDPORT_EVENT_CARD_RW_END) != 0) {
        status = (slot->DataTransferred >= slot->DataLength)
                     ? STATUS_SUCCESS
                     : STATUS_DEVICE_DATA_ERROR;
        SdPortCompleteRequest(Request, status);
    }
}

_Use_decl_annotations_
VOID
RkemmcToggleEvents(
    PVOID PrivateExtension,
    ULONG EventMask,
    BOOLEAN Enable
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;

    //
    // v1 keeps a broad, always-on enable set and treats sdport's mask as
    // advisory, the same simplification the SD driver makes.  Finer gating
    // means mapping SDPORT_EVENT_* onto individual SDHCI enable bits, which is
    // only worth doing if spurious interrupts show up.
    //
    UNREFERENCED_PARAMETER(EventMask);

    if (Enable) {
        USHORT normal = SDHCI_INT_RESPONSE | SDHCI_INT_DATA_END |
                        SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL |
                        SDHCI_INT_CARD_INSERT | SDHCI_INT_CARD_REMOVE;

        EmmcWrite16(slot, SDHCI_INT_ENABLE, normal);
        EmmcWrite16(slot, SDHCI_SIGNAL_ENABLE, normal);
        EmmcWrite16(slot, SDHCI_ERR_INT_ENABLE, 0xFFFF);
        EmmcWrite16(slot, SDHCI_ERR_SIGNAL_ENABLE, 0xFFFF);
    }
}

_Use_decl_annotations_
VOID
RkemmcClearEvents(
    PVOID PrivateExtension,
    ULONG EventMask
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;
    UNREFERENCED_PARAMETER(EventMask);

    //
    // Both status registers are write-1-to-clear.
    //
    EmmcWrite16(slot, SDHCI_INT_STATUS, 0xFFFF);
    EmmcWrite16(slot, SDHCI_ERR_INT_STATUS, 0xFFFF);
}

_Use_decl_annotations_
VOID
RkemmcSaveContext(
    PVOID PrivateExtension
    )
{
    UNREFERENCED_PARAMETER(PrivateExtension);
}

_Use_decl_annotations_
VOID
RkemmcRestoreContext(
    PVOID PrivateExtension
    )
{
    PRKEMMC_SLOT slot = (PRKEMMC_SLOT)PrivateExtension;

    //
    // Coming out of a D-state the controller has been reset by hardware, so
    // the vendor bits are clear again.  EmmcInitController rebuilds
    // everything including them.
    //
    EmmcInitController(slot);
}

_Use_decl_annotations_
VOID
RkemmcCleanup(
    PSD_MINIPORT Miniport
    )
{
    UNREFERENCED_PARAMETER(Miniport);

    if (g_RkDiagKey != NULL) {
        ZwClose(g_RkDiagKey);
        g_RkDiagKey = NULL;
    }
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

    init.GetSlotCount = RkemmcGetSlotCount;
    init.GetSlotCapabilities = RkemmcGetSlotCapabilities;
    init.Initialize = RkemmcSlotInitialize;
    init.IssueBusOperation = RkemmcIssueBusOperation;
    init.GetCardDetectState = RkemmcGetCardDetectState;
    init.GetWriteProtectState = RkemmcGetWriteProtectState;
    init.IssueRequest = RkemmcIssueRequest;
    init.GetResponse = RkemmcGetResponse;
    init.Interrupt = RkemmcInterrupt;
    init.RequestDpc = RkemmcRequestDpc;
    init.ToggleEvents = RkemmcToggleEvents;
    init.ClearEvents = RkemmcClearEvents;
    init.SaveContext = RkemmcSaveContext;
    init.RestoreContext = RkemmcRestoreContext;
    init.Cleanup = RkemmcCleanup;

    init.PrivateExtensionSize = sizeof(RKEMMC_SLOT);
    init.CrashdumpSupported = FALSE;

    return SdPortInitialize(DriverObject, RegistryPath, &init);
}
