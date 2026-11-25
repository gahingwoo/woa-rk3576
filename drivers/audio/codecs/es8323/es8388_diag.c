/*
 * Register dump for the ES8388 on an RK3576 SAI (RKCP8388 only).
 *
 * Every 2 s while the codec is in D0 (for at most 10 minutes after each
 * power-up), the driver writes into its "Device Parameters" key,
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP8388\0\Device Parameters:
 *
 *   Es8388Regs      REG_BINARY  codec registers 0x00..0x34, one byte each
 *                               (0xEE where the I2C read failed)
 *   Es8388ReadFail  REG_DWORD   number of failed register reads in that dump
 *   InitStatus      REG_DWORD   NTSTATUS of the last power-up sequence
 *   InitFailedReg   REG_DWORD   register whose write failed (0xFFFFFFFF none)
 *   AmpPresent      REG_DWORD   1 if ACPI gave an amplifier GpioIo
 *   AmpOpenStatus   REG_DWORD   NTSTATUS of opening it
 *   AmpWriteStatus  REG_DWORD   NTSTATUS of the last write, AmpWriteValue
 *   DumpCount       REG_DWORD   increments with every dump
 *   ActiveOutputRoute, ActiveInputSelect
 *                   REG_DWORD   the routing in use (es8388_sai.c)
 *   JackInterrupt   REG_DWORD   1 if _CRS had a GpioInt (jack detect)
 *   Soc_<address>   REG_DWORD   the SoC registers listed below, read through
 *                               a mapping of their page
 *
 * The SoC registers are read only, and only ones whose reads have no side
 * effect: CRU clock selects and gates, the IOC MCLK-out gate and GPIO4
 * iomux/pull, GPIO4 data and direction, PD_AUDIO status, the SAI1 block and
 * the PL330 DMA0 status of SAI1's channels.
 *
 * Diagnostics, to be removed once audio works.
 *
 * Copyright (c) 2026, Jiaxing Hu <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#include "driver.h"
#include <ntstrsafe.h>

#define DIAG_PERIOD_MS      2000
#define DIAG_MAX_DUMPS      300         // 10 minutes
#define ES8388_LAST_REG     0x34

static const ULONG DiagSocRegs[] = {
	// CRU: clk_audio_frac_1, mclk_sai1, gates
	0x27200338, 0x2720033C, 0x272003B8,
	0x27200804, 0x2720081C, 0x27200820, 0x27200824, 0x27200830, 0x27200848, 0x2720084C,
	// IOC: VCCIO_IOC_MISC_CON0 (mclk_sai1_to_io), GPIO4 PA/PB iomux, GPIO4 PA/PB pull
	0x26046400, 0x26044080, 0x26044084, 0x26044088, 0x2604408C,
	// PMU: PD_AUDIO repair status, bus idle status
	0x27380570, 0x27380128,
	// GPIO4: DR_L, DR_H, DDR_L, DDR_H, EXT_PORT
	0x2AE40000, 0x2AE40004, 0x2AE40008, 0x2AE4000C, 0x2AE40070,
	// SAI1: TXCR FSCR RXCR MONO XFER CLR CKR TXFIFOLR RXFIFOLR DMACR INTCR
	//       INTSR PATH_SEL TX_SHIFT RX_SHIFT STATUS VERSION
	0x2A610000, 0x2A610004, 0x2A610008, 0x2A61000C, 0x2A610010, 0x2A610014,
	0x2A610018, 0x2A61001C, 0x2A610020, 0x2A610024, 0x2A610028, 0x2A61002C,
	0x2A610038, 0x2A610064, 0x2A610068, 0x2A61006C, 0x2A610070,
	// DMA0 (PL330, SAI1 TX = channel/request 2, RX = 3): DSR DPC INTEN
	// INT_EVENT_RIS INTMIS FSRD FSRC FTRD FTR2 FTR3 CSR2 CPC2 CSR3 CPC3
	// SA2 DA2 CCR2 LC0_2 SA3 DA3 CR0 CR3 CR4 CRD (status reads only)
	0x2AB90000, 0x2AB90004, 0x2AB90020, 0x2AB90024, 0x2AB90028, 0x2AB90030,
	0x2AB90034, 0x2AB90038, 0x2AB90048, 0x2AB9004C, 0x2AB90110, 0x2AB90114,
	0x2AB90118, 0x2AB9011C, 0x2AB90440, 0x2AB90444, 0x2AB90448, 0x2AB9044C,
	0x2AB90460, 0x2AB90464, 0x2AB90E00, 0x2AB90E0C, 0x2AB90E10, 0x2AB90E14,
};

static void DiagSetDword(WDFKEY key, PCWSTR name, ULONG value) {
	UNICODE_STRING us;
	RtlInitUnicodeString(&us, name);
	WdfRegistryAssignULong(key, &us, value);
}

static ULONG DiagReadSoc(ULONG address) {
	PHYSICAL_ADDRESS pa;
	pa.QuadPart = address & ~(ULONG)(PAGE_SIZE - 1);
	PUCHAR va = (PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
	if (!va) {
		return 0xDEADBEEF;
	}
	ULONG value = READ_REGISTER_ULONG((volatile ULONG*)(va + (address & (PAGE_SIZE - 1))));
	MmUnmapIoSpace(va, PAGE_SIZE);
	return value;
}

static void Es8388DiagDump(_In_ PES8323_CONTEXT pDevice) {
	UCHAR regs[ES8388_LAST_REG + 1];
	ULONG failed = 0;
	WDFKEY key;

	for (UINT8 r = 0; r <= ES8388_LAST_REG; r++) {
		UINT8 v = 0;
		if (NT_SUCCESS(es8323_reg_read(pDevice, r, &v))) {
			regs[r] = v;
		} else {
			regs[r] = 0xEE;
			failed++;
		}
	}

	NTSTATUS status = WdfDeviceOpenRegistryKey(pDevice->FxDevice, PLUGPLAY_REGKEY_DEVICE,
		KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key);
	if (!NT_SUCCESS(status)) {
		DbgPrint("es8323: diag cannot open Device Parameters: 0x%x\n", status);
		return;
	}

	UNICODE_STRING name;
	RtlInitUnicodeString(&name, L"Es8388Regs");
	WdfRegistryAssignValue(key, &name, REG_BINARY, sizeof(regs), regs);
	DiagSetDword(key, L"Es8388ReadFail", failed);
	DiagSetDword(key, L"InitStatus", (ULONG)pDevice->InitStatus);
	DiagSetDword(key, L"InitFailedReg", pDevice->InitFailedReg);
	DiagSetDword(key, L"AmpPresent", pDevice->HasAmpGpio ? 1 : 0);
	DiagSetDword(key, L"AmpOpenStatus", (ULONG)pDevice->AmpOpenStatus);
	DiagSetDword(key, L"AmpWriteStatus", (ULONG)pDevice->AmpWriteStatus);
	DiagSetDword(key, L"AmpWriteValue", pDevice->AmpWriteValue);
	DiagSetDword(key, L"DumpCount", pDevice->DiagCount);
	// Not OutputRoute/InputSelect: those names are the overrides.
	DiagSetDword(key, L"ActiveOutputRoute", pDevice->OutputRoute);
	DiagSetDword(key, L"ActiveInputSelect", pDevice->InputSelect);
	DiagSetDword(key, L"JackInterrupt", pDevice->Interrupt ? 1 : 0);

	for (ULONG i = 0; i < ARRAYSIZE(DiagSocRegs); i++) {
		WCHAR buf[16];
		RtlStringCchPrintfW(buf, ARRAYSIZE(buf), L"Soc_%08X", DiagSocRegs[i]);
		DiagSetDword(key, buf, DiagReadSoc(DiagSocRegs[i]));
	}

	WdfRegistryClose(key);
}

static VOID Es8388DiagWork(_In_ WDFWORKITEM workItem) {
	WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(workItem);
	PES8323_CONTEXT pDevice = GetDeviceContext(device);

	pDevice->DiagCount++;
	Es8388DiagDump(pDevice);
}

static VOID Es8388DiagTimer(_In_ WDFTIMER timer) {
	WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(timer);
	PES8323_CONTEXT pDevice = GetDeviceContext(device);

	if (pDevice->DiagCount >= DIAG_MAX_DUMPS) {
		WdfTimerStop(timer, FALSE);
		return;
	}
	WdfWorkItemEnqueue(pDevice->DiagWorkItem);
}

NTSTATUS Es8388DiagCreate(_In_ PES8323_CONTEXT pDevice) {
	WDF_OBJECT_ATTRIBUTES attributes;
	NTSTATUS status;

	if (pDevice->DiagTimer) {
		return STATUS_SUCCESS;
	}

	WDF_WORKITEM_CONFIG workConfig;
	WDF_WORKITEM_CONFIG_INIT(&workConfig, Es8388DiagWork);
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;
	status = WdfWorkItemCreate(&workConfig, &attributes, &pDevice->DiagWorkItem);
	if (!NT_SUCCESS(status)) {
		pDevice->DiagWorkItem = NULL;
		return status;
	}

	WDF_TIMER_CONFIG timerConfig;
	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, Es8388DiagTimer, DIAG_PERIOD_MS);
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;
	status = WdfTimerCreate(&timerConfig, &attributes, &pDevice->DiagTimer);
	if (!NT_SUCCESS(status)) {
		pDevice->DiagTimer = NULL;
	}
	return status;
}

VOID Es8388DiagStart(_In_ PES8323_CONTEXT pDevice) {
	pDevice->DiagCount = 0;
	if (pDevice->DiagWorkItem) {
		// One dump right away, with the power-up result.
		WdfWorkItemEnqueue(pDevice->DiagWorkItem);
	}
	if (pDevice->DiagTimer) {
		WdfTimerStart(pDevice->DiagTimer, WDF_REL_TIMEOUT_IN_MS(DIAG_PERIOD_MS));
	}
}

VOID Es8388DiagStop(_In_ PES8323_CONTEXT pDevice) {
	if (pDevice->DiagTimer) {
		WdfTimerStop(pDevice->DiagTimer, TRUE);
	}
	if (pDevice->DiagWorkItem) {
		WdfWorkItemFlush(pDevice->DiagWorkItem);
	}
}
