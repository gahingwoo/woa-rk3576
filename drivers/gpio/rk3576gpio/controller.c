/*++

Module Name:

    controller.c

Abstract:

    GpioClx CLIENT_* callbacks for the RK3576 "GPIO v2" controller. One driver
    instance services one 32-pin bank (one ACPI\RKCP3002 device). GpioClx owns
    PnP/power/interrupt connection; this module only touches the bank registers.

    IRQL notes:
      - PrepareController / ReleaseController / Start / Stop / Query* run at
        PASSIVE_LEVEL.
      - Connect/Disconnect/Read/Write run at <= DISPATCH_LEVEL.
      - Enable/Disable/Mask/Unmask/QueryActive/ClearActive/Reconfigure run at
        the controller's interrupt IRQL (DIRQL).
    All accessors here are non-paged MMIO and safe at DIRQL. GpioClx serializes
    callbacks per controller, so register read-modify-write is race-free.

Environment:

    Kernel mode.

--*/

#include "rk3576gpio.h"

//
// ----------------------------------------------------------------------------
// Controller lifetime
// ----------------------------------------------------------------------------
//

_Use_decl_annotations_
NTSTATUS
RkGpioPrepareController(
    WDFDEVICE Device,
    PVOID Context,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG count;
    BOOLEAN memoryFound = FALSE;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);

    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->BankNumber = (ULONG)-1;

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (ULONG i = 0; i < count; i += 1) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc == NULL) {
            continue;
        }

        if (desc->Type == CmResourceTypeMemory && !memoryFound) {
            ctx->RegsPhysical = desc->u.Memory.Start;
            ctx->RegsLength = desc->u.Memory.Length;
            ctx->Regs = (volatile UCHAR *)MmMapIoSpaceEx(desc->u.Memory.Start,
                                                         desc->u.Memory.Length,
                                                         PAGE_READWRITE | PAGE_NOCACHE);
            if (ctx->Regs == NULL) {
                RkLog(RK_DBG_ERROR, "MmMapIoSpaceEx failed for 0x%llx len 0x%x\n",
                      desc->u.Memory.Start.QuadPart, desc->u.Memory.Length);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            memoryFound = TRUE;
        }
    }

    if (!memoryFound) {
        RkLog(RK_DBG_ERROR, "no memory resource in _CRS\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // Decode the bank index from the base address (diagnostic only). The driver
    // is otherwise base-address agnostic and works for any RKCP3002 bank.
    //
    switch ((ULONG)ctx->RegsPhysical.QuadPart) {
    case 0x27320000: ctx->BankNumber = 0; break;
    case 0x2AE10000: ctx->BankNumber = 1; break;
    case 0x2AE20000: ctx->BankNumber = 2; break;
    case 0x2AE30000: ctx->BankNumber = 3; break;
    case 0x2AE40000: ctx->BankNumber = 4; break;
    default: break;
    }

    //
    // Start from a known, quiet state: every interrupt disabled and masked,
    // any latched edge cleared.
    //
    RkGpioWrite32(ctx->Regs, RK_GPIO_INT_EN, 0x00000000);
    RkGpioWrite32(ctx->Regs, RK_GPIO_INT_MASK, 0xFFFFFFFF);
    RkGpioWrite32(ctx->Regs, RK_GPIO_PORT_EOI, 0xFFFFFFFF);

    RkLog(RK_DBG_INFO, "PrepareController bank %u @ 0x%llx (ver 0x%08x)\n",
          ctx->BankNumber, ctx->RegsPhysical.QuadPart,
          RkGpioRead32(ctx->Regs, RK_GPIO_VERSION_ID));

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioReleaseController(
    WDFDEVICE Device,
    PVOID Context
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Device);

    if (ctx->Regs != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Regs, ctx->RegsLength);
        ctx->Regs = NULL;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioStartController(
    PVOID Context,
    BOOLEAN RestoreContext,
    WDF_POWER_DEVICE_STATE PreviousPowerState
    )
{
    //
    // GPIO PCLK/DBCLK are left enabled by firmware (the EDK2 RK3576 port brings
    // up the GPIO clocks). Windows-on-ARM has no generic clock framework, so
    // there is nothing to gate here. Interrupt state is re-established by
    // GpioClx replaying EnableInterrupt after a power transition.
    //
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(RestoreContext);
    UNREFERENCED_PARAMETER(PreviousPowerState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioStopController(
    PVOID Context,
    BOOLEAN SaveContext,
    WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(SaveContext);
    UNREFERENCED_PARAMETER(TargetState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioQueryControllerBasicInformation(
    PVOID Context,
    PCLIENT_CONTROLLER_BASIC_INFORMATION ControllerInformation
    )
{
    UNREFERENCED_PARAMETER(Context);

    ControllerInformation->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    ControllerInformation->Size = sizeof(CLIENT_CONTROLLER_BASIC_INFORMATION);
    ControllerInformation->TotalPins = RK3576GPIO_PINS_PER_BANK;
    ControllerInformation->NumberOfPinsPerBank = RK3576GPIO_PINS_PER_BANK;

    ControllerInformation->Flags.MemoryMappedController = TRUE;
    ControllerInformation->Flags.FormatIoRequestsAsMasks = TRUE;
    ControllerInformation->Flags.ActiveInterruptsAutoClearOnRead = FALSE;

    //
    // The v2 controller has native both-edge and debounce support, so GpioClx
    // need not emulate either.
    //
    ControllerInformation->Flags.EmulateActiveBoth = FALSE;
    ControllerInformation->Flags.EmulateDebounce = FALSE;

    ControllerInformation->Flags.DeviceIdlePowerMgmtSupported = FALSE;
    ControllerInformation->Flags.BankIdlePowerMgmtSupported = FALSE;

    return STATUS_SUCCESS;
}

//
// ----------------------------------------------------------------------------
// I/O pins
// ----------------------------------------------------------------------------
//

_Use_decl_annotations_
NTSTATUS
RkGpioConnectIoPins(
    PVOID Context,
    PGPIO_CONNECT_IO_PINS_PARAMETERS ConnectParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    PGPIO_PIN_NUMBER pins = ConnectParameters->PinNumberTable;
    ULONG pinCount = ConnectParameters->PinCount;
    BOOLEAN output = (ConnectParameters->ConnectMode == ConnectModeOutput);

    //
    // NOTE: pin mux (IOMUX to GPIO function) and pull-up/down live in the GRF /
    // pinctrl block (0x26040000), not in the GPIO bank window. v1 assumes the
    // firmware has already muxed the referenced pins to GPIO. A future pinctrl
    // driver (or _DSD pull hints) will own that; see docs/ARCHITECTURE.md.
    //
    for (ULONG i = 0; i < pinCount; i += 1) {
        ULONG pin = pins[i];
        if (pin >= RK3576GPIO_PINS_PER_BANK) {
            continue;
        }
        RkGpioWriteBit(ctx->Regs, RK_GPIO_SWPORT_DDR, pin, output);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioDisconnectIoPins(
    PVOID Context,
    PGPIO_DISCONNECT_IO_PINS_PARAMETERS DisconnectParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    PGPIO_PIN_NUMBER pins = DisconnectParameters->PinNumberTable;
    ULONG pinCount = DisconnectParameters->PinCount;

    if (DisconnectParameters->DisconnectFlags.PreserveConfiguration) {
        return STATUS_SUCCESS;
    }

    //
    // Revert disconnected pins to input (the safe default).
    //
    for (ULONG i = 0; i < pinCount; i += 1) {
        ULONG pin = pins[i];
        if (pin >= RK3576GPIO_PINS_PER_BANK) {
            continue;
        }
        RkGpioWriteBit(ctx->Regs, RK_GPIO_SWPORT_DDR, pin, FALSE);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioReadPinsUsingMask(
    PVOID Context,
    PGPIO_READ_PINS_MASK_PARAMETERS ReadParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG value;

    //
    // When reading the configured-output value GpioClx sets WriteConfiguration;
    // otherwise we sample the live pin level from EXT_PORT.
    //
    if (ReadParameters->Flags.WriteConfiguration) {
        value = RkGpioRead32(ctx->Regs, RK_GPIO_SWPORT_DR);
    } else {
        value = RkGpioRead32(ctx->Regs, RK_GPIO_EXT_PORT);
    }

    *ReadParameters->PinValues = (ULONG64)value;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioWritePinsUsingMask(
    PVOID Context,
    PGPIO_WRITE_PINS_MASK_PARAMETERS WriteParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG setMask = (ULONG)WriteParameters->SetMask;
    ULONG clearMask = (ULONG)WriteParameters->ClearMask;

    RkGpioApplyMask32(ctx->Regs, RK_GPIO_SWPORT_DR, setMask, clearMask);
    return STATUS_SUCCESS;
}

//
// ----------------------------------------------------------------------------
// Interrupts (called at the controller's DIRQL)
// ----------------------------------------------------------------------------
//

static
VOID
RkGpioProgramTrigger(
    _In_ PRK3576GPIO_CONTEXT Ctx,
    _In_ ULONG Pin,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity
    )
{
    BOOLEAN edge = (Mode == Latched);

    RkGpioWriteBit(Ctx->Regs, RK_GPIO_INT_TYPE, Pin, edge);

    if (edge && Polarity == InterruptActiveBoth) {
        RkGpioWriteBit(Ctx->Regs, RK_GPIO_INT_BOTHEDGE, Pin, TRUE);
    } else {
        RkGpioWriteBit(Ctx->Regs, RK_GPIO_INT_BOTHEDGE, Pin, FALSE);
        //
        // POLARITY: 1 = high level / rising edge, 0 = low level / falling edge.
        //
        RkGpioWriteBit(Ctx->Regs, RK_GPIO_INT_POLARITY, Pin,
                       (BOOLEAN)(Polarity == InterruptActiveHigh));
    }
}

_Use_decl_annotations_
NTSTATUS
RkGpioEnableInterrupt(
    PVOID Context,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS EnableParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG pin = EnableParameters->PinNumber;

    if (pin >= RK3576GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // An interrupt source must be an input pin.
    //
    RkGpioWriteBit(ctx->Regs, RK_GPIO_SWPORT_DDR, pin, FALSE);

    RkGpioProgramTrigger(ctx, pin,
                         EnableParameters->InterruptMode,
                         EnableParameters->Polarity);

    //
    // Hardware debounce. We reported EmulateDebounce = FALSE, so honour a
    // non-zero requested timeout natively: program the (coarse) divider once and
    // enable debounce for this pin. DBCLK_DIV_CON is a plain count register, not
    // the per-pin write-enable form.
    //
    if (EnableParameters->DebounceTimeout != 0) {
        WRITE_REGISTER_ULONG((volatile ULONG *)(ctx->Regs + RK_GPIO_DBCLK_DIV_CON),
                             RK_GPIO_DBCLK_DIV_DEFAULT);
        RkGpioWriteBit(ctx->Regs, RK_GPIO_DBCLK_DIV_EN, pin, TRUE);
        RkGpioWriteBit(ctx->Regs, RK_GPIO_DEBOUNCE, pin, TRUE);
    } else {
        RkGpioWriteBit(ctx->Regs, RK_GPIO_DEBOUNCE, pin, FALSE);
        RkGpioWriteBit(ctx->Regs, RK_GPIO_DBCLK_DIV_EN, pin, FALSE);
    }

    //
    // Drop any stale latched edge, enable the source, and unmask so it can be
    // delivered. GpioClx uses Mask/Unmask for transient masking afterwards.
    //
    RkGpioWriteBit(ctx->Regs, RK_GPIO_PORT_EOI, pin, TRUE);
    RkGpioWriteBit(ctx->Regs, RK_GPIO_INT_EN, pin, TRUE);
    RkGpioWriteBit(ctx->Regs, RK_GPIO_INT_MASK, pin, FALSE);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioDisableInterrupt(
    PVOID Context,
    PGPIO_DISABLE_INTERRUPT_PARAMETERS DisableParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG pin = DisableParameters->PinNumber;

    if (pin >= RK3576GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }

    RkGpioWriteBit(ctx->Regs, RK_GPIO_INT_MASK, pin, TRUE);
    RkGpioWriteBit(ctx->Regs, RK_GPIO_INT_EN, pin, FALSE);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioMaskInterrupts(
    PVOID Context,
    PGPIO_MASK_INTERRUPT_PARAMETERS MaskParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG mask = (ULONG)MaskParameters->PinMask;

    //
    // Set the mask bits for the requested pins; leave the rest untouched.
    //
    RkGpioApplyMask32(ctx->Regs, RK_GPIO_INT_MASK, mask, 0);
    MaskParameters->FailedMask = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioUnmaskInterrupt(
    PVOID Context,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS UnmaskParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG pin = UnmaskParameters->PinNumber;

    if (pin >= RK3576GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }

    RkGpioWriteBit(ctx->Regs, RK_GPIO_INT_MASK, pin, FALSE);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioQueryActiveInterrupts(
    PVOID Context,
    PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS QueryActiveParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;

    //
    // INT_STATUS already reflects the mask, so it is exactly the set of pending
    // interrupts GpioClx should dispatch.
    //
    QueryActiveParameters->ActiveMask =
        (ULONG64)RkGpioRead32(ctx->Regs, RK_GPIO_INT_STATUS);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioClearActiveInterrupts(
    PVOID Context,
    PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS ClearParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG mask = (ULONG)ClearParameters->ClearActiveMask;

    //
    // EOI clears latched edge interrupts. For level interrupts the bit clears
    // only when the source de-asserts; writing EOI for them is a harmless no-op.
    //
    RkGpioWrite32(ctx->Regs, RK_GPIO_PORT_EOI, mask);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioReconfigureInterrupt(
    PVOID Context,
    PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS ReconfigureParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;
    ULONG pin = ReconfigureParameters->PinNumber;

    if (pin >= RK3576GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }

    RkGpioProgramTrigger(ctx, pin,
                         ReconfigureParameters->InterruptMode,
                         ReconfigureParameters->Polarity);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
RkGpioQueryEnabledInterrupts(
    PVOID Context,
    PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS QueryEnabledParameters
    )
{
    PRK3576GPIO_CONTEXT ctx = (PRK3576GPIO_CONTEXT)Context;

    QueryEnabledParameters->EnabledMask =
        (ULONG64)RkGpioRead32(ctx->Regs, RK_GPIO_INT_EN);

    return STATUS_SUCCESS;
}
