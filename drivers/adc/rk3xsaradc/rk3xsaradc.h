/*
 * rk3xsaradc.h - RK3576 SARADC driver, shared declarations
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RK3XSARADC_H
#define RK3XSARADC_H

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>

#include "rk3xsaradc_ioctl.h"
#include "saradc_logic.h"

#define RKADC_POOL_TAG          'cdAR'

#define RKADC_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "rk3xsaradc: " fmt "\n", __VA_ARGS__)

/*
 * Registers, RK3576 TRM V1.2 Part 1 chapter 18 (the "v2" layout of Linux
 * drivers/iio/adc/rockchip_saradc.c, which RK3576 shares with RK3588 and
 * RK3528).  Registers with a write-enable half take the mask in [31:16].
 */
#define SARADC_CONV_CON         0x000
#define SARADC_T_PD_SOC         0x004
#define SARADC_T_DAS_SOC        0x00C
#define SARADC_END_INT_EN       0x104
#define SARADC_STATUS           0x10C
#define SARADC_END_INT_ST       0x110
#define SARADC_DATA(ch)         (0x120 + 4 * (ch))

#define SARADC_CONV_CHANNEL     0x0Fu       /* [3:0] channel_sel */
#define SARADC_CONV_START       0x10u       /* [4] start_adc, W1C */
#define SARADC_CONV_SINGLE      0x20u       /* [5] single_pd_mode */
#define SARADC_END_INT          0x01u
#define SARADC_DATA_MASK        0xFFFu
#define SARADC_REGS_MIN         0x140       /* through DATA7 */

#define RKADC_CHANNELS          8u
#define RKADC_BITS              12u
#define RKADC_DEFAULT_VREF_UV   1800000u     /* vref-supply of every RK3576 board: vcca_1v8 */
#define RKADC_DEFAULT_POLL_MS   100u         /* adc-keys poll-interval of the boards' trees */
#define RKADC_MIN_POLL_MS       20u
#define RKADC_MAX_POLL_MS       2000u
#define RKADC_DEFAULT_KEY_SAMPLES 2u
#define RKADC_KEY_ARM_SAMPLES   3u           /* key-up polls in a row before any key counts */
#define RKADC_DEFAULT_JACK_MS   150u         /* simple-audio-card's hp-det debounce */
#define RKADC_MAX_JACK_MS       2000u
/* At 1 MHz (the firmware's rate) a conversion takes about 60 us. */
#define RKADC_CONV_TIMEOUT_US   2000u
#define RKADC_MAX_TIMEOUTS      10u          /* in a row: stop polling */

/* HID report ids of the descriptor in hid.c */
#define RKADC_REPORT_CONSUMER   1
#define RKADC_REPORT_SYSTEM     2
#define RKADC_REPORT_VENDOR     3

typedef struct _RKADC_CONTEXT {
    WDFDEVICE Device;
    PUCHAR Regs;
    SIZE_T RegsLength;
    WDFWAITLOCK Lock;               /* the converter, the state below */
    WDFTIMER Timer;
    WDFWORKITEM JackWork;

    VHFHANDLE Vhf;
    BOOLEAN VhfStarted;

    /* configuration: _DSD, then the registry */
    NTSTATUS DsdStatus;
    ULONG VrefMicrovolts;
    ULONG PollMs;

    BOOLEAN HaveKeys;
    ULONG KeysChannel;
    SARADC_KEYMAP Keymap;
    ULONG KeyCode[RK3X_SARADC_MAX_KEYS];
    ULONG KeyUsage[RK3X_SARADC_MAX_KEYS];
    ULONG KeySamples;
    SARADC_DEBOUNCE KeyDebounce;
    SARADC_KEYARM KeyArm;           /* no key until seen released (start, resume) */
    BOOLEAN KeyArmSaved;            /* DiagKeysArmed written since the last arm */
    SARADC_HID_STATE HidLast;

    BOOLEAN HaveJack;
    ULONG JackChannel;
    SARADC_JACK Jack;
    ULONG JackSequence;
    ULONG JackMicrovolts;
    PCALLBACK_OBJECT JackCallback;
    PVOID JackCallbackReg;

    /* the controller reset the firmware offers (Saradc.asl, RSTC) */
    BOOLEAN HaveReset;
    ULONG LastChannel;
    ULONG Resets;

    /* state */
    BOOLEAN Polling;
    BOOLEAN Stopped;
    ULONG LastRaw[RKADC_CHANNELS];
    ULONG Conversions;
    ULONG Timeouts;
    ULONG TimeoutsInRow;
    ULONG Notifications;
    ULONG HidReports;
} RKADC_CONTEXT, *PRKADC_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RKADC_CONTEXT, RkAdcGetContext)

/* driver.c */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS RkAdcConvertLocked(_In_ PRKADC_CONTEXT ctx, _In_ ULONG channel, _Out_ PULONG raw);
VOID RkAdcDiagSave(_In_ PRKADC_CONTEXT ctx);

/* dsd.c */
_IRQL_requires_(PASSIVE_LEVEL)
VOID RkAdcReadConfig(_In_ PRKADC_CONTEXT ctx);

/* hid.c */
extern const UCHAR RkAdcReportDescriptor[];
extern const USHORT RkAdcReportDescriptorLength;
VOID RkAdcHidReport(_In_ PRKADC_CONTEXT ctx, _In_ int key);

/* jack.c */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS RkAdcJackOpen(_In_ PRKADC_CONTEXT ctx);
_IRQL_requires_(PASSIVE_LEVEL)
VOID RkAdcJackClose(_In_ PRKADC_CONTEXT ctx);
_IRQL_requires_(PASSIVE_LEVEL)
VOID RkAdcJackNotify(_In_ PRKADC_CONTEXT ctx);
VOID RkAdcJackInfo(_In_ PRKADC_CONTEXT ctx, _Out_ PRK3X_SARADC_JACK_INFO info);
EVT_WDF_WORKITEM RkAdcJackWork;

/* ioctl.c */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RkAdcEvtIoDeviceControl;

#endif /* RK3XSARADC_H */
