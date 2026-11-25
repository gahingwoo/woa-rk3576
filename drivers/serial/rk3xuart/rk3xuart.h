/*
 * rk3xuart.h - SerCx2 controller driver for the RK3576 DesignWare APB UARTs
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <ntddser.h>
#include <acpiioct.h>
#include <reshub.h>
#include <sercx.h>

#define RKU_POOL_TAG    'tUkR'

#define RKU_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "rk3xuart: " fmt "\n", __VA_ARGS__)
#define RKU_ERR(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "rk3xuart: " fmt "\n", __VA_ARGS__)

/* ------------------------------------------------------------ DW APB UART
 *
 * Synopsys DesignWare APB UART, 16550 compatible, 32-bit registers four
 * bytes apart (rk3576.dtsi: reg-shift = 2, reg-io-width = 4). The offsets
 * are byte offsets, already multiplied by four.
 */
#define UART_RBR        0x00    /* read, DLAB = 0 */
#define UART_THR        0x00    /* write, DLAB = 0 */
#define UART_DLL        0x00    /* DLAB = 1 */
#define UART_IER        0x04    /* DLAB = 0 */
#define UART_DLH        0x04    /* DLAB = 1 */
#define UART_IIR        0x08    /* read */
#define UART_FCR        0x08    /* write */
#define UART_LCR        0x0C
#define UART_MCR        0x10
#define UART_LSR        0x14
#define UART_MSR        0x18
#define UART_USR        0x7C    /* DesignWare: 0 busy, 1 TX FIFO not full, 3 RX FIFO not empty */
#define UART_TFL        0x80    /* DesignWare: TX FIFO level */
#define UART_RFL        0x84    /* DesignWare: RX FIFO level */
#define UART_SRR        0x88    /* DesignWare: 0 UART reset, 1 RX FIFO reset, 2 TX FIFO reset */
#define UART_CPR        0xF4    /* DesignWare: component parameters (may read 0) */
#define UART_UCV        0xF8    /* DesignWare: component version */

#define IER_ERBFI       0x01    /* received data available / character timeout */
#define IER_ETBEI       0x02    /* transmit holding register empty */
#define IER_ELSI        0x04    /* receiver line status */
#define IER_EDSSI       0x08    /* modem status */

#define IIR_ID_MASK     0x0F
#define IIR_MSI         0x00
#define IIR_NONE        0x01
#define IIR_THRE        0x02
#define IIR_RDA         0x04
#define IIR_RLS         0x06
#define IIR_BUSY        0x07    /* DesignWare: LCR written while busy */
#define IIR_TIMEOUT     0x0C

#define FCR_FIFOE       0x01
#define FCR_RFIFOR      0x02
#define FCR_XFIFOR      0x04
#define FCR_TET_EMPTY   0x00    /* TX empty trigger: FIFO empty */
#define FCR_RT_QUARTER  0x40    /* RX trigger: FIFO 1/4 full */

#define LCR_WLS_MASK    0x03
#define LCR_STB         0x04
#define LCR_PEN         0x08
#define LCR_EPS         0x10
#define LCR_SP          0x20    /* stick parity */
#define LCR_BC          0x40    /* break control */
#define LCR_DLAB        0x80

#define MCR_DTR         0x01
#define MCR_RTS         0x02
#define MCR_LOOP        0x10

#define LSR_DR          0x01
#define LSR_OE          0x02
#define LSR_PE          0x04
#define LSR_FE          0x08
#define LSR_BI          0x10
#define LSR_THRE        0x20
#define LSR_TEMT        0x40
#define LSR_RFE         0x80

#define MSR_CTS         0x10
#define MSR_DSR         0x20
#define MSR_RI          0x40
#define MSR_DCD         0x80

#define USR_BUSY        0x01
#define USR_TFNF        0x02
#define USR_RFNE        0x08

#define SRR_UR          0x01
#define SRR_RFR         0x02
#define SRR_XFR         0x04

/* Defaults when the firmware does not say otherwise. */
#define RKU_DEFAULT_CLOCK_HZ    24000000UL  /* sclk_uartN from xin24m */
#define RKU_DEFAULT_FIFO        64          /* RK3576 UART FIFO depth */
#define RKU_DEFAULT_BAUD        115200
#define RKU_RX_RING_SIZE        4096        /* power of two */

/* Wait events this driver can raise. */
#define RKU_WAIT_SUPPORTED  (SERIAL_EV_RXCHAR | SERIAL_EV_TXEMPTY | SERIAL_EV_BREAK | SERIAL_EV_ERR)
/*
 * Accepted in a wait mask and never raised: the modem lines are not wired
 * to the 40-pin header. Windows.Devices.SerialCommunication sets them by
 * default, so refusing them would break every UWP serial app.
 */
#define RKU_WAIT_IGNORED    (SERIAL_EV_CTS | SERIAL_EV_DSR | SERIAL_EV_RLSD | SERIAL_EV_RING)

typedef struct _RKU_DEVICE RKU_DEVICE, *PRKU_DEVICE;

typedef struct _RKU_PIO_CONTEXT {
    PRKU_DEVICE Dev;
} RKU_PIO_CONTEXT, *PRKU_PIO_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RKU_PIO_CONTEXT, RkuGetPioContext)

