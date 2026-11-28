/*
 * wdicore.h - the WDI message handling shared by both Wi-Fi front ends of
 * bcmwlsdio: WiFiCx 1.0 on Windows 11 (wificx.cpp, bcmwlsdio.sys) and the
 * NDIS WDI miniport on Windows 10 (wdi.cpp, bcmwlwdi.sys). WiFiCx carries
 * the same WDI messages (WDI_MESSAGE_HEADER and TLVs) as WDI: scan,
 * connect, keys, radio, reset and abort are one code (wdicore.cpp), built
 * against the TLV library of each (WificxTLVGenParse.lib, km\wlan\2.0;
 * TLVGeneratorParser.lib, km\wlan\1.0). The front ends move the messages
 * to and from the OS (BwlFe*) and report the capabilities.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "bcmwlsdio.h"
#ifdef BWL_WDI
#include <dot11wdi.h>
#include <wditypes.hpp>
#define BWL_FE_NAME "wdi"
#else
#include <wificx.h>
#define BWL_FE_NAME "wificx"
#endif
#include <TlvGeneratorParser.hpp>

/* A request from the OS: a WIFIREQUEST (WiFiCx) or an NDIS_OID_REQUEST (WDI). */
typedef PVOID BWL_REQ;

#define BWL_WIFI_TAG        'wWcB'
#define BWL_WIFI_RING       32          /* WIFIREQUESTs waiting for the worker */
#define BWL_WIFI_BSS_MAX    64          /* networks per scan */
#define BWL_WIFI_BODY_BUF   0xC000      /* their frame bodies */
#define BWL_WIFI_BSS_PER_IND 16         /* entries per BSS_ENTRY_LIST */
#define BWL_WIFI_CONN_BODY  1024        /* the candidate's body, for ASSOCIATION_RESULT */
#define BWL_WIFI_TRACE      64          /* message IDs kept for WdiTrace */
#define BWL_WIFI_SCAN_MS    10000       /* a scan task ends (aborted) after this */
#define BWL_WIFI_FW_SCAN_MS 8000        /* a firmware scan still running is given up after this */
#define BWL_WIFI_LINK_MS    5000        /* rate and signal are read this often while connected */
#define BWL_WIFI_M4_WAIT_MS 200         /* a pairwise key waits this long at most for message 4 to go out */

#ifndef NDIS_STATUS_REQUEST_ABORTED
#define NDIS_STATUS_REQUEST_ABORTED ((NDIS_STATUS)0xC001000CL)
#endif

typedef struct _BWL_WIFI_BSS {
    UCHAR  Bssid[6];
    UCHAR  Channel;
    UCHAR  Band;            /* 0 2.4 GHz, 1 5 GHz */
    CHAR   Rssi;
    BOOLEAN Beacon;         /* the body is a beacon (else a probe response) */
    USHORT Len;
    ULONG  Off;             /* into Body */
} BWL_WIFI_BSS;

typedef struct _BWL_WIFI_TASK {
    BOOLEAN Active;         /* M3 sent, M4 not yet */
    BOOLEAN Aborted;
    UINT16  PortId;
    UINT32  Tid;
    UINT32  Ihv;
    ULONG   StartMs;
} BWL_WIFI_TASK;

