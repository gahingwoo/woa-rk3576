/*
 * Headphone jack detection for the ES8388 on an RK3576 SAI (RKCP8388 only).
 *
 * The ES8388 has no jack detect of its own; the board wires the jack's
 * detect switch to a SoC GPIO (TaishanPi 3M: GPIO4_PB0, hp-det-gpios in its
 * device tree).  The firmware describes that line twice in _CRS, as on the
 * SD card detect:
 *
 *   GpioInt (Edge, ActiveBoth, Shared, PullUp)   the line moved
 *   GpioIo  (Shared, PullUp, InputOnly)          read its level
 *
 * and in _DSD:
 *
 *   rockchip,hp-det-gpio-io      which GpioIo of _CRS is the detect line
 *   rockchip,hp-det-active-high  1: high = plugged, 0: low = plugged
 *   rockchip,hp-det-debounce-ms  settle time after the last edge
 *
 * Each edge (re)starts a passive-level timer; when the line has been quiet
 * for the debounce time the driver reads the level and, when it differs from
 * what it last reported, tells csaudiork3x through the CsAudio callback
 * (\Callback\CsAudioCallbackAPI, CSAudioEndpointJackState for the headphone
 * endpoint).  csaudiork3x answers KSPROPERTY_JACK_DESCRIPTION with it and
 * raises KSEVENT_PINCAPS_JACKINFOCHANGE, so Windows marks the Headphones
 * endpoint plugged or unplugged.  When csaudiork3x registers after the
 * codec has started, it gets the current state again.
 *
 * With the jack unplugged the headphone amplifier is switched off (its enable
 * is the first GpioIo, es8388_sai.c), and back on when a plug goes in.
 *
 * Without the _DSD (older firmware) nothing here runs and the endpoint stays
 * plugged, as before.
 *
 * Overrides, REG_DWORD in the device's "Device Parameters" key
 * (HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP8388\0\Device Parameters),
 * read at start:
 *   JackDetect    0: do not report jack state at all (default 1)
 *   HpDetInvert   1: the opposite of the firmware's active level (default 0)
 *   JackAmpGate   0: leave the amplifier on when unplugged (default 1)
 * and written by the driver:
 *   JackSetupStatus, JackOpenStatus, JackReadStatus (NTSTATUS),
 *   JackHpDetIndex, JackActiveHigh, JackDebounceMs, JackLevel,
 *   JackConnected, JackEdges, JackChanges, JackNotifies
 *
 * Copyright (c) 2026, Jiaxing Hu <gahing@gahingwoo.com>
 * SPDX-License-Identifier: MIT
 */

#include "driver.h"
#include <gpio.h>
#include <reshub.h>
#include <acpiioct.h>
#include <ntstrsafe.h>

DEFINE_GUID(ES8388_GUID_ACPI_DSD,
	0xdaffd814, 0x6eba, 0x4d8c, 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01);

#define ES8388_JACK_DEFAULT_DEBOUNCE_MS 150
#define ES8388_JACK_MAX_DEBOUNCE_MS     2000u

//
// The CsAudio callback API, laid out as csaudiork3x's Utilities/hw.h has it.
//
typedef enum {
	EsCsAudioEndpointTypeDSP,
	EsCsAudioEndpointTypeSpeaker,
	EsCsAudioEndpointTypeHeadphone,
	EsCsAudioEndpointTypeMicArray,
	EsCsAudioEndpointTypeMicJack
} ES_CSAUDIO_ENDPOINT_TYPE;

typedef enum {
	EsCsAudioEndpointRegister,
	EsCsAudioEndpointStart,
	EsCsAudioEndpointStop,
	EsCsAudioEndpointOverrideFormat,
	EsCsAudioEndpointJackState
} ES_CSAUDIO_ENDPOINT_REQUEST;

typedef struct {
	UINT16 channels;
	UINT16 frequency;
	UINT16 bitsPerSample;
	UINT16 validBitsPerSample;
	BOOL force32BitOutputContainer;
} ES_CSAUDIO_FORMAT_OVERRIDE;

typedef struct {
	UINT32 connected;
} ES_CSAUDIO_JACK_STATE;