struct _RKU_DEVICE {
    WDFDEVICE           Device;
    WDFINTERRUPT        Interrupt;
    WDFTIMER            DrainTimer;
    SERCX2PIORECEIVE    PioReceive;
    SERCX2PIOTRANSMIT   PioTransmit;

    volatile UCHAR      *Regs;
    SIZE_T              RegsLength;
    PHYSICAL_ADDRESS    RegsPhys;

    /* Configuration, from _DSD and the device's hardware key. */
    ULONG               ClockHz;
    ULONG               FifoDepth;
    ULONG               UartIndex;      /* N of UARTN, from the register address */
    WCHAR               PortName[16];   /* e.g. L"COM4"; empty for none */
    WCHAR               DeviceName[32]; /* \Device\RkUartN */
    BOOLEAN             PortLinkCreated;
    BOOLEAN             SerialCommWritten;

    /* Line state (passive level, serialised by SerCx2). */
    ULONG               Baud;
    SERIAL_LINE_CONTROL LineControl;
    SERIAL_HANDFLOW     Handflow;
    UCHAR               Lcr;            /* without DLAB */
    UCHAR               Mcr;
    BOOLEAN             BreakOn;

    /*
     * Everything below is shared with the ISR and is touched only with the
     * interrupt lock held (WdfInterruptAcquireLock or inside the ISR).
     */
    UCHAR               Ier;
    UCHAR               RxRing[RKU_RX_RING_SIZE];
    ULONG               RxHead;         /* ISR writes */
    ULONG               RxTail;         /* ReadBuffer reads */
    BOOLEAN             RxArmed;        /* SerCx2 waits for SerCx2PioReceiveReady */
    BOOLEAN             TxArmed;        /* SerCx2 waits for SerCx2PioTransmitReady */
    BOOLEAN             RxReadyPending; /* DPC must call SerCx2PioReceiveReady */
    BOOLEAN             TxReadyPending; /* DPC must call SerCx2PioTransmitReady */
    BOOLEAN             DrainArmed;
    ULONG               WaitMask;
    ULONG               WaitEvents;     /* raised, not yet reported */
    ULONG               CommErrors;     /* SERIAL_ERROR_*, cleared by GET_COMMSTATUS */
    ULONG               BusyIrqs;
    ULONG               Overruns;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RKU_DEVICE, RkuGetDevice)

/* uart.c */
ULONG RkuRead(_In_ PRKU_DEVICE Dev, _In_ ULONG Off);
VOID RkuWrite(_In_ PRKU_DEVICE Dev, _In_ ULONG Off, _In_ ULONG Val);
VOID RkuHwInit(_In_ PRKU_DEVICE Dev);
VOID RkuHwQuiesce(_In_ PRKU_DEVICE Dev);
NTSTATUS RkuSetBaud(_In_ PRKU_DEVICE Dev, _In_ ULONG Baud);
NTSTATUS RkuSetLineControl(_In_ PRKU_DEVICE Dev, _In_ const SERIAL_LINE_CONTROL *Lc);
VOID RkuSetBreak(_In_ PRKU_DEVICE Dev, _In_ BOOLEAN On);
VOID RkuSetMcr(_In_ PRKU_DEVICE Dev, _In_ UCHAR Set, _In_ UCHAR Clear);
VOID RkuPurgeFifos(_In_ PRKU_DEVICE Dev, _In_ BOOLEAN Rx, _In_ BOOLEAN Tx);

EVT_WDF_INTERRUPT_ISR RkuEvtIsr;
EVT_WDF_INTERRUPT_DPC RkuEvtDpc;
EVT_WDF_TIMER RkuEvtDrainTimer;

EVT_SERCX2_PIO_RECEIVE_READ_BUFFER RkuEvtRxReadBuffer;
EVT_SERCX2_PIO_RECEIVE_ENABLE_READY_NOTIFICATION RkuEvtRxEnableReady;
EVT_SERCX2_PIO_RECEIVE_CANCEL_READY_NOTIFICATION RkuEvtRxCancelReady;
EVT_SERCX2_PIO_TRANSMIT_WRITE_BUFFER RkuEvtTxWriteBuffer;
EVT_SERCX2_PIO_TRANSMIT_ENABLE_READY_NOTIFICATION RkuEvtTxEnableReady;
EVT_SERCX2_PIO_TRANSMIT_CANCEL_READY_NOTIFICATION RkuEvtTxCancelReady;
EVT_SERCX2_PIO_TRANSMIT_DRAIN_FIFO RkuEvtTxDrainFifo;
EVT_SERCX2_PIO_TRANSMIT_CANCEL_DRAIN_FIFO RkuEvtTxCancelDrainFifo;
EVT_SERCX2_PIO_TRANSMIT_PURGE_FIFO RkuEvtTxPurgeFifo;

/* ioctl.c */
EVT_SERCX2_CONTROL RkuEvtControl;
EVT_SERCX2_APPLY_CONFIG RkuEvtApplyConfig;
EVT_SERCX2_PURGE_FIFOS RkuEvtPurgeFifos;
EVT_SERCX2_SET_WAIT_MASK RkuEvtSetWaitMask;
EVT_SERCX2_FILEOPEN RkuEvtFileOpen;
EVT_SERCX2_FILECLOSE RkuEvtFileClose;
