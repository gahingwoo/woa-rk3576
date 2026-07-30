/*++

Module Name:

    sip.c

Abstract:

    Rockchip SD/MMC Control Service client — see sip.h.

    The service ids below are the SMC32 encodings. A caller built for AArch64
    must raise bit 30 (the SMCCC SMC64 bit); the RK3576 BL31 handler accepts
    both encodings, but sending the wrong one to a different implementation
    would silently land in SMC_UNK, so it is set explicitly here.

Environment:

    Kernel mode.

--*/

#include "rkdwmmc.h"
#include "sip.h"

#define RK_SIP_SDMMC_CLOCK_RATE_GET         0x82000020
#define RK_SIP_SDMMC_CLOCK_RATE_SET         0x82000021
#define RK_SIP_SDMMC_REGULATOR_VOLTAGE_GET  0x82000024
#define RK_SIP_SDMMC_REGULATOR_VOLTAGE_SET  0x82000025
#define RK_SIP_SDMMC_REGULATOR_ENABLE_GET   0x82000026
#define RK_SIP_SDMMC_REGULATOR_ENABLE_SET   0x82000027

#define SMC64(_Id)  ((_Id) | (1u << 30))

//
// Status codes returned in x0 by the service. -1 is SMC_UNK, so "this SoC has
// no such service at all" and "this call is not implemented" are the same
// answer, deliberately.
//
#define RK_SIP_SDMMC_E_SUCCESS          0
#define RK_SIP_SDMMC_E_NOT_SUPPORTED    (-1)
#define RK_SIP_SDMMC_E_INVALID_PARAM    (-2)
#define RK_SIP_SDMMC_E_NOT_IMPLEMENTED  (-3)

typedef struct _RK_SMC_ARGS {
    ULONG_PTR Arg0;
    ULONG_PTR Arg1;
    ULONG_PTR Arg2;
    ULONG_PTR Arg3;
    ULONG_PTR Arg4;
    ULONG_PTR Arg5;
    ULONG_PTR Arg6;
    ULONG_PTR Arg7;
} RK_SMC_ARGS;

VOID
RkArmCallSmc(
    _Inout_ RK_SMC_ARGS* Args
    );

static
NTSTATUS
SmcStatusToNtStatus(
    _In_ ULONG_PTR Ret
    )
{
    switch ((LONG_PTR)Ret) {
    case RK_SIP_SDMMC_E_SUCCESS:
        return STATUS_SUCCESS;
    case RK_SIP_SDMMC_E_NOT_SUPPORTED:
        return STATUS_NOT_SUPPORTED;
    case RK_SIP_SDMMC_E_INVALID_PARAM:
        return STATUS_INVALID_PARAMETER;
    case RK_SIP_SDMMC_E_NOT_IMPLEMENTED:
        return STATUS_NOT_IMPLEMENTED;
    default:
        return STATUS_UNSUCCESSFUL;
    }
}

static
NTSTATUS
SipGet(
    _In_ ULONG FunctionId,
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _Out_opt_ ULONG* Value
    )
{
    RK_SMC_ARGS args = { 0 };

    args.Arg0 = SMC64(FunctionId);
    args.Arg1 = ControllerAddress;
    args.Arg2 = Id;

    RkArmCallSmc(&args);

    if (Value != NULL) {
        *Value = (ULONG)args.Arg1;
    }

    return SmcStatusToNtStatus(args.Arg0);
}

static
NTSTATUS
SipSet(
    _In_ ULONG FunctionId,
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ ULONG_PTR Value
    )
{
    RK_SMC_ARGS args = { 0 };

    args.Arg0 = SMC64(FunctionId);
    args.Arg1 = ControllerAddress;
    args.Arg2 = Id;
    args.Arg3 = Value;

    RkArmCallSmc(&args);

    return SmcStatusToNtStatus(args.Arg0);
}

NTSTATUS
RkSipSdmmcClockRateGet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _Out_ ULONG* RateHz
    )
{
    return SipGet(RK_SIP_SDMMC_CLOCK_RATE_GET, ControllerAddress, Id, RateHz);
}

NTSTATUS
RkSipSdmmcClockRateSet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ ULONG RateHz
    )
{
    return SipSet(RK_SIP_SDMMC_CLOCK_RATE_SET, ControllerAddress, Id, RateHz);
}

NTSTATUS
RkSipSdmmcRegulatorVoltageGet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _Out_ ULONG* Microvolts
    )
{
    return SipGet(RK_SIP_SDMMC_REGULATOR_VOLTAGE_GET, ControllerAddress, Id,
                  Microvolts);
}

NTSTATUS
RkSipSdmmcRegulatorVoltageSet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ ULONG Microvolts
    )
{
    return SipSet(RK_SIP_SDMMC_REGULATOR_VOLTAGE_SET, ControllerAddress, Id,
                  Microvolts);
}

NTSTATUS
RkSipSdmmcRegulatorEnableSet(
    _In_ ULONG_PTR ControllerAddress,
    _In_ UCHAR Id,
    _In_ BOOLEAN Enable
    )
{
    return SipSet(RK_SIP_SDMMC_REGULATOR_ENABLE_SET, ControllerAddress, Id,
                  Enable ? 1u : 0u);
}
