/*
 * hid.c - the adc-keys as HID buttons, through vhf.sys
 *
 * One fixed report descriptor with three top-level collections, so the
 * device can be created before _DSD says which keys exist:
 *
 *   report 1  Consumer Control: two 16-bit usage slots (volume, mute, media,
 *             AC Back/Home/Search, ...), which Windows acts on in-box
 *   report 2  System Control: Power Down / Sleep / Wake Up
 *   report 3  Vendor page 0xFF00, usage 1: eight buttons for keys with no
 *             standard meaning (TaishanPi 3M Recovery, the Maskrom keys)
 *             which a program opens with HidD_* / ReadFile; Windows does
 *             nothing with them by itself
 *
 * The usage of each key comes from dsd.c; a report is sent only for the
 * collection whose state changed.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "rk3xsaradc.h"

const UCHAR RkAdcReportDescriptor[] = {
    0x05, 0x0C,             /* Usage Page (Consumer) */
    0x09, 0x01,             /* Usage (Consumer Control) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, RKADC_REPORT_CONSUMER, /* Report ID (1) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x03,       /*   Logical Maximum (0x3FF) */
    0x19, 0x00,             /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x03,       /*   Usage Maximum (0x3FF) */
    0x75, 0x10,             /*   Report Size (16) */
    0x95, 0x02,             /*   Report Count (2) */
    0x81, 0x00,             /*   Input (Data, Array, Absolute) */
    0xC0,                   /* End Collection */

    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x80,             /* Usage (System Control) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, RKADC_REPORT_SYSTEM, /* Report ID (2) */
    0x19, 0x81,             /*   Usage Minimum (System Power Down) */
    0x29, 0x83,             /*   Usage Maximum (System Wake Up) */
    0x15, 0x01,             /*   Logical Minimum (1) */
    0x25, 0x03,             /*   Logical Maximum (3) */
    0x75, 0x02,             /*   Report Size (2) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x00,             /*   Input (Data, Array, Absolute) */
    0x75, 0x06,             /*   Report Size (6) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x03,             /*   Input (Constant): padding */
    0xC0,                   /* End Collection */

    0x06, 0x00, 0xFF,       /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,             /* Usage (1) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, RKADC_REPORT_VENDOR, /* Report ID (3) */
    0x19, 0x01,             /*   Usage Minimum (1) */
    0x29, 0x08,             /*   Usage Maximum (8) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x08,             /*   Report Count (8) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0xC0,                   /* End Collection */
};

const USHORT RkAdcReportDescriptorLength = (USHORT)sizeof(RkAdcReportDescriptor);

static
VOID
RkAdcSubmit(
    _In_ PRKADC_CONTEXT ctx,
    _In_reads_(length) PUCHAR report,
    _In_ ULONG length
    )
{
    HID_XFER_PACKET packet;
    NTSTATUS status;

    packet.reportBuffer = report;
    packet.reportBufferLen = length;
    packet.reportId = report[0];
    /* VHF copies the report; the stack buffer may go away. */
    status = VhfReadReportSubmit(ctx->Vhf, &packet);
    if (NT_SUCCESS(status)) {
        ctx->HidReports++;
    } else {
        RKADC_LOG("VhfReadReportSubmit(%u) failed 0x%08X", report[0], status);
    }
}

/* Lock held.  key: index into the keymap, or SARADC_NO_KEY. */
VOID
RkAdcHidReport(
    _In_ PRKADC_CONTEXT ctx,
    _In_ int key
    )
{
    SARADC_HID_STATE now;
    UCHAR report[5];

    if (ctx->Vhf == NULL || !ctx->VhfStarted) {
        return;
    }
    if (key >= 0 && (ULONG)key < ctx->Keymap.Count) {
        SaradcHidStateFromUsage(ctx->KeyUsage[key], &now);
    } else {
        SaradcHidStateFromUsage(0, &now);
    }

    if (now.Consumer != ctx->HidLast.Consumer) {
        report[0] = RKADC_REPORT_CONSUMER;
        report[1] = (UCHAR)(now.Consumer & 0xFF);
        report[2] = (UCHAR)(now.Consumer >> 8);
        report[3] = 0;
        report[4] = 0;
        RkAdcSubmit(ctx, report, 5);
    }
    if (now.System != ctx->HidLast.System) {
        report[0] = RKADC_REPORT_SYSTEM;
        report[1] = (UCHAR)(now.System & 0x3);
        RkAdcSubmit(ctx, report, 2);
    }
    if (now.Vendor != ctx->HidLast.Vendor) {
        report[0] = RKADC_REPORT_VENDOR;
        report[1] = (UCHAR)now.Vendor;
        RkAdcSubmit(ctx, report, 2);
    }
    ctx->HidLast = now;
}