typedef struct {
	UINT32 argSz;
	ES_CSAUDIO_ENDPOINT_TYPE endpointType;
	ES_CSAUDIO_ENDPOINT_REQUEST endpointRequest;
	union {
		ES_CSAUDIO_FORMAT_OVERRIDE formatOverride;
		ES_CSAUDIO_JACK_STATE jackState;
	};
} ES_CSAUDIO_ARG, *PES_CSAUDIO_ARG;

// Argument2 of our own notifications, so the callback can skip them.
static int Es8388JackMarker;

/* ------------------------------------------------------------ diagnostics */

static void JackPut(WDFKEY key, PCWSTR name, ULONG value) {
	UNICODE_STRING us;
	RtlInitUnicodeString(&us, name);
	WdfRegistryAssignULong(key, &us, value);
}

static void Es8388JackDiagSave(_In_ PES8323_CONTEXT pDevice) {
	WDFKEY key;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
		return;
	}
	if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(pDevice->FxDevice, PLUGPLAY_REGKEY_DEVICE,
		KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
		return;
	}
	JackPut(key, L"JackSetupStatus", (ULONG)pDevice->JackSetupStatus);
	JackPut(key, L"JackOpenStatus", (ULONG)pDevice->JackOpenStatus);
	JackPut(key, L"JackReadStatus", (ULONG)pDevice->JackReadStatus);
	JackPut(key, L"JackHpDetIndex", pDevice->HpDetIoIndex);
	JackPut(key, L"JackActiveHigh", pDevice->HpDetActiveHigh);
	JackPut(key, L"JackDebounceMs", pDevice->HpDetDebounceMs);
	JackPut(key, L"JackLevel", pDevice->JackLevel);
	JackPut(key, L"JackConnected", pDevice->JackKnown ? pDevice->JackConnected : 0xFFFFFFFF);
	JackPut(key, L"JackEdges", (ULONG)pDevice->JackEdges);
	JackPut(key, L"JackChanges", pDevice->JackChanges);
	JackPut(key, L"JackNotifies", pDevice->JackNotifies);
	WdfRegistryClose(key);
}

/* ------------------------------------------------------------ _DSD */

static BOOLEAN DsdNameIs(_In_ PACPI_METHOD_ARGUMENT name, _In_ PCSTR str) {
	size_t len = strlen(str);
	return name->Type == ACPI_METHOD_ARGUMENT_STRING &&
		name->DataLength == len + 1 &&
		RtlCompareMemory(name->Data, str, len) == len;
}

static BOOLEAN DsdInteger(_In_ PACPI_METHOD_ARGUMENT value, _Out_ ULONG* out) {
	*out = 0;
	if (value->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
		return FALSE;
	}
	RtlCopyMemory(out, value->Data, min(value->DataLength, sizeof(*out)));
	return TRUE;
}

