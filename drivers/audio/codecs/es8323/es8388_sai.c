/*
 * ES8388 on an RK3576 SAI (ACPI\RKCP8388).
 *
 * The RK3588 boards this driver came from feed the codec 16-bit I2S from the
 * I2S/TDM block.  On RK3576 the codec sits on a SAI that csaudiork3x runs as
 * bit-clock and frame master with two 32-bit slots per frame and 24 valid
 * bits in each (the same format as its HDMI endpoint).  The codec is the I2S
 * slave, MCLK = 256 fs = 12.288 MHz from the SoC (set by the firmware), one
 * shared LRCK for the DAC and the ADC.
 *
 * Register values come from the Everest ES8388 datasheet (Revision 5.0, July
 * 2018, section 6, register descriptions) and the ES8388 User Guide
 * (2011-06-17, section 10, "Register configuration sequence": 10.1 start up
 * codec, 10.5 power down to standby).  No code from the Linux es8328 driver
 * was used.
 *
 * Board wiring the values assume (LCKFB TaishanPi 3M, from the board's
 * device tree as Armbian carries it):
 *   LOUT1/ROUT1 -> headphone amplifier -> headphone jack; OUT2 unused.
 *   LINPUT1/LINPUT2 = onboard microphone, RINPUT1/RINPUT2 = headset
 *   microphone.  Capture takes the onboard microphone (LINPUT1) on both
 *   channels.
 *
 * NOT RUN ON HARDWARE.
 *
 * Copyright (c) 2026, Jiaxing Hu <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#include "driver.h"
#include <gpio.h>
#include <reshub.h>

#define ES8388_POOL_TAG     (ULONG) '8838'

typedef struct {
	UINT8 Reg;
	UINT8 Val;
	UINT16 DelayMs;     // after the write
} ES8388_WRITE;

//
// User Guide 10.1, in its order, with these differences:
//   - reg 0 bit 7 (SCPReset) pulsed first, so every register not written
//     below is at its datasheet default;
//   - reg 4 = 0x30 (LOUT1/ROUT1 only), not 0x3C: OUT2 is not connected;
//   - reg 12 = 0x40: DATSEL 01 puts the left ADC (LINPUT1, the onboard
//     microphone) in both channels;
//   - reg 48/49 (LOUT2/ROUT2 volume) left at -45 dB;
//   - the unmutes of 10.3 and 10.2 (reg 25 = 0x32, reg 15 = 0x30) after the
//     DEM/STM power-up.
//
static const ES8388_WRITE Es8388SaiStartUp[] = {
	{ 0x00, 0x80, 0 },  // Chip Control 1: SCPReset
	{ 0x00, 0x00, 0 },
	{ 0x08, 0x00, 0 },  // Master Mode Control: MSC 0, slave
	{ 0x02, 0xF3, 0 },  // Chip Power: DEM, STM and DLLs held down
	{ 0x2B, 0x80, 0 },  // DAC Control 21: slrck, DAC and ADC share LRCK
	{ 0x00, 0x05, 0 },  // Chip Control 1: EnRef, VMIDSEL 50 kOhm
	{ 0x01, 0x40, 0 },  // Chip Control 2: analog and ibias up
	{ 0x03, 0x00, 0 },  // ADC Power: inputs, ADCs, mic bias up
	{ 0x04, 0x30, 0 },  // DAC Power: DACs, LOUT1 and ROUT1 up
	{ 0x0A, 0x00, 0 },  // ADC Control 2: LINPUT1, RINPUT1
	{ 0x09, 0x88, 0 },  // ADC Control 1: PGA +24 dB both channels
	{ 0x0C, 0x40, 0 },  // ADC Control 4: DATSEL 01, 24-bit, I2S
	{ 0x0D, 0x02, 0 },  // ADC Control 5: single speed, MCLK = 256 fs
	{ 0x10, 0x00, 0 },  // ADC Control 8: left ADC 0 dB
	{ 0x11, 0x00, 0 },  // ADC Control 9: right ADC 0 dB
	{ 0x17, 0x00, 0 },  // DAC Control 1: 24-bit, I2S
	{ 0x18, 0x02, 0 },  // DAC Control 2: single speed, MCLK = 256 fs
	{ 0x1A, 0x00, 0 },  // DAC Control 4: left DAC 0 dB
	{ 0x1B, 0x00, 0 },  // DAC Control 5: right DAC 0 dB
	{ 0x26, 0x00, 0 },  // DAC Control 16: mixer inputs LIN1/RIN1
	{ 0x27, 0xB8, 0 },  // DAC Control 17: left DAC to left mixer
	{ 0x28, 0x38, 0 },  // DAC Control 18 (default)
	{ 0x29, 0x38, 0 },  // DAC Control 19 (default)
	{ 0x2A, 0xB8, 0 },  // DAC Control 20: right DAC to right mixer
	{ 0x2E, 0x1E, 0 },  // DAC Control 24: LOUT1 0 dB
	{ 0x2F, 0x1E, 0 },  // DAC Control 25: ROUT1 0 dB
	{ 0x02, 0x00, 10 }, // Chip Power: DEM and STM up
	{ 0x19, 0x32, 0 },  // DAC Control 3: unmute, soft ramp
	{ 0x0F, 0x30, 0 },  // ADC Control 7: unmute, soft ramp
};

//
// User Guide 10.5.
//
static const ES8388_WRITE Es8388SaiStandby[] = {
	{ 0x0F, 0x34, 0 },  // ADC mute
	{ 0x19, 0x36, 0 },  // DAC mute
	{ 0x02, 0xF3, 0 },  // DEM and STM down
	{ 0x03, 0xFC, 0 },  // ADC, inputs, mic bias down
	{ 0x04, 0xC0, 0 },  // DACs down, outputs off
};

static void Es8388SaiDelayMs(ULONG ms) {
	LARGE_INTEGER interval;
	interval.QuadPart = -10000LL * ms;
	KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

static NTSTATUS Es8388SaiWriteTable(
	_In_ PES8323_CONTEXT pDevice,
	_In_reads_(count) const ES8388_WRITE* table,
	_In_ ULONG count
) {
	for (ULONG i = 0; i < count; i++) {
		NTSTATUS status = es8323_reg_write(pDevice, table[i].Reg, table[i].Val);
		if (!NT_SUCCESS(status)) {
			DbgPrint("es8323: ES8388 write reg 0x%02x = 0x%02x failed 0x%x\n",
				table[i].Reg, table[i].Val, status);
			pDevice->InitFailedReg = table[i].Reg;
			return status;
		}
		if (table[i].DelayMs) {
			Es8388SaiDelayMs(table[i].DelayMs);
		}
	}
	return STATUS_SUCCESS;
}

BOOLEAN Es8388SaiIsBound(_In_ WDFDEVICE FxDevice) {
	WDFMEMORY memory = NULL;
	size_t size = 0;
	BOOLEAN found = FALSE;

	NTSTATUS status = WdfDeviceAllocAndQueryProperty(FxDevice,
		DevicePropertyHardwareID, NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, &memory);
	if (!NT_SUCCESS(status)) {
		return FALSE;
	}

	PWCHAR ids = (PWCHAR)WdfMemoryGetBuffer(memory, &size);
	size_t chars = size / sizeof(WCHAR);

	// A REG_MULTI_SZ: strings back to back, an empty string at the end.
	for (size_t i = 0; i < chars && ids[i] != L'\0'; ) {
		size_t len = 0;
		while (i + len < chars && ids[i + len] != L'\0') {
			len++;
		}
		for (size_t j = 0; j + 8 <= len; j++) {
			if (RtlCompareMemory(&ids[i + j], L"RKCP8388", 8 * sizeof(WCHAR)) == 8 * sizeof(WCHAR)) {
				found = TRUE;
				break;
			}
		}
		if (found) {
			break;
		}
		i += len + 1;
	}

	WdfObjectDelete(memory);
	return found;
}

NTSTATUS Es8388SaiOpenAmpGpio(_In_ PES8323_CONTEXT pDevice) {
	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_IO_TARGET_OPEN_PARAMS openParams;
	UNICODE_STRING path;
	WCHAR pathBuffer[RESOURCE_HUB_PATH_SIZE];
	NTSTATUS status;

	if (!pDevice->HasAmpGpio) {
		return STATUS_SUCCESS;
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;

	status = WdfIoTargetCreate(pDevice->FxDevice, &attributes, &pDevice->AmpGpioTarget);
	if (!NT_SUCCESS(status)) {
		pDevice->AmpGpioTarget = NULL;
		return status;
	}

	RtlInitEmptyUnicodeString(&path, pathBuffer, sizeof(pathBuffer));
	status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path,
		pDevice->AmpGpioResHubId.LowPart, pDevice->AmpGpioResHubId.HighPart);
	if (!NT_SUCCESS(status)) {
		goto fail;
	}

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &path, GENERIC_WRITE);
	openParams.ShareAccess = 0;
	openParams.CreateDisposition = FILE_OPEN;
	openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;

	status = WdfIoTargetOpen(pDevice->AmpGpioTarget, &openParams);
	if (!NT_SUCCESS(status)) {
		goto fail;
	}
	pDevice->AmpOpenStatus = STATUS_SUCCESS;
	return STATUS_SUCCESS;

fail:
	pDevice->AmpOpenStatus = status;
	DbgPrint("es8323: cannot open the amplifier GPIO: 0x%x\n", status);
	WdfObjectDelete(pDevice->AmpGpioTarget);
	pDevice->AmpGpioTarget = NULL;
	return status;
}

VOID Es8388SaiCloseAmpGpio(_In_ PES8323_CONTEXT pDevice) {
	if (pDevice->AmpGpioTarget) {
		WdfIoTargetClose(pDevice->AmpGpioTarget);
		WdfObjectDelete(pDevice->AmpGpioTarget);
		pDevice->AmpGpioTarget = NULL;
	}
}

static NTSTATUS Es8388SaiSetAmp(_In_ PES8323_CONTEXT pDevice, BOOLEAN on) {
	WDF_MEMORY_DESCRIPTOR inDesc;
	WDF_MEMORY_DESCRIPTOR outDesc;
	UCHAR value = on ? 1 : 0;
	UCHAR readBack = 0;

	if (!pDevice->AmpGpioTarget) {
		return STATUS_SUCCESS;
	}

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, &value, sizeof(value));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, &readBack, sizeof(readBack));

	NTSTATUS status = WdfIoTargetSendIoctlSynchronously(pDevice->AmpGpioTarget, NULL,
		IOCTL_GPIO_WRITE_PINS, &inDesc, &outDesc, NULL, NULL);
	if (!NT_SUCCESS(status)) {
		DbgPrint("es8323: amplifier GPIO write %u failed 0x%x\n", value, status);
	}
	pDevice->AmpWriteStatus = status;
	pDevice->AmpWriteValue = value;
	return status;
}

NTSTATUS Es8388SaiPowerUp(_In_ PES8323_CONTEXT pDevice) {
	pDevice->InitFailedReg = 0xFFFFFFFF;
	NTSTATUS status = Es8388SaiWriteTable(pDevice, Es8388SaiStartUp, ARRAYSIZE(Es8388SaiStartUp));
	pDevice->InitStatus = status;
	if (!NT_SUCCESS(status)) {
		return status;
	}

	// The amplifier last, once the outputs are biased and unmuted.
	Es8388SaiSetAmp(pDevice, TRUE);
	return STATUS_SUCCESS;
}

VOID Es8388SaiPowerDown(_In_ PES8323_CONTEXT pDevice) {
	// The amplifier first, so it does not pass on the outputs going down.
	Es8388SaiSetAmp(pDevice, FALSE);
	Es8388SaiWriteTable(pDevice, Es8388SaiStandby, ARRAYSIZE(Es8388SaiStandby));
}
