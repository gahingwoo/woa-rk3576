/*++

Module Name:

    miniport.c

Abstract:

    sdport miniport glue for the RK3576 dw_mmc SD slot. DriverEntry registers
    the SDPORT_* callback table; each callback maps an sdport operation onto the
    verified dw_mmc engine in hw.c.

    Compiled against <sdport.h> from WDK 10.0.26100. The dw_mmc register logic
    (hw.c) is verified against the kernel driver; this file is verified to the
    extent a compiler can verify it -- it has not run on silicon. The one item a
    build cannot settle is the R2 response byte order; see RkdwmmcGetResponse.

Environment:

    Kernel mode.

--*/

#include "rkdwmmc.h"
#include "sip.h"

//
// dw_mmc RINTSTS bits that make up a "command complete" and "transfer complete"
// for the sdport event mapping below.
//
#define DWMMC_CMD_ERROR_BITS   (DWMMC_INT_RTO | DWMMC_INT_RCRC | DWMMC_INT_RESP_ERR)
#define DWMMC_DATA_ERROR_BITS  (DWMMC_INT_DRTO | DWMMC_INT_DCRC | DWMMC_INT_SBE | \
                                DWMMC_INT_EBE | DWMMC_INT_FRUN | DWMMC_INT_HTO)

RKDWMMC_DIAG g_RkDiag;
static HANDLE g_RkDiagKey = NULL;

