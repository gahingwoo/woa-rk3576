/*
 * bcm4345bt.h - Broadcom BCM4345C5 Bluetooth on an RK3576 UART (H4), for
 * the Windows Bluetooth stack through the Bluetooth extensible transport
 * (BthX) DDI and the in-box BthMini.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <gpio.h>
#include <bthxddi.h>

#include "hcd.h"

#define BT_POOL_TAG             'B54B'

#define BT_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "bcm4345bt: " fmt "\n", __VA_ARGS__)

/* ------------------------------------------------------------ DW APB UART
 *
 * The RK3576 UARTs are Synopsys DesignWare APB UARTs, 16550 compatible,
 * with 32-bit registers 4 bytes apart (rk3576.dtsi: reg-shift 2,
 * reg-io-width 4). Offsets below are already multiplied by 4.
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
#define UART_USR        0x7C    /* DesignWare: bit 0 busy */
#define UART_TFL        0x80    /* DesignWare: transmit FIFO level */
#define UART_RFL        0x84    /* DesignWare: receive FIFO level */
#define UART_SRR        0x88    /* DesignWare: bit 0 reset, 1 RX FIFO, 2 TX FIFO */
#define UART_CPR        0xF4    /* DesignWare: component parameters, may read 0 */

#define IER_ERBFI       0x01    /* received data available */
#define IER_ETBEI       0x02    /* transmit holding register empty */
#define IER_ELSI        0x04    /* receiver line status */

#define IIR_ID_MASK     0x0F
#define IIR_NONE        0x01
#define IIR_MSI         0x00
#define IIR_THRE        0x02
#define IIR_RDA         0x04
#define IIR_RLS         0x06
#define IIR_BUSY        0x07    /* DesignWare: LCR written while busy */
#define IIR_TIMEOUT     0x0C

#define FCR_FIFOE       0x01
#define FCR_RFIFOR      0x02
#define FCR_XFIFOR      0x04
#define FCR_RT_HALF     0x80    /* RX trigger: FIFO half full */

#define LCR_8N1         0x03
#define LCR_DLAB        0x80

#define MCR_RTS         0x02
#define MCR_AFCE        0x20    /* DesignWare: automatic RTS/CTS */

#define LSR_DR          0x01
#define LSR_OE          0x02
#define LSR_PE          0x04
#define LSR_FE          0x08
#define LSR_BI          0x10
#define LSR_THRE        0x20
#define LSR_TEMT        0x40

#define USR_BUSY        0x01

#define SRR_RFR         0x02
#define SRR_XFR         0x04

/* Bytes written into the TX FIFO per THRE interrupt. The RK3576 UART FIFO
 * is 64 deep; half of it leaves room if the depth is smaller than thought. */
#define UART_TX_BURST   32

/* ------------------------------------------------------------- HCI / H4 */
#define H4_COMMAND      0x01
#define H4_ACL          0x02
#define H4_SCO          0x03
#define H4_EVENT        0x04

#define HCI_EV_CMD_COMPLETE     0x0E
#define HCI_EV_CMD_STATUS       0x0F

#define HCI_OP_RESET                0x0C03
#define HCI_OP_READ_LOCAL_NAME      0x0C14
#define HCI_OP_READ_LOCAL_VERSION   0x1001
#define HCI_OP_READ_BD_ADDR         0x1009
#define BCM_OP_WRITE_BD_ADDR        0xFC01
#define BCM_OP_UPDATE_BAUD          0xFC18
#define BCM_OP_DOWNLOAD_MINIDRV     0xFC2E

/* Largest frames this driver carries, H4 type byte excluded. */
#define BT_MAX_ACL_PAYLOAD      1021    /* BCM4345 ACL buffer size */
#define BT_MAX_FRAME            (4 + BT_MAX_ACL_PAYLOAD)
#define BT_MAX_EVENT            (2 + 255)

/* Received packets kept while nobody has a read pending. */
#define BT_EVENT_SLOTS          16
#define BT_ACL_SLOTS            32

#define BT_RX_RING              8192    /* power of two */
#define BT_TX_RING              8192    /* power of two */

#define BT_INIT_BAUD            115200
#define BT_DEFAULT_OPER_BAUD    1500000
#define BT_DEFAULT_UART_CLOCK   24000000

#define BT_CMD_TIMEOUT_MS       2000

#define BT_DEFAULT_FIRMWARE     L"\\SystemRoot\\System32\\drivers\\brcm\\BCM4345C5.hcd"
#define BT_FIRMWARE_MAX_BYTES   (256 * 1024)

/* Child PDO the in-box bth.inf binds BthMini on (compatible ID). */
#define BT_PDO_DEVICE_ID        L"BCM4345BT\\UART_H4"
#define BT_PDO_HARDWARE_IDS     L"BCM4345BT\\UART_H4\0"
#define BT_PDO_COMPATIBLE_ID    L"MS_BTHX_BTHMINI"

