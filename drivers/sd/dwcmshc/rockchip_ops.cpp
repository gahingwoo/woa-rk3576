/*++

Copyright (c) 2024 Mario Bălănică <mariobalanica02@gmail.com>
Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd

SPDX-License-Identifier: MIT

Module Name:

    rockchip_ops.cpp

Abstract:

    Rockchip specific abstractions

Environment:

    Kernel mode only.

--*/

#include "precomp.h"
#pragma hdrstop


#include "trace.h"
#include "rockchip_ops.tmh"

#include "dwcmshc.h"
#include "rockchip_ops.h"

#define NUM_PHASES              360
#define BAD_PHASES_TO_SKIP      20

#define SAMPLE_PHASE_DEFAULT    0

#define RANGE_INDEX_TO_PHASE(_Index, _NumPhases) \
        DIV_CEIL((_Index) * 360, (_NumPhases))

#define PHASE_TO_RANGE_INDEX(_Phase, _NumPhases) \
        DIV_CEIL((_Phase) * (_NumPhases), 360)

#define CRU_SD_CLKGEN_DIV   2

//
// Defined in dwcmshc.cpp.
//
extern BOOLEAN gCrashdumpMode;

//
// RK3576 differs from RK3588 in two ways that matter here, and the BL31 this
// board ships does not implement the Rockchip SiP SD/MMC service the RK3588
// driver calls:
//
//  - the card clock comes from CCLK_SRC_SDMMC0 in the CRU, programmed here
//    directly (mainline clk-rk3576.c: CLKSEL_CON(105), mux [14:13] over
//    gpll/cpll/xin24m, divider [12:7]);
//  - the drive and sample phases live in the controller's own TIMING_CON0/1
//    (mainline dw_mmc-rockchip.c sets internal_phase for rk3576), not in CRU
//    phase clocks.
//
// Parent rates are the ones TF-A's rk3576_clk.c programs.
//

#define RK3576_CRU_CLKSEL_CON105        0x272004A4
#define RK3576_SDMMC_MUX_SHIFT          13
#define RK3576_SDMMC_DIV_SHIFT          7
#define RK3576_SDMMC_WRITE_MASK         (((0x3UL << RK3576_SDMMC_MUX_SHIFT) | \
                                          (0x3FUL << RK3576_SDMMC_DIV_SHIFT)) << 16)
#define RK3576_SDMMC_DIV_MAX            64

#define MSHC_TIMING_CON0                0x130   // drive phase
#define MSHC_TIMING_CON1                0x134   // sample phase
#define ROCKCHIP_MMC_DELAY_SEL          (1UL << 10)
#define ROCKCHIP_MMC_DELAYNUM_OFFSET    2
#define ROCKCHIP_MMC_DELAY_ELEMENT_PSEC 60
#define ROCKCHIP_MMC_TIMING_FIELD       0x0FFEUL        // bits [11:1]

static const ULONG Rk3576SdmmcParentHz[] = {
    1188000000,     // 0: GPLL
    1000000000,     // 1: CPLL
    24000000        // 2: xin24m
};

