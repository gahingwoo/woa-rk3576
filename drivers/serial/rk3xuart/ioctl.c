/*
 * ioctl.c - the serial IOCTLs SerCx2 hands to the controller driver, and
 * the open, close, purge, wait-mask and default-configuration callbacks
 *
 * SerCx2 itself handles reads, writes, timeouts, wait-on-mask, purge and the
 * queue IOCTLs, and passes the line IOCTLs here (EvtSerCx2Control); each one
 * is completed here, with the status also returned. The list is the one
 * SerCx2's documentation marks as passed to the controller ("Serial I/O
 * Request Interface", Microsoft Learn), and the answers are the ones
 * Windows.Devices.SerialCommunication and Win32 SetCommState need.
 *
 * The 40-pin header carries TX and RX only. RTS, CTS, DTR, DSR, DCD and RI
 * are not wired, so: DTR and RTS can be set and read back (MCR) and change
 * nothing outside; the modem status reads CTS and DSR asserted, so a program
 * that waits for them does not hang; hardware and software flow control are
 * refused.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xuart.h"

#define RKU_SETTABLE_BAUD \
    (SERIAL_BAUD_075 | SERIAL_BAUD_110 | SERIAL_BAUD_150 | SERIAL_BAUD_300 | \
     SERIAL_BAUD_600 | SERIAL_BAUD_1200 | SERIAL_BAUD_1800 | SERIAL_BAUD_2400 | \
     SERIAL_BAUD_4800 | SERIAL_BAUD_7200 | SERIAL_BAUD_9600 | SERIAL_BAUD_14400 | \
     SERIAL_BAUD_19200 | SERIAL_BAUD_38400 | SERIAL_BAUD_56K | SERIAL_BAUD_57600 | \
     SERIAL_BAUD_115200 | SERIAL_BAUD_USER)

/* ControlHandShake and FlowReplace bits that need wires or features we lack. */
#define RKU_HANDSHAKE_UNSUPPORTED \
    (SERIAL_CTS_HANDSHAKE | SERIAL_DSR_HANDSHAKE | SERIAL_DCD_HANDSHAKE | \
     SERIAL_DSR_SENSITIVITY | SERIAL_DTR_HANDSHAKE)
#define RKU_FLOWREPLACE_UNSUPPORTED \
    (SERIAL_AUTO_TRANSMIT | SERIAL_AUTO_RECEIVE | SERIAL_RTS_HANDSHAKE | \
     SERIAL_ERROR_CHAR | SERIAL_NULL_STRIPPING | SERIAL_BREAK_CHAR)

static NTSTATUS
InBuf(_In_ WDFREQUEST Request, _In_ size_t Len, _Out_ PVOID *Buf)
{
    return WdfRequestRetrieveInputBuffer(Request, Len, Buf, NULL);
}

static NTSTATUS
OutBuf(_In_ WDFREQUEST Request, _In_ size_t Len, _Out_ PVOID *Buf)
{
    return WdfRequestRetrieveOutputBuffer(Request, Len, Buf, NULL);
}