typedef struct _BT_PACKET_SLOT {
    ULONG   Length;
    UCHAR   Data[BT_MAX_FRAME];
} BT_PACKET_SLOT;

typedef struct _BT_PACKET_FIFO {
    ULONG           Head;   /* next to hand out */
    ULONG           Count;
    ULONG           Capacity;
    ULONG           Dropped;
    BT_PACKET_SLOT *Slots;
} BT_PACKET_FIFO;

typedef enum _BT_RX_STATE {
    RxType = 0,
    RxHeader,
    RxPayload,
} BT_RX_STATE;

typedef struct _BT_DEVICE_CONTEXT {
    WDFDEVICE       Device;

    /* UART */
    volatile UCHAR *Regs;
    ULONG           RegsLength;
    ULONG           UartClock;
    ULONG           OperBaud;
    WDFINTERRUPT    Interrupt;

    /* GPIO: BT_REG_ON and BT_WAKE */
    LARGE_INTEGER   RegOnConnection;
    LARGE_INTEGER   WakeConnection;
    BOOLEAN         HaveRegOn;
    BOOLEAN         HaveWake;
    WDFIOTARGET     RegOnTarget;
    WDFIOTARGET     WakeTarget;

    /* Rings shared with the ISR; guarded by the interrupt lock. */
    UCHAR           RxRing[BT_RX_RING];
    ULONG           RxHead;         /* ISR writes */
    ULONG           RxTail;         /* DPC reads */
    ULONG           RxOverflow;
    ULONG           LineErrors;
    UCHAR           TxRing[BT_TX_RING];
    ULONG           TxHead;         /* writers add */
    ULONG           TxTail;         /* ISR sends */
    UCHAR           Ier;

    /* Parser, packet FIFOs and bring-up state; guarded by PacketLock. */
    WDFSPINLOCK     PacketLock;
    UCHAR           RxStage[BT_RX_RING];
    BT_RX_STATE     RxState;
    UCHAR           RxPacketType;
    ULONG           RxNeed;
    ULONG           RxHave;
    UCHAR           RxPacket[BT_MAX_FRAME];
    BT_PACKET_FIFO  Events;
    BT_PACKET_FIFO  Acl;
    BT_PACKET_SLOT  EventSlots[BT_EVENT_SLOTS];
    BT_PACKET_SLOT  AclSlots[BT_ACL_SLOTS];

    /* Bring-up: one command in flight, answered through SyncEvent. */
    BOOLEAN         BringUp;
    USHORT          SyncOpcode;
    UCHAR           SyncReply[BT_MAX_EVENT];
    ULONG           SyncReplyLength;
    KEVENT          SyncEvent;

    /* Queues */
    WDFSPINLOCK     TxLock;         /* TxWaitQueue to TX ring, in order */
    WDFQUEUE        EventReadQueue;
    WDFQUEUE        AclReadQueue;
    WDFQUEUE        TxWaitQueue;

    /* State */
    BOOLEAN         Ready;          /* bring-up done, BthX traffic allowed */
    BOOLEAN         ChildCreated;
    BTHX_CAPABILITIES Caps;
} BT_DEVICE_CONTEXT, *PBT_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BT_DEVICE_CONTEXT, BtGetContext)

/* uart.c */
VOID     UartInit(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Baud);
VOID     UartSetBaud(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG Baud);
VOID     UartFlush(_In_ PBT_DEVICE_CONTEXT Ctx);
BOOLEAN  UartQueueFrame(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ UCHAR Type,
                        _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length);
BOOLEAN  UartWaitTxIdle(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ ULONG TimeoutMs);
EVT_WDF_INTERRUPT_ISR       BtEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC       BtEvtInterruptDpc;
EVT_WDF_INTERRUPT_ENABLE    BtEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE   BtEvtInterruptDisable;

/* hci.c */
NTSTATUS BtBringUp(_In_ PBT_DEVICE_CONTEXT Ctx);
VOID     BtInitQueuesAndFifos(_In_ PBT_DEVICE_CONTEXT Ctx);
VOID     BtParseBytes(_In_ PBT_DEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR *Bytes, _In_ ULONG Length);
VOID     BtServiceReads(_In_ PBT_DEVICE_CONTEXT Ctx);
VOID     BtServiceWrites(_In_ PBT_DEVICE_CONTEXT Ctx);
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BtEvtIoDeviceControl;

/* btgpio.c */
NTSTATUS BtGpioOpen(_In_ PBT_DEVICE_CONTEXT Ctx);
NTSTATUS BtGpioSet(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ WDFIOTARGET Target, _In_ BOOLEAN High);
VOID     BtPower(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ BOOLEAN On);

/* pdo.c */
NTSTATUS BtCreateChild(_In_ PBT_DEVICE_CONTEXT Ctx);

/* driver.c */
VOID     BtDelayMs(_In_ ULONG Ms);
VOID     BtRecordStatus(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value);
VOID     BtRecordString(_In_ PBT_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value, _In_ ULONG Length);
