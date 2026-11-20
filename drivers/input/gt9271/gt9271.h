/*
 * gt9271.h - Goodix GT9271 touch controller as a HID touch screen (VHF)
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <vhf.h>

#define GT_POOL_TAG             '9TGG'

/*
 * Goodix GT9xx register map, as far as this driver uses it. Addresses are
 * 16 bits, sent big-endian ahead of the data.
 *
 * The configuration area starts at 0x8047. This driver reads it for the log
 * and never writes it: a bad write there replaces the factory configuration.
 */
#define GT_REG_CONFIG_XMAX      0x8048  /* X output max, low byte first; 4 bytes with Y */
#define GT_REG_PRODUCT_ID       0x8140  /* ASCII, "9271" */
#define GT_REG_FW_VERSION       0x8144  /* 16 bits, low byte first */
#define GT_REG_STATUS           0x814E  /* bit 7 buffer ready, bits 3:0 points */
#define GT_REG_POINT0           0x814F  /* 8 bytes per point */

#define GT_STATUS_READY         0x80
#define GT_STATUS_POINTS_MASK   0x0F
#define GT_POINT_SIZE           8
#define GT_MAX_POINTS           10

/* The panel, native portrait, as Windows shows it today. */
#define GT_LOGICAL_MAX_X        799
#define GT_LOGICAL_MAX_Y        1279

/*
 * Active area in units of 0.1 mm (HID unit centimetre, exponent -2). A 10.1
 * inch 800x1280 panel with a 0.1695 mm pixel pitch: 135.6 x 216.96 mm.
 */
#define GT_PHYSICAL_MAX_X       1356
#define GT_PHYSICAL_MAX_Y       2170

#define GT_REPORT_ID_TOUCH      1
#define GT_REPORT_ID_MAX_COUNT  2

/* While a finger is down, look at the controller this often even without an
 * interrupt, so that a missed edge cannot leave a contact stuck. */
#define GT_GUARD_POLL_MS        100
/* With no interrupt in _CRS at all. */
#define GT_POLL_MS              10

#define GT_I2C_TIMEOUT_MS       100

#include <pshpack1.h>
typedef struct _GT_HID_CONTACT {
    UCHAR   Flags;          /* bit 0 tip switch */
    UCHAR   ContactId;
    USHORT  X;
    USHORT  Y;
} GT_HID_CONTACT;

typedef struct _GT_HID_INPUT_REPORT {
    UCHAR           ReportId;
    GT_HID_CONTACT  Contact[GT_MAX_POINTS];
    USHORT          ScanTime;
    UCHAR           ContactCount;
} GT_HID_INPUT_REPORT;

typedef struct _GT_HID_FEATURE_MAX_COUNT {
    UCHAR   ReportId;
    UCHAR   ContactCountMaximum;
} GT_HID_FEATURE_MAX_COUNT;
#include <poppack.h>

C_ASSERT(sizeof(GT_HID_CONTACT) == 6);
C_ASSERT(sizeof(GT_HID_INPUT_REPORT) == 64);

typedef struct _GT_POINT {
    UCHAR   Id;
    USHORT  X;
    USHORT  Y;
} GT_POINT;

typedef struct _GT_DEVICE_CONTEXT {
    WDFDEVICE       Device;
    WDFIOTARGET     SpbTarget;
    LARGE_INTEGER   SpbConnectionId;
    BOOLEAN         HaveSpb;
    WDFINTERRUPT    Interrupt;
    WDFTIMER        Timer;
    WDFWAITLOCK     Lock;
    VHFHANDLE       Vhf;
    BOOLEAN         VhfStarted;
    BOOLEAN         Polled;
    BOOLEAN         Running;

    /* Contacts down after the last frame sent. */
    GT_POINT        Down[GT_MAX_POINTS];
    ULONG           DownCount;

    ULONG           ReadErrors;
} GT_DEVICE_CONTEXT, *PGT_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GT_DEVICE_CONTEXT, GtGetContext)

#define GT_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "gt9271: " fmt "\n", __VA_ARGS__)
