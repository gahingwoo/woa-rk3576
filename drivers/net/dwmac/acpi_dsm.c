/*++

Module Name:

    acpi_dsm.c

Abstract:

    Evaluates the GMAC TX-clock _DSM published by the EDK2 RK3576 port.

    The RGMII TX clock source has to be reselected every time the link speed
    changes (125 MHz for 1000, 25 MHz for 100, 2.5 MHz for 10). The register
    that does it lives in SDGMAC_GRF, which is *not* part of the GMAC's ACPI
    _CRS — it belongs to firmware, and firmware exposes it as a _DSM on the MAC
    device instead:

      GUID d637828d-556c-4829-966a-237072f00ff1, revision 0, function 1,
      Arg3[0] = link speed in Mbit/s.

    Going through the _DSM rather than mapping SDGMAC_GRF directly matters for
    three reasons: the window is outside anything ACPI granted this device, two
    independent writers of a HIWORD-masked register is a race waiting to happen,
    and this is the same contract the RK3588 GMAC driver already uses, so one
    firmware serves both.

    The marshalling below follows the WDK acpiutil sample's
    AcpiFormatDsmFunctionInputBuffer / AcpiEvaluateMethod, which is what the
    RK3588 driver uses and is therefore known to work against a real ACPI
    driver. Two details are load-bearing and were both wrong in the first cut of
    this file:

    - Arg3 must be a *package* (ACPI_METHOD_ARGUMENT_PACKAGE_EX whose Data holds
      the packed sub-arguments), not an integer. The ASL indexes it as Arg3[0];
      an integer cannot be indexed.
    - IOCTL_ACPI_EVAL_METHOD with ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE and
      MethodNameAsUlong is the plain "evaluate a method on this device" form.
      The _EX variants exist for naming an object by path and are not what is
      wanted here.

    The helper is not shared as a library because this driver evaluates exactly
    one _DSM; the sample's full acpiutil is C++ and this driver is C.

Environment:

    Kernel mode. Called at PASSIVE_LEVEL from the link-state path.

--*/

#include "dwmac.h"

#include <initguid.h>
#include <acpiioct.h>

//
// {d637828d-556c-4829-966a-237072f00ff1}
//
static const GUID DwmacTxClockDsmGuid = {
    0xd637828d, 0x556c, 0x4829,
    { 0x96, 0x6a, 0x23, 0x70, 0x72, 0xf0, 0x0f, 0xf1 }
};

#define DWMAC_DSM_REVISION          0
#define DWMAC_DSM_FUNC_SET_TX_CLK   1

//
// _DSM always takes four arguments: GUID buffer, revision, function index,
// and a package of function-specific arguments.
//
#define DWMAC_DSM_ARGUMENT_COUNT    4

//
// MethodNameAsUlong wants the four characters of "_DSM" in memory order.
// acpiioct.h does not provide this constant; the acpiutil sample spells it as
// the multi-character literal 'MSD_', which is the same value written out.
//
#define DWMAC_ACPI_METHOD_DSM \
    ((ULONG)'_' | ((ULONG)'D' << 8) | ((ULONG)'S' << 16) | ((ULONG)'M' << 24))

//
// One integer inside the Arg3 package.
//
typedef struct _DWMAC_DSM_ARG3 {
    ACPI_METHOD_ARGUMENT Speed;
} DWMAC_DSM_ARG3;

//
// The complete input buffer. ACPI_EVAL_INPUT_BUFFER_COMPLEX ends in a
// single-element Argument[] array, so the trailing arguments are laid out by
// hand: the GUID buffer argument grows past sizeof(ACPI_METHOD_ARGUMENT) by
// (sizeof(GUID) - sizeof(ULONG)), and Arg3 likewise by its package contents.
//
typedef struct _DWMAC_DSM_INPUT {
    ACPI_EVAL_INPUT_BUFFER_COMPLEX Header;
    UCHAR                          Trailing[
                                       (sizeof(GUID) - sizeof(ULONG)) +
                                       sizeof(ACPI_METHOD_ARGUMENT) * 2 +
                                       sizeof(ACPI_METHOD_ARGUMENT) +
                                       (sizeof(DWMAC_DSM_ARG3) -
                                        sizeof(ULONG))];
} DWMAC_DSM_INPUT;

static
NTSTATUS
DwmacSendAcpiIoctl(
    _In_ PDEVICE_OBJECT Pdo,
    _In_reads_bytes_(InputSize) PVOID Input,
    _In_ ULONG InputSize,
    _Out_writes_bytes_(OutputSize) PVOID Output,
    _In_ ULONG OutputSize,
    _Out_ PULONG BytesReturned
    )
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS status;

    *BytesReturned = 0;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,
                                        Pdo,
                                        Input,
                                        InputSize,
                                        Output,
                                        OutputSize,
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(Pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    *BytesReturned = (ULONG)iosb.Information;
    return status;
}

NTSTATUS
DwmacSetTxClockDsm(
    _In_ PDWMAC_ADAPTER A,
    _In_ ULONG SpeedMbps
    )
{
    DWMAC_DSM_INPUT input;
    DWMAC_DSM_ARG3 arg3;
    ACPI_EVAL_OUTPUT_BUFFER output;
    PACPI_METHOD_ARGUMENT arg;
    PDEVICE_OBJECT pdo;
    NTSTATUS status;
    ULONG returned;

    pdo = WdfDeviceWdmGetPhysicalDevice(A->Device);
    if (pdo == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }

    RtlZeroMemory(&input, sizeof(input));
    RtlZeroMemory(&arg3, sizeof(arg3));
    RtlZeroMemory(&output, sizeof(output));

    input.Header.Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    input.Header.MethodNameAsUlong = DWMAC_ACPI_METHOD_DSM;
    input.Header.Size = sizeof(input);
    input.Header.ArgumentCount = DWMAC_DSM_ARGUMENT_COUNT;

    //
    // Arg0: the GUID, as a raw buffer.
    //
    arg = input.Header.Argument;
    ACPI_METHOD_SET_ARGUMENT_BUFFER(arg, &DwmacTxClockDsmGuid, sizeof(GUID));

    //
    // Arg1: revision. Arg2: function index.
    //
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, DWMAC_DSM_REVISION);

    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, DWMAC_DSM_FUNC_SET_TX_CLK);

    //
    // Arg3: a package holding the speed, which the ASL reads as Arg3[0].
    //
    ACPI_METHOD_SET_ARGUMENT_INTEGER((&arg3.Speed), SpeedMbps);

    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    arg->Type = ACPI_METHOD_ARGUMENT_PACKAGE_EX;
    arg->DataLength = (USHORT)sizeof(arg3);
    RtlCopyMemory(arg->Data, &arg3, sizeof(arg3));

    NT_ASSERT((PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(arg) ==
              (PUCHAR)&input + sizeof(input));

    //
    // This _DSM returns nothing, so a bare output header is enough; a device
    // that did return data would answer STATUS_BUFFER_OVERFLOW and we would
    // have to allocate. Nothing here needs the value, so do not.
    //
    status = DwmacSendAcpiIoctl(pdo,
                                &input,
                                sizeof(input),
                                &output,
                                sizeof(output),
                                &returned);

    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR,
              "TX-clock _DSM for %lu Mbit/s failed: 0x%08X\n",
              SpeedMbps, status);
    }

    return status;
}