static
NTSTATUS
MshcRk3576SetPhase(
    _In_ PMSHC_EXTENSION MshcExtension,
    _In_ BOOLEAN Sample,
    _In_ ULONG Degrees
    )
{
    ULONG Rate;
    ULONG Nineties;
    ULONG Remainder;
    ULONG64 Delay;
    ULONG64 Denominator;
    ULONG DelayNum;
    ULONG RawValue;

    //
    // The card clock, i.e. the CRU rate after the fixed divide-by-two.
    //
    Rate = MshcExtension->RkCardClockHz;
    if (Rate < 1000) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    Degrees %= 360;
    Nineties = Degrees / 90;
    Remainder = Degrees % 90;

    //
    // Same arithmetic as mainline rockchip_mmc_set_internal_phase(): the
    // remainder as a fraction of a clock period, in 60 ps delay elements.
    //
    Delay = 10000000ULL * Remainder;
    Denominator = (ULONG64)(Rate / 1000) * 36 * (ROCKCHIP_MMC_DELAY_ELEMENT_PSEC / 10);
    Delay = (Delay + Denominator / 2) / Denominator;
    DelayNum = (Delay > 255) ? 255 : (ULONG)Delay;

    RawValue = DelayNum ? ROCKCHIP_MMC_DELAY_SEL : 0;
    RawValue |= DelayNum << ROCKCHIP_MMC_DELAYNUM_OFFSET;
    RawValue |= Nineties;

    //
    // FIELD_PREP_WM16(GENMASK(11, 1), RawValue): into bits [11:1], with the
    // matching write-enable bits in the upper half.
    //
    MshcWriteRegister(
        MshcExtension,
        Sample ? MSHC_TIMING_CON1 : MSHC_TIMING_CON0,
        ((RawValue << 1) & ROCKCHIP_MMC_TIMING_FIELD) | (ROCKCHIP_MMC_TIMING_FIELD << 16));

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
MshcRockchipSetClock(
    PMSHC_EXTENSION MshcExtension,
    ULONG FrequencyKhz
    )
{
    ULONG TargetHz;
    ULONG BestHz;
    ULONG BestMux;
    ULONG BestDiv;
    ULONG Mux;

    //
    // Map CCLK_SRC_SDMMC0 on first use. It lies outside this device's _CRS.
    // Not in crashdump mode, where the memory manager is off limits: there the
    // clock stays where the running system left it, which is why crashdump to
    // an SD card is not supported on RK3576.
    //
    if ((MshcExtension->RkCruClkSel == NULL) && !gCrashdumpMode) {
        PHYSICAL_ADDRESS CruClkSel;

        CruClkSel.QuadPart = RK3576_CRU_CLKSEL_CON105;
        MshcExtension->RkCruClkSel = (volatile ULONG *)MmMapIoSpaceEx(
            CruClkSel,
            sizeof(ULONG),
            PAGE_READWRITE | PAGE_NOCACHE);
    }

    if (MshcExtension->RkCruClkSel == NULL) {
        MSHC_LOG_WARN(
            MshcExtension->LogHandle,
            MshcExtension,
            "CRU not mapped, clock left unchanged");
        return STATUS_NOT_SUPPORTED;
    }

    //
    // The controller sees the CRU rate divided by two. Pick the parent and
    // divider that come closest to the target without exceeding it.
    //
    TargetHz = FrequencyKhz * 1000 * CRU_SD_CLKGEN_DIV;
    BestHz = 0;
    BestMux = 2;
    BestDiv = RK3576_SDMMC_DIV_MAX;

    for (Mux = 0; Mux < ARRAYSIZE(Rk3576SdmmcParentHz); Mux++) {
        ULONG ParentHz = Rk3576SdmmcParentHz[Mux];
        ULONG Div = (ParentHz + TargetHz - 1) / TargetHz;
        ULONG Hz;

        if (Div < 1) {
            Div = 1;
        }
        if (Div > RK3576_SDMMC_DIV_MAX) {
            Div = RK3576_SDMMC_DIV_MAX;
        }

        Hz = ParentHz / Div;
        if ((Hz <= TargetHz) && (Hz > BestHz)) {
            BestHz = Hz;
            BestMux = Mux;
            BestDiv = Div;
        }
    }

    if (BestHz == 0) {
        //
        // Below what the crystal reaches with the largest divider: go as low
        // as the hardware allows.
        //
        BestHz = Rk3576SdmmcParentHz[BestMux] / BestDiv;
    }

    WRITE_REGISTER_ULONG(
        (volatile ULONG *)MshcExtension->RkCruClkSel,
        RK3576_SDMMC_WRITE_MASK |
        (BestMux << RK3576_SDMMC_MUX_SHIFT) |
        ((BestDiv - 1) << RK3576_SDMMC_DIV_SHIFT));

    MshcExtension->RkCardClockHz = BestHz / CRU_SD_CLKGEN_DIV;

    //
    // Never faster than asked, so the controller's own divider stays at
    // bypass, as it does on RK3588 after a successful SiP call.
    //
    MshcExtension->Capabilities.BaseClockFrequencyKhz = FrequencyKhz;

    MSHC_LOG_INFO(
        MshcExtension->LogHandle,
        MshcExtension,
        "Card clock %u kHz asked, %u Hz set (mux %u, div %u)",
        FrequencyKhz,
        MshcExtension->RkCardClockHz,
        BestMux,
        BestDiv);

    if (FrequencyKhz <= 400) {
        (VOID)MshcRk3576SetPhase(MshcExtension, TRUE, SAMPLE_PHASE_DEFAULT);
    }

    (VOID)MshcRk3576SetPhase(MshcExtension, FALSE, (FrequencyKhz >= 100000) ? 180 : 90);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
MshcRockchipSetVoltage(
    PMSHC_EXTENSION MshcExtension,
    SDPORT_BUS_VOLTAGE Voltage
    )
{
    UNREFERENCED_PARAMETER(MshcExtension);
    UNREFERENCED_PARAMETER(Voltage);

    //
    // vmmc is vcc_3v3_s3 on both RK3576 boards: fixed and always on. There is
    // no external supply the OS can switch, so report card power control as
    // unsupported. The controller's own PWREN is handled by the caller.
    //
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS
MshcRockchipSetSignalingVoltage(
    PMSHC_EXTENSION MshcExtension,
    SDPORT_SIGNALING_VOLTAGE Voltage
    )
{
    UNREFERENCED_PARAMETER(MshcExtension);

    //
    // vqmmc (vccio_sd_s0, an RK806 LDO) stays at 3.3 V: nothing reachable from
    // here can reprogram the PMIC. The firmware publishes "no-1-8-v" so that
    // 1.8 V is never offered; refuse it outright if it is asked for anyway.
    //
    return (Voltage == SdSignalingVoltage33) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

VOID
MshcRockchipCleanup(
    _In_ PMSHC_EXTENSION MshcExtension
    )
{
    if (MshcExtension->RkCruClkSel != NULL) {
        MmUnmapIoSpace((PVOID)MshcExtension->RkCruClkSel, sizeof(ULONG));
        MshcExtension->RkCruClkSel = NULL;
    }
}

NTSTATUS
MshcRockchipExecuteTuning(
    _In_ PMSHC_EXTENSION MshcExtension
    )
{
    NTSTATUS Status;
    ULONG Index;
    BOOLEAN CurrentTuneCmdOk;
    BOOLEAN PreviousTuneCmdOk;
    BOOLEAN FirstTuneCmdOk;
    ULONG RangeCount;
    LONG LongestRangeLength;
    LONG LongestRange;
    LONG MiddlePhase;

    struct {
        LONG Start;
        LONG End;
    } Ranges[NUM_PHASES / 2 + 1] = { 0 };

    PreviousTuneCmdOk = FALSE;
    FirstTuneCmdOk = FALSE;
    RangeCount = 0;
    LongestRangeLength = -1;
    LongestRange = -1;

    //
    // Try each phase and extract good ranges.
    //
    for (Index = 0; Index < NUM_PHASES;) {
        Status = MshcRk3576SetPhase(
            MshcExtension,
            TRUE,
            RANGE_INDEX_TO_PHASE(Index, NUM_PHASES));

        if (!NT_SUCCESS(Status)) {
            MSHC_LOG_WARN(
                MshcExtension->LogHandle,
                MshcExtension,
                "Failed to set sample phase to %d. Status=%!STATUS!",
                RANGE_INDEX_TO_PHASE(Index, NUM_PHASES),
                Status);
        } else {
            Status = MshcSendTuningCommand(MshcExtension);
        }
        CurrentTuneCmdOk = NT_SUCCESS(Status);

        if (Index == 0) {
            FirstTuneCmdOk = CurrentTuneCmdOk;
        }

        if (CurrentTuneCmdOk) {
            if (!PreviousTuneCmdOk) {
                RangeCount++;
                Ranges[RangeCount - 1].Start = Index;
            }
            Ranges[RangeCount - 1].End = Index;
            Index++;
        } else if (Index == NUM_PHASES - 1) {
            // No extra skipping rules if we're at the end.
            Index++;
        } else {
            //
            // No need to check too close to an invalid one since testing
            // bad phases is slow. Skip a few degrees.
            //
            Index += PHASE_TO_RANGE_INDEX(BAD_PHASES_TO_SKIP, NUM_PHASES);

            // Don't skip too far.
            if (Index >= NUM_PHASES) {
                Index = NUM_PHASES - 1;
            }
        }

        PreviousTuneCmdOk = CurrentTuneCmdOk;
    }

    if (RangeCount == 0) {
        MSHC_LOG_ERROR(
            MshcExtension->LogHandle,
            MshcExtension,
            "All phases bad!");

        return STATUS_IO_DEVICE_ERROR;
    }

    // Wrap around case (e.g. 340 - 12 degrees), merge the end points.
    if ((RangeCount > 1) && FirstTuneCmdOk && CurrentTuneCmdOk) {
        Ranges[0].Start = Ranges[RangeCount - 1].Start;
        RangeCount--;
    }

    if ((Ranges[0].Start == 0) && (Ranges[0].End == NUM_PHASES - 1)) {
        MSHC_LOG_INFO(
            MshcExtension->LogHandle,
            MshcExtension,
            "All phases good - using default phase %d",
            SAMPLE_PHASE_DEFAULT);

        Status = MshcRk3576SetPhase(
            MshcExtension,
            TRUE,
            SAMPLE_PHASE_DEFAULT);

        if (!NT_SUCCESS(Status)) {
            MSHC_LOG_WARN(
                MshcExtension->LogHandle,
                MshcExtension,
                "Failed to set sample phase to %d. Status=%!STATUS!",
                SAMPLE_PHASE_DEFAULT,
                Status);
        }

        return STATUS_SUCCESS;
    }

    //
    // Find the longest range.
    //
    for (Index = 0; Index < RangeCount; Index++) {
        LONG Length = (Ranges[Index].End - Ranges[Index].Start + 1);

        if (Length < 0) {
            Length += NUM_PHASES;
        }

        if (LongestRangeLength < Length) {
            LongestRangeLength = Length;
            LongestRange = Index;
        }

        MSHC_LOG_INFO(
            MshcExtension->LogHandle,
            MshcExtension,
            "Good phase range %d-%d (Length=%d)",
            RANGE_INDEX_TO_PHASE(Ranges[Index].Start, NUM_PHASES),
            RANGE_INDEX_TO_PHASE(Ranges[Index].End, NUM_PHASES),
            Length);
    }

    MSHC_LOG_INFO(
        MshcExtension->LogHandle,
        MshcExtension,
        "Best phase range %d-%d (Length=%d)",
        RANGE_INDEX_TO_PHASE(Ranges[LongestRange].Start, NUM_PHASES),
        RANGE_INDEX_TO_PHASE(Ranges[LongestRange].End, NUM_PHASES),
        LongestRangeLength);

    // Sampling point should be in the middle of a good range.
    MiddlePhase = Ranges[LongestRange].Start + LongestRangeLength / 2;
    MiddlePhase %= NUM_PHASES;

    MSHC_LOG_INFO(
        MshcExtension->LogHandle,
        MshcExtension,
        "Successfully tuned phase to %d",
        RANGE_INDEX_TO_PHASE(MiddlePhase, NUM_PHASES));

    Status = MshcRk3576SetPhase(
        MshcExtension,
        TRUE,
        RANGE_INDEX_TO_PHASE(MiddlePhase, NUM_PHASES));

    if (!NT_SUCCESS(Status)) {
        MSHC_LOG_WARN(
            MshcExtension->LogHandle,
            MshcExtension,
            "Failed to set sample phase to %d. Status=%!STATUS!",
            RANGE_INDEX_TO_PHASE(MiddlePhase, NUM_PHASES),
            Status);
    }

    return STATUS_SUCCESS;
}