//
// The jack-detect properties of _DSD.  STATUS_NOT_FOUND when the firmware
// has none of them.
//
static NTSTATUS Es8388JackReadDsd(_In_ PES8323_CONTEXT pDevice) {
	ACPI_EVAL_INPUT_BUFFER_EX inputBuffer;
	ACPI_EVAL_OUTPUT_BUFFER outputSizeBuffer;
	WDF_MEMORY_DESCRIPTOR inputMemDesc, outputSizeMemDesc, outputMemDesc;
	WDF_OBJECT_ATTRIBUTES attributes;
	WDFMEMORY outputMemory = NULL;
	PACPI_EVAL_OUTPUT_BUFFER outputBuffer;
	BOOLEAN haveIndex = FALSE;
	NTSTATUS status;

	RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));
	inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
	status = RtlStringCchCopyA(inputBuffer.MethodName, sizeof(inputBuffer.MethodName), "_DSD");
	if (!NT_SUCCESS(status)) {
		return status;
	}

	RtlZeroMemory(&outputSizeBuffer, sizeof(outputSizeBuffer));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, &inputBuffer, (ULONG)sizeof(inputBuffer));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputSizeMemDesc, &outputSizeBuffer, (ULONG)sizeof(outputSizeBuffer));

	status = WdfIoTargetSendInternalIoctlSynchronously(WdfDeviceGetIoTarget(pDevice->FxDevice),
		NULL, IOCTL_ACPI_EVAL_METHOD_EX, &inputMemDesc, &outputSizeMemDesc, NULL, NULL);
	if (status != STATUS_BUFFER_OVERFLOW) {
		// No _DSD, or an empty one.
		return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
	}

	// As rk-tplg.cpp: Length, plus the header, to be safe.
	if (outputSizeBuffer.Length > 4096) {
		return STATUS_ACPI_INVALID_DATA;
	}
	size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + outputSizeBuffer.Length;
	if (outputBufferSize < sizeof(ACPI_EVAL_OUTPUT_BUFFER)) {
		outputBufferSize = sizeof(ACPI_EVAL_OUTPUT_BUFFER);
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;
	status = WdfMemoryCreate(&attributes, NonPagedPoolNx, ES8323_POOL_TAG, outputBufferSize,
		&outputMemory, (PVOID*)&outputBuffer);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	RtlZeroMemory(outputBuffer, outputBufferSize);
	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

	status = WdfIoTargetSendInternalIoctlSynchronously(WdfDeviceGetIoTarget(pDevice->FxDevice),
		NULL, IOCTL_ACPI_EVAL_METHOD_EX, &inputMemDesc, &outputMemDesc, NULL, NULL);
	if (!NT_SUCCESS(status)) {
		goto exit;
	}
	if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || (outputBuffer->Count % 2) != 0) {
		status = STATUS_ACPI_INVALID_DATA;
		goto exit;
	}

	PUCHAR end = (PUCHAR)outputBuffer + outputBufferSize;
	PACPI_METHOD_ARGUMENT arg = &outputBuffer->Argument[0];
	for (ULONG i = 0; i < outputBuffer->Count; i += 2) {
		PACPI_METHOD_ARGUMENT uuidArg = arg;
		PACPI_METHOD_ARGUMENT packageArg = ACPI_METHOD_NEXT_ARGUMENT(uuidArg);
		arg = ACPI_METHOD_NEXT_ARGUMENT(packageArg);

		if ((PUCHAR)arg > end ||
			uuidArg->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
			uuidArg->DataLength != sizeof(GUID) ||
			packageArg->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
			break;
		}
		if (RtlCompareMemory(uuidArg->Data, &ES8388_GUID_ACPI_DSD, sizeof(GUID)) != sizeof(GUID)) {
			continue;
		}

		PUCHAR packageEnd = packageArg->Data + packageArg->DataLength;
		for (PACPI_METHOD_ARGUMENT property = (PACPI_METHOD_ARGUMENT)packageArg->Data;
			(PUCHAR)property < packageEnd;
			property = ACPI_METHOD_NEXT_ARGUMENT(property)) {
			if (property->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
				continue;
			}
			PACPI_METHOD_ARGUMENT name = (PACPI_METHOD_ARGUMENT)property->Data;
			PACPI_METHOD_ARGUMENT value = ACPI_METHOD_NEXT_ARGUMENT(name);
			if ((PUCHAR)value >= property->Data + property->DataLength) {
				continue;
			}
			ULONG v;

			if (DsdNameIs(name, "rockchip,hp-det-gpio-io") && DsdInteger(value, &v)) {
				pDevice->HpDetIoIndex = v;
				haveIndex = TRUE;
			}
			else if (DsdNameIs(name, "rockchip,hp-det-active-high") && DsdInteger(value, &v)) {
				pDevice->HpDetActiveHigh = (v != 0);
			}
			else if (DsdNameIs(name, "rockchip,hp-det-debounce-ms") && DsdInteger(value, &v)) {
				pDevice->HpDetDebounceMs = min(v, ES8388_JACK_MAX_DEBOUNCE_MS);
			}
		}
	}
	status = haveIndex ? STATUS_SUCCESS : STATUS_NOT_FOUND;

exit:
	WdfObjectDelete(outputMemory);
	return status;
}

/* ------------------------------------------------------------ resources */