static NTSTATUS
RkuSetHandflow(_In_ PRKU_DEVICE Dev, _In_ const SERIAL_HANDFLOW *Hf)
{
    UCHAR set = 0;
    UCHAR clear = 0;

    if ((Hf->ControlHandShake & RKU_HANDSHAKE_UNSUPPORTED) != 0 ||
        (Hf->FlowReplace & RKU_FLOWREPLACE_UNSUPPORTED) != 0) {
        RKU_ERR("UART%lu: flow control 0x%lx/0x%lx refused: TX and RX only on the header",
                Dev->UartIndex, Hf->ControlHandShake, Hf->FlowReplace);
        return STATUS_NOT_SUPPORTED;
    }

    /* SERIAL_DTR_CONTROL / SERIAL_RTS_CONTROL: the line is on while open */
    if ((Hf->ControlHandShake & SERIAL_DTR_MASK) == SERIAL_DTR_CONTROL) {
        set |= MCR_DTR;
    } else {
        clear |= MCR_DTR;
    }
    if ((Hf->FlowReplace & SERIAL_RTS_MASK) == SERIAL_RTS_CONTROL) {
        set |= MCR_RTS;
    } else {
        clear |= MCR_RTS;
    }
    RkuSetMcr(Dev, set, clear);
    Dev->Handflow = *Hf;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkuEvtControl(
    WDFDEVICE Device,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    PVOID buf = NULL;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_SERIAL_SET_BAUD_RATE:
        status = InBuf(Request, sizeof(SERIAL_BAUD_RATE), &buf);
        if (NT_SUCCESS(status)) {
            status = RkuSetBaud(dev, ((PSERIAL_BAUD_RATE)buf)->BaudRate);
        }
        break;

    case IOCTL_SERIAL_GET_BAUD_RATE:
        status = OutBuf(Request, sizeof(SERIAL_BAUD_RATE), &buf);
        if (NT_SUCCESS(status)) {
            ((PSERIAL_BAUD_RATE)buf)->BaudRate = dev->Baud;
            info = sizeof(SERIAL_BAUD_RATE);
        }
        break;

    case IOCTL_SERIAL_SET_LINE_CONTROL:
        status = InBuf(Request, sizeof(SERIAL_LINE_CONTROL), &buf);
        if (NT_SUCCESS(status)) {
            status = RkuSetLineControl(dev, (PSERIAL_LINE_CONTROL)buf);
        }
        break;

    case IOCTL_SERIAL_GET_LINE_CONTROL:
        status = OutBuf(Request, sizeof(SERIAL_LINE_CONTROL), &buf);
        if (NT_SUCCESS(status)) {
            *(PSERIAL_LINE_CONTROL)buf = dev->LineControl;
            info = sizeof(SERIAL_LINE_CONTROL);
        }
        break;

    case IOCTL_SERIAL_SET_HANDFLOW:
        status = InBuf(Request, sizeof(SERIAL_HANDFLOW), &buf);
        if (NT_SUCCESS(status)) {
            status = RkuSetHandflow(dev, (PSERIAL_HANDFLOW)buf);
        }
        break;

    case IOCTL_SERIAL_GET_HANDFLOW:
        status = OutBuf(Request, sizeof(SERIAL_HANDFLOW), &buf);
        if (NT_SUCCESS(status)) {
            *(PSERIAL_HANDFLOW)buf = dev->Handflow;
            info = sizeof(SERIAL_HANDFLOW);
        }
        break;

    case IOCTL_SERIAL_GET_PROPERTIES:
        status = OutBuf(Request, sizeof(SERIAL_COMMPROP), &buf);
        if (NT_SUCCESS(status)) {
            PSERIAL_COMMPROP p = (PSERIAL_COMMPROP)buf;

            RtlZeroMemory(p, sizeof(*p));
            p->PacketLength = sizeof(SERIAL_COMMPROP);
            p->PacketVersion = 2;
            p->ServiceMask = SERIAL_SP_SERIALCOMM;
            p->MaxBaud = SERIAL_BAUD_USER;
            p->ProvSubType = SERIAL_SP_RS232;
            p->ProvCapabilities = SERIAL_PCF_PARITY_CHECK | SERIAL_PCF_TOTALTIMEOUTS |
                                  SERIAL_PCF_INTTIMEOUTS;
            p->SettableParams = SERIAL_SP_PARITY | SERIAL_SP_BAUD | SERIAL_SP_DATABITS |
                                SERIAL_SP_STOPBITS | SERIAL_SP_PARITY_CHECK;
            p->SettableBaud = RKU_SETTABLE_BAUD;
            p->SettableData = SERIAL_DATABITS_5 | SERIAL_DATABITS_6 | SERIAL_DATABITS_7 |
                              SERIAL_DATABITS_8;
            p->SettableStopParity = SERIAL_STOPBITS_10 | SERIAL_STOPBITS_15 | SERIAL_STOPBITS_20 |
                                    SERIAL_PARITY_NONE | SERIAL_PARITY_ODD | SERIAL_PARITY_EVEN |
                                    SERIAL_PARITY_MARK | SERIAL_PARITY_SPACE;
            p->CurrentRxQueue = RKU_RX_RING_SIZE;
            p->CurrentTxQueue = dev->FifoDepth;
            info = sizeof(SERIAL_COMMPROP);
        }
        break;

    case IOCTL_SERIAL_GET_COMMSTATUS:
        status = OutBuf(Request, sizeof(SERIAL_STATUS), &buf);
        if (NT_SUCCESS(status)) {
            PSERIAL_STATUS s = (PSERIAL_STATUS)buf;

            RtlZeroMemory(s, sizeof(*s));
            WdfInterruptAcquireLock(dev->Interrupt);
            s->Errors = dev->CommErrors;
            dev->CommErrors = 0;
            s->AmountInInQueue = (dev->RxHead - dev->RxTail) & (RKU_RX_RING_SIZE - 1);
            s->AmountInOutQueue = RkuRead(dev, UART_TFL);
            WdfInterruptReleaseLock(dev->Interrupt);
            info = sizeof(SERIAL_STATUS);
        }
        break;

    case IOCTL_SERIAL_GET_MODEMSTATUS:
        status = OutBuf(Request, sizeof(ULONG), &buf);
        if (NT_SUCCESS(status)) {
            /* not wired: report CTS and DSR asserted, DCD and RI not */
            *(PULONG)buf = MSR_CTS | MSR_DSR;
            info = sizeof(ULONG);
        }
        break;

    case IOCTL_SERIAL_SET_RTS:
        RkuSetMcr(dev, MCR_RTS, 0);
        break;
    case IOCTL_SERIAL_CLR_RTS:
        RkuSetMcr(dev, 0, MCR_RTS);
        break;
    case IOCTL_SERIAL_SET_DTR:
        RkuSetMcr(dev, MCR_DTR, 0);
        break;
    case IOCTL_SERIAL_CLR_DTR:
        RkuSetMcr(dev, 0, MCR_DTR);
        break;

    case IOCTL_SERIAL_GET_DTRRTS:
        status = OutBuf(Request, sizeof(ULONG), &buf);
        if (NT_SUCCESS(status)) {
            ULONG v = 0;

            if (dev->Mcr & MCR_DTR) {
                v |= SERIAL_DTR_STATE;
            }
            if (dev->Mcr & MCR_RTS) {
                v |= SERIAL_RTS_STATE;
            }
            *(PULONG)buf = v;
            info = sizeof(ULONG);
        }
        break;

    case IOCTL_SERIAL_GET_MODEM_CONTROL:
        status = OutBuf(Request, sizeof(ULONG), &buf);
        if (NT_SUCCESS(status)) {
            *(PULONG)buf = dev->Mcr;
            info = sizeof(ULONG);
        }
        break;

    case IOCTL_SERIAL_SET_MODEM_CONTROL:
        status = InBuf(Request, sizeof(ULONG), &buf);
        if (NT_SUCCESS(status)) {
            /* DTR, RTS and loopback only: OUT1/OUT2/AFCE are not for users */
            UCHAR v = (UCHAR)(*(PULONG)buf & (MCR_DTR | MCR_RTS | MCR_LOOP));
            RkuSetMcr(dev, v, (UCHAR)(~v & (MCR_DTR | MCR_RTS | MCR_LOOP)));
        }
        break;

    case IOCTL_SERIAL_SET_BREAK_ON:
        RkuSetBreak(dev, TRUE);
        break;
    case IOCTL_SERIAL_SET_BREAK_OFF:
        RkuSetBreak(dev, FALSE);
        break;

    case IOCTL_SERIAL_SET_FIFO_CONTROL:
        /* the FIFOs stay on with the trigger levels this driver chose */
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? info : 0);
    return status;
}

