/*
 * bcmwlsdio.h - KMDF function driver for the Broadcom BCM43456 Wi-Fi
 * (AMPAK AP6256, TaishanPi 3M) and BCM43752 Wi-Fi (BW3752-50B1, ArmSoM
 * CM5) on the RK3576 SDIO controller.
 *
 * W3 of docs/dev/wifi.md: open the SD bus driver's SDBUS interface, talk
 * CMD52/CMD53 to the card, identify the chip, download the firmware, NVRAM
 * and CLM blob, start the firmware and read its version. No network
 * interface yet. Everything it learns goes to the device's registry key
 * (Device Parameters), flushed, so a board session can read it afterwards.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <ntddsd.h>

#include "bwl.h"

#define BWL_POOL_TAG    'lWcB'

#define BWL_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "bcmwlsdio: " fmt "\n", __VA_ARGS__)

/*
 * Stage (DWORD under Device Parameters, set by the INF to 0):
 *   0  open the SDBUS interface and record what the bus driver says; no
 *      access to the card
 *   1  also identify: CCCR, chip ID, cores, RAM size (bwfm's attach)
 *   2  also download the firmware and start it, then read its version
 */
#define BWL_STAGE_OPEN      0
#define BWL_STAGE_IDENTIFY  1
#define BWL_STAGE_FIRMWARE  2

#define BWL_DEFAULT_FW_DIR  L"\\SystemRoot\\System32\\drivers\\brcm"
#define BWL_FW_MAX_BYTES    (1024u * 1024u)
#define BWL_NVRAM_MAX_BYTES (16u * 1024u)
#define BWL_CLM_MAX_BYTES   (64u * 1024u)

typedef struct _BWL_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFWORKITEM BringUp;
    WDFMEMORY BounceMemory;
    PUCHAR Bounce;

    SDBUS_INTERFACE_STANDARD Sd;
    BOOLEAN SdOpen;
    UCHAR Function;             /* SDP_FUNCTION_NUMBER of this PDO */
    volatile LONG Stopping;

    ULONG Stage;
    ULONG MaxBlocks;
    ULONG F2BlockMode;
    ULONG Verify;

    /* Counters (CMD52/CMD53 through sdbus) */
    ULONG Cmd52;
    ULONG Cmd53;
    ULONG Cmd52Errors;
    ULONG Cmd53Errors;
    NTSTATUS LastSdStatus;
    ULONG LastSdArg;
    ULONG LastR5Flags;

    LONG StepSeq;

    /*
     * Boot timing, interrupt time (100 ns) at each PnP step, and what the
     * early SDBUS open in AddDevice returned (see BwlEvtDeviceAdd).
     */
    LONG64 AddDeviceTime;
    LONG64 PrepareHardwareTime;
    NTSTATUS EarlyOpenStatus;

    bwl_dev Bwl;
} BWL_DEVICE_CONTEXT, *PBWL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BWL_DEVICE_CONTEXT, BwlGetContext)

/* sdbus.c */
extern const bwl_bus_ops BwlSdOps;
NTSTATUS BwlSdOpen(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlSdClose(_In_ PBWL_DEVICE_CONTEXT Ctx);
NTSTATUS BwlSdGetProperty(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SDBUS_PROPERTY Property,
                          _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);
NTSTATUS BwlSdSetProperty(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ SDBUS_PROPERTY Property,
                          _In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length);

/* files.c */
NTSTATUS BwlLoadFile(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG MaxBytes,
                     _Out_ WDFMEMORY *Memory, _Out_ PUCHAR *Data, _Out_ PULONG Length);

/* driver.c: breadcrumbs, PASSIVE_LEVEL */
VOID BwlCrumb(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCSTR Step);
VOID BwlPutULong(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value);
VOID BwlPutString(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCSTR Value);
VOID BwlPutWString(_In_ PBWL_DEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ PCWSTR Value);