VOID Es8388JackSelectGpios(_In_ PES8323_CONTEXT pDevice) {
	ULONG ampIndex = ES8388_NO_INDEX;

	pDevice->HpDetIoIndex = ES8388_NO_INDEX;
	pDevice->HpDetActiveHigh = TRUE;
	pDevice->HpDetDebounceMs = ES8388_JACK_DEFAULT_DEBOUNCE_MS;
	pDevice->JackSetupStatus = STATUS_NOT_FOUND;

	if (pDevice->Rk3576Sai) {
		pDevice->JackSetupStatus = Es8388JackReadDsd(pDevice);
		if (!NT_SUCCESS(pDevice->JackSetupStatus) || pDevice->HpDetIoIndex >= pDevice->GpioIoCount ||
			pDevice->HpDetIoIndex >= ES8388_MAX_GPIOIO) {
			if (NT_SUCCESS(pDevice->JackSetupStatus)) {
				pDevice->JackSetupStatus = STATUS_INVALID_PARAMETER;
			}
			pDevice->HpDetIoIndex = ES8388_NO_INDEX;
		}
	}

	// The amplifier enable: the first GpioIo that is not the detect line.
	for (ULONG i = 0; i < pDevice->GpioIoCount && i < ES8388_MAX_GPIOIO; i++) {
		if (i != pDevice->HpDetIoIndex) {
			ampIndex = i;
			break;
		}
	}
	if (ampIndex != ES8388_NO_INDEX) {
		pDevice->AmpGpioResHubId = pDevice->GpioIoResHubId[ampIndex];
		pDevice->HasAmpGpio = TRUE;
	}
}

static ULONG JackRegDword(WDFKEY key, PCWSTR name, ULONG dflt) {
	UNICODE_STRING us;
	ULONG v;

	RtlInitUnicodeString(&us, name);
	if (NT_SUCCESS(WdfRegistryQueryULong(key, &us, &v))) {
		return v;
	}
	return dflt;
}

static NTSTATUS Es8388JackOpenTarget(_In_ PES8323_CONTEXT pDevice, ULONG shareAccess) {
	WDF_IO_TARGET_OPEN_PARAMS openParams;
	UNICODE_STRING path;
	WCHAR pathBuffer[RESOURCE_HUB_PATH_SIZE];
	LARGE_INTEGER id = pDevice->GpioIoResHubId[pDevice->HpDetIoIndex];

	RtlInitEmptyUnicodeString(&path, pathBuffer, sizeof(pathBuffer));
	NTSTATUS status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&path, id.LowPart, id.HighPart);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &path, GENERIC_READ);
	openParams.ShareAccess = shareAccess;
	openParams.CreateDisposition = FILE_OPEN;
	openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;
	return WdfIoTargetOpen(pDevice->HpDetTarget, &openParams);
}

static VOID Es8388JackTimer(_In_ WDFTIMER timer);
static VOID Es8388JackWork(_In_ WDFWORKITEM workItem);
static VOID Es8388JackCsAudioCallback(PVOID context, PVOID argument1, PVOID argument2);

