/*++

Module Name:

    sip.h

Abstract:

    Client for the Rockchip SD/MMC Control Service, a SiP SMC service provided
    by BL31 (TF-A) at EL3.

    A Windows storage miniport has no clock or regulator framework, and the
    RK35xx card clock cannot be produced by the host controller's own divider
    alone — the rate is set in the CRU. EL3 owns the CRU, so the miniport asks
    EL3 instead of touching it.

    Note that clock *phase* is not part of this interface on RK3576: unlike
    RK3568/RK3588, which expose ciu-drive/ciu-sample as CRU phase clocks,
    RK3576 keeps the phase in the controller's own register window
    (TIMING_CON0/TIMING_CON1), which this driver programs directly. See
    DwmmcSetPhase in hw.c.

Environment:

    Kernel mode.

--*/

#pragma once

//
// Clock ids for a dw-mshc controller (sdmmc0, sdio).
//
#define RK_SIP_SDMMC_CLOCK_ID_MSHC_CIU          0
#define RK_SIP_SDMMC_CLOCK_ID_MSHC_CIU_DRIVE    1
#define RK_SIP_SDMMC_CLOCK_ID_MSHC_CIU_SAMPLE   2

//
// Clock id for the dwcmshc controller (emmc).
//
#define RK_SIP_SDMMC_CLOCK_ID_EMMC_CCLK         0

//
// Regulator ids: SUPPLY is vmmc (card power), SIGNAL is vqmmc (I/O level).
//
#define RK_SIP_SDMMC_REGULATOR_ID_SUPPLY        0
#define RK_SIP_SDMMC_REGULATOR_ID_SIGNAL        1

NTSTATUS
RkSipSdmmcClockRateGet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _Out_ ULONG* RateHz
    );

NTSTATUS
RkSipSdmmcClockRateSet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ ULONG RateHz
    );

NTSTATUS
RkSipSdmmcRegulatorVoltageGet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _Out_ ULONG* Microvolts
    );

NTSTATUS
RkSipSdmmcRegulatorVoltageSet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ ULONG Microvolts
    );

NTSTATUS
RkSipSdmmcRegulatorEnableSet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ BOOLEAN Enable
    );
