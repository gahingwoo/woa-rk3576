/*
 * rk3xsaradc_ioctl.h - the interfaces of rk3xsaradc.sys
 *
 * Two consumers, two interfaces:
 *
 *   User mode (tools/adctest): \\.\RK3XSARADC and the IOCTLs below.
 *
 *   Kernel mode (es8323.sys, for the headphone jack): the callback object
 *   \Callback\Rk3xSaradcJack.  Open it with ExCreateCallback (Create TRUE,
 *   AllowMultipleCallbacks TRUE, so the order the two drivers start in does
 *   not matter), ExRegisterCallback a function, then ExNotifyCallback it
 *   with Argument1 = RK3X_SARADC_JACK_QUERY and Argument2 = NULL.  The
 *   SARADC driver answers from a work item, so not inside the query call:
 *
 *     Argument1 = RK3X_SARADC_JACK_STATE
 *     Argument2 = PRK3X_SARADC_JACK_INFO, valid only during the call
 *
 *   and sends the same notification every time the debounced state changes.
 *   Notifications come at PASSIVE_LEVEL.  A callback must ignore any
 *   Argument1 it does not know (RK3X_SARADC_JACK_QUERY included: it sees
 *   other drivers' queries and its own), and must not call ExNotifyCallback
 *   on the object from inside the callback.
 *
 *   No notification ever comes when the firmware describes no jack channel,
 *   or when the SARADC driver is not installed: a consumer keeps its own
 *   default (es8323: endpoint plugged) until the first STATE arrives.
 *
 * Shared by the driver, the test tool and es8323; include after <windows.h>
 * or <ntddk.h>, or with RK3X_SARADC_STANDALONE for a host build.
 *
 * MIT, unlike the driver (GPL-2.0-only), so that es8323 (MIT) can include
 * it: it is an interface, constants and structures only.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef RK3X_SARADC_IOCTL_H
#define RK3X_SARADC_IOCTL_H

/* \\.\RK3XSARADC */
#define RK3X_SARADC_DOS_NAME        L"\\DosDevices\\RK3XSARADC"
#define RK3X_SARADC_USER_PATH       L"\\\\.\\RK3XSARADC"

#define RK3X_SARADC_INTERFACE_VERSION   1

#define RK3X_SARADC_MAX_CHANNELS    8
#define RK3X_SARADC_MAX_KEYS        8

#ifndef CTL_CODE
#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define METHOD_BUFFERED   0
#define FILE_ANY_ACCESS   0
#define FILE_READ_ACCESS  1
#define FILE_WRITE_ACCESS 2
#endif

#define RK3X_SARADC_DEVICE_TYPE     0x8ADCu     /* unsigned: << 16 does not fit an int */

/* out RK3X_SARADC_INFO */
#define IOCTL_RK3X_SARADC_GET_INFO  CTL_CODE(RK3X_SARADC_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* in RK3X_SARADC_READ_IN, out RK3X_SARADC_READ_OUT: one conversion, now */
#define IOCTL_RK3X_SARADC_READ_RAW  CTL_CODE(RK3X_SARADC_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
/* out RK3X_SARADC_JACK_INFO: the debounced jack state */
#define IOCTL_RK3X_SARADC_GET_JACK  CTL_CODE(RK3X_SARADC_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* RK3X_SARADC_INFO.Flags */
#define RK3X_SARADC_FLAG_KEYS       0x1     /* adc-keys configured, HID started */
#define RK3X_SARADC_FLAG_JACK       0x2     /* a jack channel configured */
#define RK3X_SARADC_FLAG_POLLING    0x4     /* the poll timer is running */
#define RK3X_SARADC_FLAG_STOPPED    0x8     /* polling given up after repeated timeouts */

#define RK3X_SARADC_NO_CHANNEL      0xFFFFFFFFu

typedef struct _RK3X_SARADC_INFO {
    unsigned int Size;              /* sizeof (RK3X_SARADC_INFO) */
    unsigned int Version;           /* RK3X_SARADC_INTERFACE_VERSION */
    unsigned int Flags;
    unsigned int Channels;          /* 8 on RK3576 */
    unsigned int Bits;              /* 12 */
    unsigned int VrefMicrovolts;    /* rockchip,vref-microvolt, 1800000 */
    unsigned int PollIntervalMs;
    unsigned int KeysChannel;       /* RK3X_SARADC_NO_CHANNEL without keys */
    unsigned int KeyCount;
    unsigned int KeyupMicrovolts;
    unsigned int KeyPressMicrovolts[RK3X_SARADC_MAX_KEYS];
    unsigned int KeyCode[RK3X_SARADC_MAX_KEYS];     /* Linux input code from the firmware */
    unsigned int KeyHidUsage[RK3X_SARADC_MAX_KEYS]; /* page << 16 | usage, as reported */
    int          KeyPressed;        /* index of the key held now, -1 for none */
    unsigned int JackChannel;       /* RK3X_SARADC_NO_CHANNEL without a jack */
    unsigned int JackPluggedBelowMicrovolts;
    unsigned int JackUnpluggedAboveMicrovolts;
    unsigned int JackDebounceMs;
    unsigned int JackConnected;     /* 0, 1, or 0xFFFFFFFF before the first decision */
    unsigned int LastRaw[RK3X_SARADC_MAX_CHANNELS];  /* 0xFFFFFFFF = never read */
    unsigned int Conversions;
    unsigned int Timeouts;
    unsigned int Notifications;     /* jack STATE notifications sent */
    unsigned int HidReports;
    unsigned int Resets;            /* controller resets through the firmware's RSTC */
    unsigned int ResetAvailable;    /* 1 when \_SB.ADC0.RSTC exists */
} RK3X_SARADC_INFO, *PRK3X_SARADC_INFO;

typedef struct _RK3X_SARADC_READ_IN {
    unsigned int Channel;           /* 0..7 */
} RK3X_SARADC_READ_IN, *PRK3X_SARADC_READ_IN;

typedef struct _RK3X_SARADC_READ_OUT {
    unsigned int Channel;
    unsigned int Raw;               /* 0..4095 */
    unsigned int Microvolts;        /* Raw * Vref / 4096 */
} RK3X_SARADC_READ_OUT, *PRK3X_SARADC_READ_OUT;

/* ------------------------------------------------------------------ jack */

#define RK3X_SARADC_JACK_CALLBACK_NAME  L"\\Callback\\Rk3xSaradcJack"

/* Argument1 of a notification on \Callback\Rk3xSaradcJack */
#define RK3X_SARADC_JACK_STATE      ((void *)(unsigned long long)0x4A41434Bu)  /* 'JACK' */
#define RK3X_SARADC_JACK_QUERY      ((void *)(unsigned long long)0x4A515259u)  /* 'JQRY' */

typedef struct _RK3X_SARADC_JACK_INFO {
    unsigned int Size;              /* sizeof (RK3X_SARADC_JACK_INFO) */
    unsigned int Known;             /* 0 until the first debounced decision */
    unsigned int Connected;         /* 1 plugged, 0 unplugged */
    unsigned int Microvolts;        /* the sample the decision was made on */
    unsigned int Sequence;          /* +1 per change */
} RK3X_SARADC_JACK_INFO, *PRK3X_SARADC_JACK_INFO;

#endif /* RK3X_SARADC_IOCTL_H */