/*
 * The default configuration comes from a UARTSerialBusV2 connection, which
 * only a peripheral device described under this UART would have. With the
 * COM-port interface (SerCx-FriendlyName in _DSD) SerCx2 has none and never
 * calls this; it is here so the UART can also serve a described peripheral.
 */
#include <pshpack1.h>
typedef struct _RKU_PNP_UART_DESCRIPTOR {
    PNP_SERIAL_BUS_DESCRIPTOR Common;
    ULONG  BaudRate;
    USHORT RxBufferSize;
    USHORT TxBufferSize;
    UCHAR  Parity;
    UCHAR  SerialLinesEnabled;
} RKU_PNP_UART_DESCRIPTOR;
#include <poppack.h>

#define RKU_UART_SERIAL_BUS_TYPE    3
#define RKU_UART_FLOW_MASK          0x0003
#define RKU_UART_STOP_SHIFT         2
#define RKU_UART_DATA_SHIFT         4

_Use_decl_annotations_
NTSTATUS
RkuEvtApplyConfig(WDFDEVICE Device, PVOID ConnectionParameters)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER rh = ConnectionParameters;
    const RKU_PNP_UART_DESCRIPTOR *d;
    SERIAL_LINE_CONTROL lc;
    USHORT flags;
    NTSTATUS status;

    if (rh == NULL || rh->PropertiesLength < sizeof(RKU_PNP_UART_DESCRIPTOR)) {
        return STATUS_INVALID_PARAMETER;
    }
    d = (const RKU_PNP_UART_DESCRIPTOR *)rh->ConnectionProperties;
    if (d->Common.SerialBusType != RKU_UART_SERIAL_BUS_TYPE) {
        return STATUS_INVALID_PARAMETER;
    }

    flags = d->Common.TypeSpecificFlags;
    if ((flags & RKU_UART_FLOW_MASK) != 0) {
        return STATUS_NOT_SUPPORTED;
    }

    switch ((flags >> RKU_UART_DATA_SHIFT) & 0x7) {
    case 0: lc.WordLength = 5; break;
    case 1: lc.WordLength = 6; break;
    case 2: lc.WordLength = 7; break;
    case 3: lc.WordLength = 8; break;
    default: return STATUS_NOT_SUPPORTED;
    }
    switch ((flags >> RKU_UART_STOP_SHIFT) & 0x3) {
    case 1: lc.StopBits = STOP_BIT_1; break;
    case 2: lc.StopBits = STOP_BITS_1_5; break;
    case 3: lc.StopBits = STOP_BITS_2; break;
    default: return STATUS_NOT_SUPPORTED;
    }
    switch (d->Parity) {
    case 0: lc.Parity = NO_PARITY; break;
    case 1: lc.Parity = EVEN_PARITY; break;
    case 2: lc.Parity = ODD_PARITY; break;
    case 3: lc.Parity = MARK_PARITY; break;
    case 4: lc.Parity = SPACE_PARITY; break;
    default: return STATUS_NOT_SUPPORTED;
    }

    status = RkuSetBaud(dev, d->BaudRate);
    if (NT_SUCCESS(status)) {
        status = RkuSetLineControl(dev, &lc);
    }
    return status;
}

