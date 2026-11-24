#include "definitions.h"
#include "hw.h"

VOID CsAudioCallbackFunction(
	CCsAudioRk3xHW* hw,
	CsAudioArg* arg,
	PVOID Argument2
) {
	if (!hw) {
		return;
	}

	if (Argument2 == &hw->CsAudioArg2) {
		return;
	}

	CsAudioArg localArg;
	RtlZeroMemory(&localArg, sizeof(CsAudioArg));
	RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));

	hw->CSAudioAPICalled(localArg);
}

NTSTATUS CCsAudioRk3xHW::CSAudioAPIInit() {
	NTSTATUS status;

	UNICODE_STRING CSAudioCallbackAPI;
	RtlInitUnicodeString(&CSAudioCallbackAPI, L"\\CallBack\\CsAudioCallbackAPI");


	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes,
		&CSAudioCallbackAPI,
		OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
		NULL,
		NULL
	);
	status = ExCreateCallback(&this->CSAudioAPICallback, &attributes, TRUE, TRUE);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	this->CSAudioAPICallbackObj = ExRegisterCallback(this->CSAudioAPICallback,
		(PCALLBACK_FUNCTION)CsAudioCallbackFunction,
		this
	);
	if (!this->CSAudioAPICallbackObj) {

		return STATUS_NO_CALLBACK_ACTIVE;
	}

	CsAudioArg arg;
	RtlZeroMemory(&arg, sizeof(CsAudioArg));
	arg.argSz = sizeof(CsAudioArg);
	arg.endpointType = CSAudioEndpointTypeDSP;
	arg.endpointRequest = CSAudioEndpointRegister;
	ExNotifyCallback(this->CSAudioAPICallback, &arg, &CsAudioArg2);

	return status;
}

NTSTATUS CCsAudioRk3xHW::CSAudioAPIDeinit() {
	if (this->CSAudioAPICallbackObj) {
		ExUnregisterCallback(this->CSAudioAPICallbackObj);
		this->CSAudioAPICallbackObj = NULL;
	}

	if (this->CSAudioAPICallback) {
		ObfDereferenceObject(this->CSAudioAPICallback);
		this->CSAudioAPICallback = NULL;
	}
	return STATUS_SUCCESS;
}

void CCsAudioRk3xHW::CSAudioAPICalled(CsAudioArg arg) {
	if (arg.endpointRequest == CSAudioEndpointRegister) {
		CsAudioArg newArg;
		RtlZeroMemory(&newArg, sizeof(CsAudioArg));
		newArg.argSz = sizeof(CsAudioArg);
		newArg.endpointType = arg.endpointType;
		newArg.endpointRequest = CSAudioEndpointStop;
		ExNotifyCallback(this->CSAudioAPICallback, &newArg, &CsAudioArg2);
	}
	else if (arg.endpointRequest == CSAudioEndpointJackState &&
		arg.endpointType == CSAudioEndpointTypeHeadphone &&
		arg.argSz >= RTL_SIZEOF_THROUGH_FIELD(CsAudioArg, jackState)) {
		jack_report(arg.jackState.connected != 0);
	}
}

//
// Headphone jack presence.
//
// The codec driver calls in from PASSIVE_LEVEL (its debounce timer or a
// work item), the miniport registers from Init and its destructor, all at
// PASSIVE_LEVEL; a guarded mutex keeps the port events pointer valid while
// GenerateEventList runs on it.
//

void CCsAudioRk3xHW::jack_register(_In_ PPORTEVENTS portEvents, _In_ ULONG pinId) {
	KeAcquireGuardedMutex(&this->jackLock);
	this->jackPortEvents = portEvents;
	this->jackPinId = pinId;
	KeReleaseGuardedMutex(&this->jackLock);
}

void CCsAudioRk3xHW::jack_unregister(_In_ PPORTEVENTS portEvents) {
	KeAcquireGuardedMutex(&this->jackLock);
	if (this->jackPortEvents == portEvents) {
		this->jackPortEvents = NULL;
	}
	KeReleaseGuardedMutex(&this->jackLock);
}

BOOLEAN CCsAudioRk3xHW::jack_connected() {
	return this->jackIsConnected;
}

void CCsAudioRk3xHW::jack_report(BOOLEAN connected) {
	if (KeGetCurrentIrql() > APC_LEVEL) {
		return;
	}

	KeAcquireGuardedMutex(&this->jackLock);
	BOOLEAN changed = (this->jackIsConnected != connected) || !this->jackReported;
	this->jackIsConnected = connected;
	this->jackReported = TRUE;
	this->jackReports++;
	if (changed && this->jackPortEvents) {
		// The endpoint builder re-reads KSPROPERTY_JACK_DESCRIPTION and
		// sets the endpoint to active or unplugged.
		this->jackPortEvents->GenerateEventList(
			(GUID*)&KSEVENTSETID_PinCapsChange,
			KSEVENT_PINCAPS_JACKINFOCHANGE,
			TRUE,                   // pin event
			this->jackPinId,        // the topology's bridge pin
			FALSE,                  // not a node event
			ULONG(-1));
		this->jackEvents++;
	}
	KeReleaseGuardedMutex(&this->jackLock);

	jack_diag_save();
}

static void jack_put(HANDLE key, PCWSTR name, ULONG value) {
	UNICODE_STRING us;
	RtlInitUnicodeString(&us, name);
	ZwSetValueKey(key, &us, 0, REG_DWORD, &value, sizeof(value));
}

// Into the adapter's "Device Parameters" key, next to the Diag* values.
void CCsAudioRk3xHW::jack_diag_save() {
	HANDLE key = NULL;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL || this->diagPdo == NULL) {
		return;
	}
	if (!NT_SUCCESS(IoOpenDeviceRegistryKey(this->diagPdo, PLUGPLAY_REGKEY_DEVICE,
		KEY_WRITE, &key))) {
		return;
	}
	jack_put(key, L"JackConnected", this->jackIsConnected);
	jack_put(key, L"JackReports", this->jackReports);
	jack_put(key, L"JackEvents", this->jackEvents);
	jack_put(key, L"JackTopologyRegistered", this->jackPortEvents != NULL);
	ZwClose(key);
}

CSAudioEndpointType CCsAudioRk3xHW::GetCSAudioEndpoint(eDeviceType deviceType) {
	switch (deviceType) {
	case eOutputDevice:
		return CSAudioEndpointTypeHeadphone;
	case eInputDevice:
		return CSAudioEndpointTypeMicJack;
	}
	return CSAudioEndpointTypeDSP;
}

eDeviceType CCsAudioRk3xHW::GetDeviceType(CSAudioEndpointType endpointType) {
	switch (endpointType) {
	case CSAudioEndpointTypeHeadphone:
		return eOutputDevice;
	case CSAudioEndpointTypeMicJack:
		return eInputDevice;
	}
	return eOutputDevice;
}