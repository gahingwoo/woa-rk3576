/*
 * wdicore.h - the WDI message handling shared by both Wi-Fi front ends of
 * aic8800usb: WiFiCx 1.0 on Windows 11 (wificx.cpp, aic8800usb.sys) and the
 * NDIS WDI miniport on Windows 10 (wdi.cpp, aic8800wdi.sys). WiFiCx carries
 * the same WDI messages (WDI_MESSAGE_HEADER and TLVs) as WDI: scan,
 * connect, keys, radio, reset and abort are one code (wdicore.cpp), built
 * against the TLV library of each (WificxTLVGenParse.lib, km\wlan\2.0;
 * TLVGeneratorParser.lib, km\wlan\1.0). The front ends move the messages
 * to and from the OS (AicFe*) and report the capabilities. bcmwlsdio's
 * wdicore.h is the same split.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "aic8800usb.h"
#ifdef AIC_WDI
#include <dot11wdi.h>
#include <wditypes.hpp>
#define AIC_FE_NAME "wdi"
#else
#include <wificx.h>
#define AIC_FE_NAME "wificx"
#endif
#include <TlvGeneratorParser.hpp>

/* A request from the OS: a WIFIREQUEST (WiFiCx) or an NDIS_OID_REQUEST (WDI). */
typedef PVOID AIC_REQ;

#define AIC_WIFI_TAG        'w8cA'
#define AIC_WIFI_RING       32          /* requests waiting for the worker */
#define AIC_WIFI_BSS_MAX    64          /* networks per scan */
#define AIC_WIFI_BODY_BUF   0xC000      /* their frame bodies */
#define AIC_WIFI_BSS_PER_IND 16         /* entries per BSS_ENTRY_LIST */
#define AIC_WIFI_CONN_BODY  1024        /* the candidate's body, for ASSOCIATION_RESULT */
#define AIC_WIFI_TRACE      64          /* message IDs kept for WdiTrace */
#define AIC_WIFI_SCAN_MS    10000       /* a scan task ends (aborted) after this */
#define AIC_WIFI_FW_SCAN_MS 8000        /* a firmware scan still running is given up after this */
#define AIC_WIFI_M4_WAIT_MS 200         /* a pairwise key waits this long at most for message 4 to go out */

#ifndef NDIS_STATUS_REQUEST_ABORTED
#define NDIS_STATUS_REQUEST_ABORTED ((NDIS_STATUS)0xC001000CL)
#endif

typedef struct _AIC_WIFI_BSS {
    UCHAR  Bssid[6];
    UCHAR  Channel;
    UCHAR  Band;            /* 0 2.4 GHz, 1 5 GHz */
    CHAR   Rssi;
    BOOLEAN Probe;          /* the body is a probe response (else a beacon) */
    USHORT Len;
    ULONG  Off;             /* into Body */
} AIC_WIFI_BSS;

typedef struct _AIC_WIFI_TASK {
    BOOLEAN Active;         /* M3 sent, M4 not yet */
    BOOLEAN Aborted;
    UINT16  PortId;
    UINT32  Tid;
    UINT32  Ihv;
    ULONG   StartMs;
} AIC_WIFI_TASK;

typedef struct _AIC_WIFI {
    PAIC_DEVICE_CONTEXT Ctx;
    TLV_CONTEXT Tlv;                    /* PeerVersion: the OS's WDI version */

    KSPIN_LOCK Lock;                    /* the ring and Accepting */
    AIC_REQ Ring[AIC_WIFI_RING];
    ULONG RingHead, RingCount;
    BOOLEAN Accepting;                  /* capabilities reported (WiFiCx) or adapter allocated (WDI) */

    BOOLEAN HwReady;                    /* firmware up, capabilities reported */
    BOOLEAN RadioOn;
    BOOLEAN AdapterUp;
    UINT16  PortId;                     /* the station adapter's */

    AIC_WIFI_TASK Scan;
    BOOLEAN FwScanning;                 /* SCANU_START sent, its end not seen */
    ULONG   FwScanStartMs;
    ULONG   BssCount, BssSent, BodyUsed;
    AIC_WIFI_BSS Bss[AIC_WIFI_BSS_MAX];

    AIC_WIFI_TASK Conn;
    BOOLEAN Connected;                  /* ASSOCIATION_RESULT (success) sent, no DISASSOCIATION since */
    UCHAR   ConnBssid[6];
    USHORT  ConnBodyLen;
    UCHAR   ConnBody[AIC_WIFI_CONN_BODY];
    ULONG   ConnTimeoutMs;

    /* records (Device Parameters) */
    ULONG Msgs, Unsupported, Indications, IndicationErrors, Keys, KeyErrors, ScanTasks, ConnectTasks;
    ULONG BssIndicated, Disassociations, LastConnectStatus, LastKeyStatus, Refused;
    ULONG KeyEapolWaitMs;               /* the last pairwise key: how long message 4 took to go out */
    u32   EapolRx0, EapolTx0;           /* the EAPOL counters at this connection's start or last pairwise key */
    USHORT Trace[AIC_WIFI_TRACE];
    ULONG TraceCount;
    ULONG LastRecordMs;

    PVOID Fe;                           /* the front end's own state (WDI) */

    UCHAR Body[AIC_WIFI_BODY_BUF];
} AIC_WIFI, *PAIC_WIFI;

/* wdicore.cpp */
PAIC_WIFI AicWifiAlloc(_In_ PAIC_DEVICE_CONTEXT Ctx);
VOID AicWifiAccept(_In_ PAIC_WIFI W);
/* Any IRQL <= DISPATCH_LEVEL: queue a request for the worker and wake it.
 * FALSE (with the reason) if it was not taken; the caller completes it. */
BOOLEAN AicWifiQueue(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _Out_ NTSTATUS *Fail);
VOID AicWifiIndicate(_In_ PAIC_WIFI W, _In_ UINT16 MessageId, _In_ UINT16 PortId, _In_ UINT32 Tid, _In_ UINT32 Ihv,
                     _In_ NDIS_STATUS Status, _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);
ULONG AicWifiNowMs(VOID);

/* The front end (wificx.cpp or wdi.cpp). All on the worker thread at
 * PASSIVE_LEVEL, except AicFeReqComplete, which AicWifiPause may call from
 * a PnP callback. */
PWDI_MESSAGE_HEADER AicFeReqBuffer(_In_ AIC_REQ Request, _Out_ UINT *InLen, _Out_ UINT *OutLen);
UINT16 AicFeReqId(_In_ AIC_REQ Request);
/* M3: complete the request; Bytes is what was written, the header included. */
VOID AicFeReqComplete(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ NTSTATUS Status, _In_ ULONG Bytes);
/* An indication (M4 or unsolicited). FALSE if it could not be made. */
BOOLEAN AicFeIndicate(_In_ PAIC_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *Header,
                      _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);
/* A message the core does not handle; TRUE if the front end completed it. */
BOOLEAN AicFeDispatch(_In_ PAIC_WIFI W, _In_ AIC_REQ Request, _In_ UINT16 Id, _In_ PWDI_MESSAGE_HEADER H,
                      _In_ UINT InLen, _In_ UINT OutLen);
/* First thing on the worker thread (WDI: the bring-up and the open's completion). */
VOID AicFeWorkerStart(_In_ PAIC_WIFI W);
/* Each turn of the worker's loop (WDI: TX restart, RX that waits). */
VOID AicFeService(_In_ PAIC_WIFI W);
