/*
 * hid.c - the HID touch screen this driver presents, and how GT9271 frames
 * become its reports
 *
 * One top-level collection, Digitizer / Touch Screen, the layout Windows
 * requires of an in-box touch screen ("Required HID Top-Level Collections",
 * Microsoft hardware design guidelines):
 *
 *   input report 1     ten finger collections, each with Tip Switch,
 *                      Contact Identifier, X and Y; then Scan Time and
 *                      Contact Count
 *   feature report 2   Contact Count Maximum = 10
 *
 * No Device Certification Status (THQA) report: it is optional since
 * Windows 10, and without it the control panel says "Windows Touch Support"
 * instead of "Full Windows Touch Support".
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#include "gt9271.h"

#define LO(v)   ((UCHAR)((v) & 0xFF))
#define HI(v)   ((UCHAR)(((v) >> 8) & 0xFF))

/*
 * One finger: 6 bytes in the report (GT_HID_CONTACT). The global items X and
 * Y set (units, physical range) are put back to none at the end, so that the
 * next finger's tip switch does not inherit a length unit.
 */
#define GT_FINGER_COLLECTION                                                    \
    0x05, 0x0D,                     /*   USAGE_PAGE (Digitizers)         */     \
    0x09, 0x22,                     /*   USAGE (Finger)                  */     \
    0xA1, 0x02,                     /*   COLLECTION (Logical)            */     \
    0x09, 0x42,                     /*     USAGE (Tip Switch)            */     \
    0x15, 0x00,                     /*     LOGICAL_MINIMUM (0)           */     \
    0x25, 0x01,                     /*     LOGICAL_MAXIMUM (1)           */     \
    0x75, 0x01,                     /*     REPORT_SIZE (1)               */     \
    0x95, 0x01,                     /*     REPORT_COUNT (1)              */     \
    0x81, 0x02,                     /*     INPUT (Data,Var,Abs)          */     \
    0x95, 0x07,                     /*     REPORT_COUNT (7)              */     \
    0x81, 0x03,                     /*     INPUT (Cnst,Var,Abs)          */     \
    0x09, 0x51,                     /*     USAGE (Contact Identifier)    */     \
    0x26, 0xFF, 0x00,               /*     LOGICAL_MAXIMUM (255)         */     \
    0x75, 0x08,                     /*     REPORT_SIZE (8)               */     \
    0x95, 0x01,                     /*     REPORT_COUNT (1)              */     \
    0x81, 0x02,                     /*     INPUT (Data,Var,Abs)          */     \
    0x05, 0x01,                     /*     USAGE_PAGE (Generic Desktop)  */     \
    0x75, 0x10,                     /*     REPORT_SIZE (16)              */     \
    0x55, 0x0E,                     /*     UNIT_EXPONENT (-2)            */     \
    0x65, 0x11,                     /*     UNIT (Centimeter, SI Linear)  */     \
    0x35, 0x00,                     /*     PHYSICAL_MINIMUM (0)          */     \
    0x09, 0x30,                     /*     USAGE (X)                     */     \
    0x26, LO(GT_LOGICAL_MAX_X), HI(GT_LOGICAL_MAX_X),                          \
    0x46, LO(GT_PHYSICAL_MAX_X), HI(GT_PHYSICAL_MAX_X),                        \
    0x81, 0x02,                     /*     INPUT (Data,Var,Abs)          */     \
    0x09, 0x31,                     /*     USAGE (Y)                     */     \
    0x26, LO(GT_LOGICAL_MAX_Y), HI(GT_LOGICAL_MAX_Y),                          \
    0x46, LO(GT_PHYSICAL_MAX_Y), HI(GT_PHYSICAL_MAX_Y),                        \
    0x81, 0x02,                     /*     INPUT (Data,Var,Abs)          */     \
    0x45, 0x00,                     /*     PHYSICAL_MAXIMUM (0)          */     \
    0x55, 0x00,                     /*     UNIT_EXPONENT (0)             */     \
    0x65, 0x00,                     /*     UNIT (None)                   */     \
    0xC0                            /*   END_COLLECTION                  */