_Use_decl_annotations_
VOID
RkuEvtPurgeFifos(WDFDEVICE Device, BOOLEAN PurgeRxFifo, BOOLEAN PurgeTxFifo)
{
    RkuPurgeFifos(RkuGetDevice(Device), PurgeRxFifo, PurgeTxFifo);
}

_Use_decl_annotations_
VOID
RkuEvtSetWaitMask(WDFDEVICE Device, WDFREQUEST Request, ULONG WaitMask)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);

    if ((WaitMask & ~(RKU_WAIT_SUPPORTED | RKU_WAIT_IGNORED)) != 0) {
        /* RXFLAG (no special characters in SerCx2), PERR, RX80FULL, EVENT1/2 */
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    WdfInterruptAcquireLock(dev->Interrupt);
    dev->WaitMask = WaitMask & RKU_WAIT_SUPPORTED;
    dev->WaitEvents = 0;
    WdfInterruptReleaseLock(dev->Interrupt);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS
RkuEvtFileOpen(WDFDEVICE Device)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);

    RkuPurgeFifos(dev, TRUE, TRUE);
    WdfInterruptAcquireLock(dev->Interrupt);
    dev->CommErrors = 0;
    dev->WaitMask = 0;
    dev->WaitEvents = 0;
    dev->Ier = IER_ERBFI | IER_ELSI;
    RkuWrite(dev, UART_IER, dev->Ier);
    WdfInterruptReleaseLock(dev->Interrupt);
    RKU_LOG("UART%lu opened at %lu baud", dev->UartIndex, dev->Baud);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
RkuEvtFileClose(WDFDEVICE Device)
{
    PRKU_DEVICE dev = RkuGetDevice(Device);

    WdfInterruptAcquireLock(dev->Interrupt);
    dev->Ier = 0;
    RkuWrite(dev, UART_IER, 0);
    dev->WaitMask = 0;
    dev->WaitEvents = 0;
    WdfInterruptReleaseLock(dev->Interrupt);
    if (dev->BreakOn) {
        RkuSetBreak(dev, FALSE);
    }
    RKU_LOG("UART%lu closed (busy %lu, overruns %lu)", dev->UartIndex, dev->BusyIrqs, dev->Overruns);
}