typedef struct _BWL_WIFI {
    PBWL_DEVICE_CONTEXT Ctx;
    TLV_CONTEXT Tlv;                    /* PeerVersion: the OS's WDI version */

    KSPIN_LOCK Lock;                    /* the ring and Accepting */
    BWL_REQ Ring[BWL_WIFI_RING];
    ULONG RingHead, RingCount;
    BOOLEAN Accepting;                  /* between PrepareHardware and ReleaseHardware */

    BOOLEAN HwReady;                    /* firmware up, capabilities reported */
    BOOLEAN RadioOn;
    BOOLEAN AdapterUp;
    UINT16  PortId;                     /* the station adapter's */

    BWL_WIFI_TASK Scan;
    BOOLEAN FwScanning;                 /* escan sent, its end not seen */
    ULONG   FwScanStartMs;
    ULONG   BssCount, BssSent, BodyUsed;
    BWL_WIFI_BSS Bss[BWL_WIFI_BSS_MAX];

    BWL_WIFI_TASK Conn;
    BOOLEAN Connected;                  /* ASSOCIATION_RESULT (success) sent, no DISASSOCIATION since */
    BOOLEAN ConnFwSup;                  /* this connection: the firmware's supplicant */
    UCHAR   ConnBssid[6];
    UCHAR   ConnChannel, ConnBand;
    CHAR    ConnRssi;
    USHORT  ConnBodyLen;
    UCHAR   ConnBody[BWL_WIFI_CONN_BODY];
    ULONG   ConnTimeoutMs;
    ULONG   LastLinkMs;
    ULONG   ReportedRateKbps;
    LONG    ReportedRssi;

    /* settings */
    ULONG   FwSupMode;                  /* FwSupplicant */
    CHAR    Passphrase[68];
    CHAR    FwSupSsid[36];

    /* records (Device Parameters) */
    ULONG Msgs, Unsupported, Indications, IndicationErrors, Keys, KeyErrors, ScanTasks, ConnectTasks;
    ULONG BssIndicated, Disassociations, LastConnectStatus, LastKeyStatus, Refused, LinkUpdates;
    ULONG ServiceErrors, ServiceErrorRun;
    ULONG KeyEapolWaitMs;               /* the last pairwise key: how long message 4 took to go out */
    u32   EapolRx0, EapolTx0;           /* the EAPOL counters at this connection's start or last pairwise key */
    LONG  LastServiceError;
    USHORT Trace[BWL_WIFI_TRACE];
    ULONG TraceCount;
    ULONG LastRecordMs;

    PVOID Fe;                           /* the front end's own state */

    UCHAR Body[BWL_WIFI_BODY_BUF];
} BWL_WIFI, *PBWL_WIFI;


/* wdicore.cpp */
PBWL_WIFI BwlWifiAlloc(_In_ PBWL_DEVICE_CONTEXT Ctx);
VOID BwlWifiAccept(_In_ PBWL_WIFI W);
BOOLEAN BwlWifiQueue(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _Out_ NTSTATUS *Fail);
VOID BwlWifiIndicate(_In_ PBWL_WIFI W, _In_ UINT16 MessageId, _In_ UINT16 PortId, _In_ UINT32 Tid, _In_ UINT32 Ihv,
                     _In_ NDIS_STATUS Status, _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);

/* The front end (wificx.cpp or wdi.cpp). All on the worker thread at
 * PASSIVE_LEVEL, except BwlFeReqComplete, which BwlWifiPause may call from
 * a PnP callback. */
PWDI_MESSAGE_HEADER BwlFeReqBuffer(_In_ BWL_REQ Request, _Out_ UINT *InLen, _Out_ UINT *OutLen);
UINT16 BwlFeReqId(_In_ BWL_REQ Request);
/* M3: complete the request; Bytes is what was written, the header included. */
VOID BwlFeReqComplete(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ NTSTATUS Status, _In_ ULONG Bytes);
/* An indication (M4 or unsolicited). FALSE if it could not be made. */
BOOLEAN BwlFeIndicate(_In_ PBWL_WIFI W, _In_ UINT16 MessageId, _In_ const WDI_MESSAGE_HEADER *Header,
                      _In_reads_bytes_opt_(TlvLen) const UINT8 *Tlv, _In_ ULONG TlvLen);
/* A message the core does not handle; TRUE if the front end completed it. */
BOOLEAN BwlFeDispatch(_In_ PBWL_WIFI W, _In_ BWL_REQ Request, _In_ UINT16 Id, _In_ PWDI_MESSAGE_HEADER H,
                      _In_ UINT InLen, _In_ UINT OutLen);
/* First thing on the worker thread. */
VOID BwlFeWorkerStart(_In_ PBWL_WIFI W);