UCHAR GtReportDescriptor[] = {
    0x05, 0x0D,                     /* USAGE_PAGE (Digitizers)           */
    0x09, 0x04,                     /* USAGE (Touch Screen)              */
    0xA1, 0x01,                     /* COLLECTION (Application)          */
    0x85, GT_REPORT_ID_TOUCH,       /*   REPORT_ID (1)                   */

    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,
    GT_FINGER_COLLECTION,

    0x05, 0x0D,                     /*   USAGE_PAGE (Digitizers)         */
    0x55, 0x0C,                     /*   UNIT_EXPONENT (-4)              */
    0x66, 0x01, 0x10,               /*   UNIT (Seconds, SI Linear)       */
    0x47, 0xFF, 0xFF, 0x00, 0x00,   /*   PHYSICAL_MAXIMUM (65535)        */
    0x27, 0xFF, 0xFF, 0x00, 0x00,   /*   LOGICAL_MAXIMUM (65535)         */
    0x75, 0x10,                     /*   REPORT_SIZE (16)                */
    0x95, 0x01,                     /*   REPORT_COUNT (1)                */
    0x09, 0x56,                     /*   USAGE (Scan Time), 100 us units */
    0x81, 0x02,                     /*   INPUT (Data,Var,Abs)            */
    0x45, 0x00,                     /*   PHYSICAL_MAXIMUM (0)            */
    0x55, 0x00,                     /*   UNIT_EXPONENT (0)               */
    0x65, 0x00,                     /*   UNIT (None)                     */

    0x09, 0x54,                     /*   USAGE (Contact Count)           */
    0x25, GT_MAX_POINTS,            /*   LOGICAL_MAXIMUM (10)            */
    0x75, 0x08,                     /*   REPORT_SIZE (8)                 */
    0x95, 0x01,                     /*   REPORT_COUNT (1)                */
    0x81, 0x02,                     /*   INPUT (Data,Var,Abs)            */

    0x85, GT_REPORT_ID_MAX_COUNT,   /*   REPORT_ID (2)                   */
    0x09, 0x55,                     /*   USAGE (Contact Count Maximum)   */
    0x25, GT_MAX_POINTS,            /*   LOGICAL_MAXIMUM (10)            */
    0xB1, 0x02,                     /*   FEATURE (Data,Var,Abs)          */
    0xC0                            /* END_COLLECTION                    */
};

USHORT GtReportDescriptorLength = sizeof(GtReportDescriptor);

/*
 * IOCTL_HID_GET_FEATURE, through VHF. The only feature is the contact
 * count maximum. The buffer starts with the report ID.
 */
VOID
GtEvtVhfGetFeature(
    _In_ PVOID VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PVOID VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
    )
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    GT_HID_FEATURE_MAX_COUNT *feature;

    UNREFERENCED_PARAMETER(VhfClientContext);
    UNREFERENCED_PARAMETER(VhfOperationContext);

    if (HidTransferPacket->reportId == GT_REPORT_ID_MAX_COUNT) {
        if (HidTransferPacket->reportBufferLen >= sizeof(GT_HID_FEATURE_MAX_COUNT)) {
            feature = (GT_HID_FEATURE_MAX_COUNT *)HidTransferPacket->reportBuffer;
            feature->ReportId = GT_REPORT_ID_MAX_COUNT;
            feature->ContactCountMaximum = GT_MAX_POINTS;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
    }

    VhfAsyncOperationComplete(VhfOperationHandle, status);
}

