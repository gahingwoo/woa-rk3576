/*
 * rkled.h - the board's LEDs: heartbeat, disk activity, on or off
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

#define RKLED_POOL_TAG      'DELR'
#define RKLED_MAX_LEDS      4
#define RKLED_MAX_DISKS     4
#define RKLED_TICK_MS       50

/* What an LED shows (rockchip,led-trigger, registry TriggerN). */
#define RKLED_TRIGGER_OFF        0
#define RKLED_TRIGGER_ON         1
#define RKLED_TRIGGER_HEARTBEAT  2
#define RKLED_TRIGGER_DISK       3
#define RKLED_TRIGGER_MAX        3

#define RKLED_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "rkled: " fmt "\n", __VA_ARGS__)

typedef struct _RKLED_LED {
    LARGE_INTEGER   Connection;
    WDFIOTARGET     Target;
    ULONG           ActiveLow;
    ULONG           Trigger;
    LONG            Lit;            /* -1 unknown, 0 off, 1 on */
} RKLED_LED, *PRKLED_LED;

typedef struct _RKLED_CONTEXT {
    WDFDEVICE       Device;
    WDFTIMER        Timer;
    ULONG           Count;
    RKLED_LED       Led[RKLED_MAX_LEDS];
    ULONG           Tick;
    BOOLEAN         Running;
    BOOLEAN         WantDisk;
    /* Disk activity: the read and write counts of PhysicalDrive0..3. */
    WDFIOTARGET     Disk[RKLED_MAX_DISKS];
    ULONGLONG       DiskOps[RKLED_MAX_DISKS];
    ULONG           DiskRetryTick;
    BOOLEAN         DiskActive;
} RKLED_CONTEXT, *PRKLED_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RKLED_CONTEXT, RkLedGetContext)

_IRQL_requires_(PASSIVE_LEVEL)
VOID
RkLedReadConfig(
    _In_ PRKLED_CONTEXT Ctx
    );
