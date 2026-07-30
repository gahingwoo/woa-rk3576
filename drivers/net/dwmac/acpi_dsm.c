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

Environment:

    Kernel mode.

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
#define DWMAC_DSM_FUNC_QUERY        0
#define DWMAC_DSM_FUNC_SET_TX_CLK   1

//
// _DSM takes four arguments: GUID buffer, revision, function index, and a
// package of function-specific arguments.
//
typedef struct _DWMAC_DSM_INPUT {
    ACPI_EVAL_INPUT_BUFFER_COMPLEX_EX Header;
    UCHAR                             ArgumentSpace[
                                          sizeof(ACPI_METHOD_ARGUMENT_V1) * 3 +
                                          sizeof(GUID) + 32];
} DWMAC_DSM_INPUT;

NTSTATUS
DwmacSetTxClockDsm(
    _In_ PDWMAC_ADAPTER A,
    _In_ ULONG SpeedMbps
    )
{
    DWMAC_DSM_INPUT input;
    ACPI_EVAL_OUTPUT_BUFFER output;
    PACPI_METHOD_ARGUMENT arg;
    PDEVICE_OBJECT pdo;
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    ULONG inputSize;

    pdo = WdfDeviceWdmGetPhysicalDevice(A->Device);
    if (pdo == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }

    RtlZeroMemory(&input, sizeof(input));
    RtlZeroMemory(&output, sizeof(output));

    input.Header.Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE_EX;
    RtlCopyMemory(input.Header.MethodName, "_DSM", 4);
    input.Header.ArgumentCount = 4;

    arg = input.Header.Argument;

    //
    // Arg0: the GUID, as a raw buffer.
    //
    ACPI_METHOD_SET_ARGUMENT_BUFFER(arg, &DwmacTxClockDsmGuid, sizeof(GUID));
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);

    //
    // Arg1: revision. Arg2: function index.
    //
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, DWMAC_DSM_REVISION);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);

    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, DWMAC_DSM_FUNC_SET_TX_CLK);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);

    //
    // Arg3: the speed. Firmware reads it as Arg3[0], so a bare integer here is
    // dereferenced as the first element of the package.
    //
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, SpeedMbps);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);

    inputSize = (ULONG)((PUCHAR)arg - (PUCHAR)&input);
    input.Header.Size = inputSize;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD_EX,
                                        pdo,
                                        &input,
                                        inputSize,
                                        &output,
                                        sizeof(output),
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (!NT_SUCCESS(status)) {
        RkLog(RK_DBG_ERROR,
              "TX-clock _DSM for %lu Mbit/s failed: 0x%08X\n",
              SpeedMbps, status);
    }

    return status;
}