static
VOID
GtSubmit(
    _In_ PGT_DEVICE_CONTEXT Ctx,
    _In_reads_(Count) const GT_HID_CONTACT *Contacts,
    _In_ ULONG Count,
    _In_ USHORT ScanTime
    )
{
    GT_HID_INPUT_REPORT report;
    HID_XFER_PACKET packet;
    NTSTATUS status;
    ULONG i;

    RtlZeroMemory(&report, sizeof(report));
    report.ReportId = GT_REPORT_ID_TOUCH;
    for (i = 0; i < Count && i < GT_MAX_POINTS; i++) {
        report.Contact[i] = Contacts[i];
    }
    report.ScanTime = ScanTime;
    report.ContactCount = (UCHAR)i;

    packet.reportBuffer = (PUCHAR)&report;
    packet.reportBufferLen = sizeof(report);
    packet.reportId = GT_REPORT_ID_TOUCH;

    /* VHF copies the report; the stack buffer may go away. */
    status = VhfReadReportSubmit(Ctx->Vhf, &packet);
    if (!NT_SUCCESS(status)) {
        GT_LOG("VhfReadReportSubmit failed 0x%08X", status);
    }
}

static
VOID
GtMakeContact(
    _Out_ GT_HID_CONTACT *Contact,
    _In_ const GT_POINT *Point,
    _In_ BOOLEAN Tip
    )
{
    Contact->Flags = Tip ? 1 : 0;
    Contact->ContactId = Point->Id;
    Contact->X = Point->X;
    Contact->Y = Point->Y;
}

static
LONG
GtFind(
    _In_reads_(Count) const GT_POINT *Points,
    _In_ ULONG Count,
    _In_ UCHAR Id
    )
{
    ULONG i;

    for (i = 0; i < Count; i++) {
        if (Points[i].Id == Id) {
            return (LONG)i;
        }
    }
    return -1;
}

/*
 * Turn one controller frame (the contacts now down) into reports.
 *
 * A contact that was down and is not any more is reported once more with
 * the tip switch clear, at the last position it had, which is how Windows
 * learns about a lift. A frame must not carry more contacts than the
 * maximum, or Windows drops the whole frame; when lifts and new contacts
 * together are more than ten, the lifts go in a frame of their own first.
 *
 * Called with Ctx->Lock held.
 */
VOID
GtReportFrame(
    _In_ PGT_DEVICE_CONTEXT Ctx,
    _In_reads_opt_(Count) const GT_POINT *Now,
    _In_ ULONG Count
    )
{
    GT_HID_CONTACT contacts[GT_MAX_POINTS * 2];
    ULONG n = 0;
    ULONG i;
    LONG j;
    ULONG newCount = 0;
    USHORT scanTime;

    if (!Ctx->VhfStarted) {
        return;
    }

    /* 100 us units, rolling over at 16 bits. */
    scanTime = (USHORT)(KeQueryInterruptTime() / 1000);

    /* Contacts that were down: still down (new position) or lifted. */
    for (i = 0; i < Ctx->DownCount; i++) {
        j = GtFind(Now, Count, Ctx->Down[i].Id);
        if (j >= 0) {
            GtMakeContact(&contacts[n++], &Now[j], TRUE);
        } else {
            GtMakeContact(&contacts[n++], &Ctx->Down[i], FALSE);
        }
    }

    for (i = 0; i < Count; i++) {
        if (GtFind(Ctx->Down, Ctx->DownCount, Now[i].Id) < 0) {
            newCount++;
        }
    }

    if (n + newCount > GT_MAX_POINTS) {
        /* Lifts and moves first, then everything that is down now. */
        GtSubmit(Ctx, contacts, n, scanTime);
        n = 0;
        for (i = 0; i < Count; i++) {
            GtMakeContact(&contacts[n++], &Now[i], TRUE);
        }
        GtSubmit(Ctx, contacts, n, (USHORT)(scanTime + 1));
    } else {
        for (i = 0; i < Count; i++) {
            if (GtFind(Ctx->Down, Ctx->DownCount, Now[i].Id) < 0) {
                GtMakeContact(&contacts[n++], &Now[i], TRUE);
            }
        }
        /* Nothing down before and nothing now: no report at all. */
        if (n != 0) {
            GtSubmit(Ctx, contacts, n, scanTime);
        }
    }

    for (i = 0; i < Count; i++) {
        Ctx->Down[i] = Now[i];
    }
    Ctx->DownCount = Count;
}