//
// From OnPrepareHardware, after Es8388JackSelectGpios and the SPB target.
// A failure only costs jack detection; the codec still starts.
//
VOID Es8388JackPrepare(_In_ PES8323_CONTEXT pDevice) {
	WDF_OBJECT_ATTRIBUTES attributes;
	WDFKEY key;
	BOOLEAN enabled = TRUE, invert = FALSE;
	NTSTATUS status;

	pDevice->JackOpenStatus = STATUS_NOT_FOUND;
	pDevice->JackAmpGate = TRUE;

	if (pDevice->HpDetIoIndex == ES8388_NO_INDEX) {
		goto out;
	}

	if (NT_SUCCESS(WdfDeviceOpenRegistryKey(pDevice->FxDevice, PLUGPLAY_REGKEY_DEVICE,
		KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
		enabled = JackRegDword(key, L"JackDetect", 1) != 0;
		invert = JackRegDword(key, L"HpDetInvert", 0) != 0;
		pDevice->JackAmpGate = JackRegDword(key, L"JackAmpGate", 1) != 0;
		WdfRegistryClose(key);
	}
	if (!enabled) {
		pDevice->JackSetupStatus = STATUS_NOT_SUPPORTED;
		goto out;
	}
	if (invert) {
		pDevice->HpDetActiveHigh = !pDevice->HpDetActiveHigh;
	}

	// The detect line, read only.
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;
	status = WdfIoTargetCreate(pDevice->FxDevice, &attributes, &pDevice->HpDetTarget);
	if (!NT_SUCCESS(status)) {
		pDevice->HpDetTarget = NULL;
		pDevice->JackOpenStatus = status;
		goto out;
	}
	status = Es8388JackOpenTarget(pDevice, 0);
	if (!NT_SUCCESS(status)) {
		// The pin is Shared with the GpioInt; try again sharing the open.
		status = Es8388JackOpenTarget(pDevice, FILE_SHARE_READ | FILE_SHARE_WRITE);
	}
	pDevice->JackOpenStatus = status;
	if (!NT_SUCCESS(status)) {
		DbgPrint("es8323: cannot open the jack-detect GPIO: 0x%x\n", status);
		WdfObjectDelete(pDevice->HpDetTarget);
		pDevice->HpDetTarget = NULL;
		goto out;
	}

	// The debounce timer runs at PASSIVE_LEVEL: it does GPIO and I2C I/O.
	if (!pDevice->JackTimer) {
		WDF_TIMER_CONFIG timerConfig;
		WDF_TIMER_CONFIG_INIT(&timerConfig, Es8388JackTimer);
		timerConfig.AutomaticSerialization = FALSE;
		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = pDevice->FxDevice;
		attributes.ExecutionLevel = WdfExecutionLevelPassive;
		status = WdfTimerCreate(&timerConfig, &attributes, &pDevice->JackTimer);
		if (!NT_SUCCESS(status)) {
			pDevice->JackTimer = NULL;
			pDevice->JackSetupStatus = status;
			goto out;
		}
	}
	if (!pDevice->JackWorkItem) {
		WDF_WORKITEM_CONFIG workConfig;
		WDF_WORKITEM_CONFIG_INIT(&workConfig, Es8388JackWork);
		workConfig.AutomaticSerialization = FALSE;
		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = pDevice->FxDevice;
		status = WdfWorkItemCreate(&workConfig, &attributes, &pDevice->JackWorkItem);
		if (!NT_SUCCESS(status)) {
			pDevice->JackWorkItem = NULL;
		}
	}

	// The channel to csaudiork3x; whichever driver comes first creates it.
	{
		UNICODE_STRING name;
		OBJECT_ATTRIBUTES objAttributes;

		RtlInitUnicodeString(&name, L"\\CallBack\\CsAudioCallbackAPI");
		InitializeObjectAttributes(&objAttributes, &name,
			OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
		status = ExCreateCallback(&pDevice->CsAudioCallback, &objAttributes, TRUE, TRUE);
		if (!NT_SUCCESS(status)) {
			pDevice->CsAudioCallback = NULL;
			pDevice->JackSetupStatus = status;
			goto out;
		}
		pDevice->CsAudioCallbackReg = ExRegisterCallback(pDevice->CsAudioCallback,
			Es8388JackCsAudioCallback, pDevice);
	}
	pDevice->JackSetupStatus = STATUS_SUCCESS;

out:
	Es8388JackDiagSave(pDevice);
}

VOID Es8388JackRelease(_In_ PES8323_CONTEXT pDevice) {
	if (pDevice->JackTimer) {
		WdfTimerStop(pDevice->JackTimer, TRUE);
	}
	if (pDevice->CsAudioCallbackReg) {
		ExUnregisterCallback(pDevice->CsAudioCallbackReg);
		pDevice->CsAudioCallbackReg = NULL;
	}
	if (pDevice->JackWorkItem) {
		WdfWorkItemFlush(pDevice->JackWorkItem);
	}
	if (pDevice->CsAudioCallback) {
		ObDereferenceObject(pDevice->CsAudioCallback);
		pDevice->CsAudioCallback = NULL;
	}
	if (pDevice->HpDetTarget) {
		WdfIoTargetClose(pDevice->HpDetTarget);
		WdfObjectDelete(pDevice->HpDetTarget);
		pDevice->HpDetTarget = NULL;
	}
	pDevice->JackInD0 = FALSE;
}

/* ------------------------------------------------------------ state */

static VOID Es8388JackNotify(_In_ PES8323_CONTEXT pDevice) {
	ES_CSAUDIO_ARG arg;

	if (!pDevice->CsAudioCallback || !pDevice->JackKnown) {
		return;
	}
	RtlZeroMemory(&arg, sizeof(arg));
	arg.argSz = sizeof(arg);
	arg.endpointType = EsCsAudioEndpointTypeHeadphone;
	arg.endpointRequest = EsCsAudioEndpointJackState;
	arg.jackState.connected = pDevice->JackConnected ? 1 : 0;
	ExNotifyCallback(pDevice->CsAudioCallback, &arg, &Es8388JackMarker);
	pDevice->JackNotifies++;
}

static NTSTATUS Es8388JackRead(_In_ PES8323_CONTEXT pDevice, _Out_ UCHAR* level) {
	WDF_MEMORY_DESCRIPTOR outDesc;

	*level = 0;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, level, sizeof(*level));
	return WdfIoTargetSendIoctlSynchronously(pDevice->HpDetTarget, NULL,
		IOCTL_GPIO_READ_PINS, NULL, &outDesc, NULL, NULL);
}

//
// The line has been quiet for the debounce time: read it and report a change.
//
static VOID Es8388JackTimer(_In_ WDFTIMER timer) {
	WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(timer);
	PES8323_CONTEXT pDevice = GetDeviceContext(device);
	UCHAR level;

	if (!pDevice->HpDetTarget || !pDevice->JackInD0) {
		return;
	}

	pDevice->JackReadStatus = Es8388JackRead(pDevice, &level);
	if (!NT_SUCCESS(pDevice->JackReadStatus)) {
		Es8388JackDiagSave(pDevice);
		return;
	}
	pDevice->JackLevel = level & 1;

	BOOLEAN connected = ((level & 1) != 0) == pDevice->HpDetActiveHigh;
	BOOLEAN changed = !pDevice->JackKnown || connected != pDevice->JackConnected;
	pDevice->JackConnected = connected;
	pDevice->JackKnown = TRUE;

	// The amplifier follows the jack; D0Entry has just switched it on.
	if (pDevice->JackAmpGate && pDevice->AmpGpioTarget &&
		(pDevice->AmpWriteValue != (ULONG)connected || !NT_SUCCESS(pDevice->AmpWriteStatus))) {
		Es8388SaiSetAmp(pDevice, connected);
	}

	if (changed) {
		pDevice->JackChanges++;
		Es8388JackNotify(pDevice);
	}
	Es8388JackDiagSave(pDevice);
}

// csaudiork3x (re)started: give it the state it missed.
static VOID Es8388JackWork(_In_ WDFWORKITEM workItem) {
	WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(workItem);
	Es8388JackNotify(GetDeviceContext(device));
}

static VOID Es8388JackCsAudioCallback(PVOID context, PVOID argument1, PVOID argument2) {
	PES8323_CONTEXT pDevice = (PES8323_CONTEXT)context;
	PES_CSAUDIO_ARG arg = (PES_CSAUDIO_ARG)argument1;

	if (argument2 == &Es8388JackMarker || !pDevice || !arg ||
		arg->argSz < RTL_SIZEOF_THROUGH_FIELD(ES_CSAUDIO_ARG, endpointRequest)) {
		return;
	}
	// csaudiork3x announces itself with a DSP Register.  Do not call back
	// into the callback object from here: queue it.
	if (arg->endpointType == EsCsAudioEndpointTypeDSP &&
		arg->endpointRequest == EsCsAudioEndpointRegister &&
		pDevice->JackWorkItem) {
		WdfWorkItemEnqueue(pDevice->JackWorkItem);
	}
}

/* ------------------------------------------------------------ power, interrupt */

VOID Es8388JackD0Entry(_In_ PES8323_CONTEXT pDevice) {
	pDevice->JackInD0 = TRUE;
	if (pDevice->JackTimer && pDevice->HpDetTarget) {
		// The first read, once the line has settled after power-up.
		WdfTimerStart(pDevice->JackTimer, WDF_REL_TIMEOUT_IN_MS(pDevice->HpDetDebounceMs));
	}
}

VOID Es8388JackD0Exit(_In_ PES8323_CONTEXT pDevice) {
	// The framework has disabled the interrupt already.
	pDevice->JackInD0 = FALSE;
	if (pDevice->JackTimer) {
		WdfTimerStop(pDevice->JackTimer, TRUE);
	}
}

// From the passive-level ISR: an edge on the detect line.
VOID Es8388JackInterrupt(_In_ PES8323_CONTEXT pDevice) {
	InterlockedIncrement(&pDevice->JackEdges);
	if (pDevice->JackTimer && pDevice->HpDetTarget && pDevice->JackInD0) {
		// Restarted by every edge: it fires once the line is quiet.
		WdfTimerStart(pDevice->JackTimer, WDF_REL_TIMEOUT_IN_MS(pDevice->HpDetDebounceMs));
	}
}