//
// Publish g_RkDiag under the driver's own service key. See rkdwmmc.h for why
// this exists instead of tracing.
//
VOID
RkdwmmcDiagFlush(
    VOID
    )
{
    UNICODE_STRING     path;
    OBJECT_ATTRIBUTES  attr;
    NTSTATUS           status;
    ULONG              disp;

    //
    // The interrupt handler updates g_RkDiag at DIRQL. Registry access needs
    // PASSIVE_LEVEL, so this is a no-op from there and the values land on the
    // next callback that is allowed to write.
    //
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    RtlInitUnicodeString(&path,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\rkdwmmc\\Diag");
    InitializeObjectAttributes(&attr, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    //
    // Open once and keep it. The first version created and closed the key on
    // every call and was flushed from the command path, so card initialisation
    // paid a create + 16 writes + close per command -- and the boot hung.
    // Same driver without it booted fine, so that cost was the difference.
    //
    if (g_RkDiagKey == NULL) {
        status = ZwCreateKey(&g_RkDiagKey, KEY_WRITE, &attr, 0, NULL,
                             REG_OPTION_NON_VOLATILE, &disp);
        if (!NT_SUCCESS(status)) {
            g_RkDiagKey = NULL;
            return;
        }
    }

#define RK_DIAG_PUT(_name, _field)                                            \
    do {                                                                      \
        UNICODE_STRING _vn;                                                   \
        ULONG _v = (ULONG)(g_RkDiag._field);                                  \
        RtlInitUnicodeString(&_vn, L##_name);                                 \
        ZwSetValueKey(g_RkDiagKey, &_vn, 0, REG_DWORD, &_v, sizeof(_v));      \
    } while (0)

    RK_DIAG_PUT("CardDetectCalls",   CardDetectCalls);
    RK_DIAG_PUT("CardDetectRaw",     CardDetectRaw);
    RK_DIAG_PUT("CardDetectPresent", CardDetectPresent);
    RK_DIAG_PUT("BusOpCalls",        BusOpCalls);
    RK_DIAG_PUT("BusOpLastType",     BusOpLastType);
    RK_DIAG_PUT("RequestCalls",      RequestCalls);
    RK_DIAG_PUT("LastCmdIndex",      LastCmdIndex);
    RK_DIAG_PUT("LastCmdArg",        LastCmdArg);
    RK_DIAG_PUT("LastCmdReg",        LastCmdReg);
    RK_DIAG_PUT("LastCmdStatus",     LastCmdStatus);
    RK_DIAG_PUT("InterruptCalls",    InterruptCalls);
    RK_DIAG_PUT("LastMintsts",       LastMintsts);
    RK_DIAG_PUT("SeenMintsts",       SeenMintsts);
    RK_DIAG_PUT("CmdErrors",         CmdErrors);
    RK_DIAG_PUT("BaseClockKhz",      BaseClockKhz);
    RK_DIAG_PUT("FifoOffset",        FifoOffset);

#undef RK_DIAG_PUT
}

_Use_decl_annotations_
NTSTATUS
RkdwmmcGetSlotCount(
    PSD_MINIPORT Miniport,
    PUCHAR SlotCount
    )
{
    UNREFERENCED_PARAMETER(Miniport);

    *SlotCount = 1;   // one dw_mmc slot per controller instance
    return STATUS_SUCCESS;
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

    //
    // sdport decides what to attempt from this, so it is worth having in the
    // log next to what the hardware then does. BaseClockFrequencyKhz in
    // particular is derived from a CIU rate this driver has to assume until
    // the firmware exports the real one.
    //
    g_RkDiag.BaseClockKhz = Capabilities->BaseClockFrequencyKhz;
    g_RkDiag.FifoOffset   = slot->FifoOffset;
    RkdwmmcDiagFlush();

    RkLog(RK_DBG_INFO, "Capabilities: base=%u kHz maxblk=%u slots=%u\n",
          Capabilities->BaseClockFrequencyKhz,
          Capabilities->MaximumBlockSize,
          Capabilities->MaximumOutstandingRequests);
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

    //
    // CiuClockHz and SipClockAvailable are established by DwmmcInitController,
    // which probes the SiP clock service.
    //
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

    g_RkDiag.BusOpCalls++;
    g_RkDiag.BusOpLastType = BusOperation->Type;
    RkdwmmcDiagFlush();

    RkLog(RK_DBG_INFO, "BusOperation type=%u\n", BusOperation->Type);

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
        // vmmc (card power) hangs off the board PMIC, which only EL3 can reach.
        // On the CM5 boards it is a fixed always-on 3.3 V rail, so the service
        // reports success without switching anything — but ask anyway, so a
        // board that does gate it works too.
        //
        return RkSipSdmmcRegulatorEnableSet(
                   (ULONG_PTR)slot->RegsPhysical.QuadPart,
                   RK_SIP_SDMMC_REGULATOR_ID_SUPPLY,
                   BusOperation->Parameters.Voltage == SdBusVoltage33);

    case SdSetBusWidth:
        DwmmcSetBusWidth(slot, BusOperation->Parameters.BusWidth);
        return STATUS_SUCCESS;

    case SdSetBusSpeed:
        //
        // The rate itself was already set by SdSetClock. What is left is the
        // drive/sample phase, which on RK3576 lives in the controller. The
        // angles match dw_mci_rk3288_set_ios: 180 degrees of drive phase once
        // the clock is fast enough to need it, 90 below that, and a sample
        // phase of 0 until tuning picks something better.
        //
        {
            ULONG drivePhase = (slot->CurrentClockHz >= 100000000UL) ? 180 : 90;

            (VOID)DwmmcSetPhase(slot, FALSE, drivePhase);

            if (slot->CurrentClockHz <= 400000UL) {
                (VOID)DwmmcSetPhase(slot, TRUE, 0);
            }
        }
        return STATUS_SUCCESS;

    case SdSetSignalingVoltage:
        //
        // vqmmc (I/O level) is a PMIC rail as well. Switching to 1.8 V is what
        // UHS needs; if EL3 cannot do it the caller stays at 3.3 V signalling
        // and high-speed modes, which is the correct fallback.
        //
        return RkSipSdmmcRegulatorVoltageSet(
                   (ULONG_PTR)slot->RegsPhysical.QuadPart,
                   RK_SIP_SDMMC_REGULATOR_ID_SIGNAL,
                   (BusOperation->Parameters.SignalingVoltage ==
                    SdSignalingVoltage18) ? 1800000 : 3300000);

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
    // Logged because this answer decides whether sdport tries to initialise a
    // card at all. The controller starts and no card appears, and a CDETECT
    // line that is not wired on this board would produce exactly that: report
    // "empty" forever and never be asked for anything else.
    //
    ULONG cdetect = DwmmcRead(slot->Regs, DWMMC_CDETECT);
    BOOLEAN present = ((cdetect & 1u) == 0);

    g_RkDiag.CardDetectCalls++;
    g_RkDiag.CardDetectRaw = cdetect;
    g_RkDiag.CardDetectPresent = present ? 1u : 0u;
    // No flush here: sdport polls this. The values ride out on the next
    // IssueBusOperation, which is not in a hot loop.

    RkLog(RK_DBG_INFO, "GetCardDetectState: CDETECT=0x%08x -> %s\n",
          cdetect, present ? "present" : "empty");
    return present;
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

    {
        NTSTATUS status = DwmmcSendCommand(slot, cmd, command->Argument);

        g_RkDiag.RequestCalls++;
        g_RkDiag.LastCmdIndex  = command->Index;
        g_RkDiag.LastCmdArg    = command->Argument;
        g_RkDiag.LastCmdReg    = cmd;
        g_RkDiag.LastCmdStatus = (ULONG)status;
        // No flush here either -- this is the command path.

        RkLog(RK_DBG_INFO,
              "CMD%u arg=0x%08x resp=%u xfer=%u cmdreg=0x%08x -> 0x%08x\n",
              command->Index, command->Argument, command->ResponseType,
              command->TransferType, cmd, status);
        return status;
    }
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
        // 136-bit response. dw_mmc RESP0..3 hold bits [31:0]..[127:96], so
        // copying them in order puts the least significant word first, which is
        // the direction sdport wants.
        //
        // VERIFY-ON-HARDWARE, and deliberately still open: whether a one-byte
        // shift is also needed is *not* a build question and the RK3588
        // reference does not answer it. That driver is SDHCI, where the
        // RESPONSE register already presents the response CRC-stripped and
        // shifted, and it simply byte-copies 16 bytes out of it. dw_mmc's RESP
        // registers are not the same view. Read a known CID/CSD and compare.
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
    // Raw MINTSTS before it is mapped. If a command never completes, the
    // question is whether the interrupt arrived at all and with which bits --
    // the mapping below can only be judged against what the hardware raised.
    //
    // DIRQL here: accumulate only. RkdwmmcDiagFlush is a no-op above
    // PASSIVE_LEVEL and the next passive callback publishes these.
    g_RkDiag.InterruptCalls++;
    g_RkDiag.LastMintsts = status;
    g_RkDiag.SeenMintsts |= status;

    RkLog(RK_DBG_INFO, "IRQ MINTSTS=0x%08x\n", status);

    //
    // ---- dw_mmc RINTSTS  ->  sdport event/error mapping ----
    //
    if (status & DWMMC_INT_CD) {
        *NotifyCardChange = TRUE;
        DwmmcWrite(slot->Regs, DWMMC_RINTSTS, DWMMC_INT_CD);
    }

    if (status & DWMMC_CMD_ERROR_BITS) {
        g_RkDiag.CmdErrors++;
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
        events |= SDPORT_EVENT_BUFFER_FULL;
    }
    if (status & DWMMC_INT_TXDR) {
        if (slot->DataBuffer != NULL && slot->DataWrite &&
            slot->DataTransferred < slot->DataLength) {
            ULONG remaining = slot->DataLength - slot->DataTransferred;
            slot->DataTransferred +=
                DwmmcWriteFifo(slot, slot->DataBuffer + slot->DataTransferred, remaining);
        }
        events |= SDPORT_EVENT_BUFFER_EMPTY;
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
    PRKDWMMC_SLOT slot = (PRKDWMMC_SLOT)PrivateExtension;
    NTSTATUS status;

    //
    // This used to do nothing, on the assumption that sdport completes a
    // request from the events the ISR reports. It does not: the miniport owns
    // the end of the request and has to call SdPortCompleteRequest.
    //
    // Found on the eMMC driver, which shares this file's structure: it issued
    // CMD0, the controller answered with no error, and sdport never asked for
    // anything again. This driver stops in the same shape -- three bus
    // operations and then nothing -- so the same repair belongs here, even
    // though it has not yet been seen to get as far as a command.
    //
    if (Errors != 0) {
        status = ((Errors & (SDPORT_ERROR_CMD_TIMEOUT | SDPORT_ERROR_DATA_TIMEOUT)) != 0)
                     ? STATUS_IO_TIMEOUT
                     : STATUS_CRC_ERROR;

        g_RkDiag.LastCmdStatus = (ULONG)status;
        SdPortCompleteRequest(Request, status);
        return;
    }

    if (Request->Command.TransferType == SdTransferTypeNone) {
        if ((Events & SDPORT_EVENT_CARD_RESPONSE) != 0) {
            SdPortCompleteRequest(Request, STATUS_SUCCESS);
        }
        return;
    }

    if ((Events & SDPORT_EVENT_CARD_RW_END) != 0) {
        status = (slot->DataTransferred >= slot->DataLength)
                     ? STATUS_SUCCESS
                     : STATUS_DEVICE_DATA_ERROR;
        SdPortCompleteRequest(Request, status);
    }
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
    // sdport mask as advisory. That is a deliberate simplification, not an
    // unknown: finer gating would mean mapping SDPORT_EVENT_* onto individual
    // INTMASK bits, which is only worth doing if spurious interrupts show up.
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
    PSD_MINIPORT Miniport
    )
{
    UNREFERENCED_PARAMETER(Miniport);

    //
    // The diagnostic key handle is opened once and kept; give it back here.
    //
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
    init.CrashdumpSupported = FALSE;

    return SdPortInitialize(DriverObject, RegistryPath, &init);
}
