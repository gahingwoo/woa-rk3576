/*
 * wdthang.sys - hang the machine on purpose, to test the hardware watchdog
 *
 * A legacy kernel driver with no device: loading it stops every CPU for
 * good, with interrupts masked, so nothing in Windows can run, and the
 * watchdog (edk2-rk3576 setup "Hardware watchdog", the WDAT) is never
 * restarted again. With the watchdog on, the board must reset by itself
 * within about 3 minutes; with it off, it stays hung until power is cut.
 *
 * It does nothing unless loaded by hand (README.md):
 *
 *     sc create wdthang type= kernel binPath= C:\woa-tools\wdthang.sys
 *     sc start wdthang
 *
 * KeIpiGenericCall runs the callback on every processor at IPI level, all
 * at once, and returns only when each one has returned; here none does.
 * The callback reads a volatile flag in a loop so the compiler cannot drop
 * it, and nothing ever clears the flag.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <ntddk.h>

DRIVER_INITIALIZE DriverEntry;

static volatile LONG g_spin = 1;

static
ULONG_PTR
WdtHangSpin(
    _In_ ULONG_PTR Argument
    )
{
    UNREFERENCED_PARAMETER(Argument);

    while (g_spin != 0) {
        YieldProcessor();
    }
    return 0;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "wdthang: stopping all %u processors now; only the hardware watchdog can reset this board\n",
               KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS));

    (void)KeIpiGenericCall(WdtHangSpin, 0);

    /* Not reached. */
    return STATUS_UNSUCCESSFUL;
}
