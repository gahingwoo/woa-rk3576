/*
 * rk-monitor: follow the headphone jack with the default playback device.
 *
 * On the TaishanPi 3M the 3.5 mm jack (ES8388 on SAI1) and HDMI (SAI6) are
 * two separate audio adapters. With jack detection in the drivers, Windows
 * marks the "Headphones" endpoint active when a plug goes in and unplugged
 * when it comes out, but it does not move the default device between
 * adapters on a jack event: the sound stays on HDMI with headphones in.
 * This does it from the user's session:
 *
 *   plug in   remember the current default playback device, then make the
 *             headphones the default for all three roles (console,
 *             multimedia, communications);
 *   unplug    make the remembered device the default again, if it is still
 *             there and active (Windows has picked some other device by
 *             then, perhaps the EC20 modem's "Speakers").
 *
 * The headphone endpoint is the render endpoint whose form factor is
 * Headphones and whose adapter's name has "Rockchip" in it, so a USB or
 * Bluetooth headset is never touched. The change is seen through
 * IMMNotificationClient::OnDeviceStateChanged (the callback only posts a
 * message; the work is done on the UI thread). When rk-monitor starts with
 * headphones already plugged in and another device as the default, it
 * switches too.
 *
 * Setting the default device. Windows has no public API for it. Like the
 * Sound control panel, and like the many tools that do this (SoundSwitch,
 * EndPointController, AudioDeviceCmdlets), this uses the undocumented
 * IPolicyConfig interface of CPolicyConfigClient, IID
 * {f8679f50-850a-41cf-9c72-430f290290c8}, whose 11th method after IUnknown
 * is SetDefaultEndpoint(PCWSTR, ERole). Microsoft can change or remove it in
 * any release. So:
 *   - it is used only on the Windows builds it is known to work on, 10240
 *     (Windows 10 1507) to 26999 (Windows 11 25H2 and its updates); outside
 *     that it is off unless AutoSwitchAnyBuild = 1 is set;
 *   - the interface is asked for by that IID, so a Windows without it fails
 *     the CoCreateInstance and the feature stays off;
 *   - the call itself is under SEH: a fault turns the feature off for the
 *     rest of the session instead of taking rk-monitor down;
 *   - nothing else depends on it: the tray, popup and widget work the same.
 *
 * Settings (HKCU, else HKLM, \Software\RK3576\Monitor):
 *   AutoSwitchHeadphones  REG_DWORD  1 on (default), 0 off; the tray menu
 *                                    item (labelled "zi dong qie huan er ji")
 *   AutoSwitchAnyBuild    REG_DWORD  1: also on builds outside the range
 *   AutoSwitchPrevious    REG_SZ     (HKCU, written by rk-monitor) the
 *                                    device to go back to on unplug
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "audioswitch.h"

#define SET_KEY         L"Software\\RK3576\\Monitor"
#define PREV_VALUE      L"AutoSwitchPrevious"
#define MATCH_ADAPTER   L"Rockchip"
#define MIN_BUILD       10240
#define MAX_BUILD       26999
#define RECHECK_MS      1000

/* Endpoint property keys, as functiondiscoverykeys_devpkey.h and
 * mmdeviceapi.h define them (local copies: no INITGUID games). */
static const PROPERTYKEY kFormFactor = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };
static const PROPERTYKEY kInterfaceName = {
    { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
static const PROPERTYKEY kFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
#define FORM_FACTOR_HEADPHONES 3

/* The undocumented interface. Only the slot of SetDefaultEndpoint matters;
 * the others are declared to keep it in place and are never called. */
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

/* CPolicyConfigClient */
static const CLSID kClsidPolicyConfig = {
    0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

static HWND g_hwnd;
static UINT g_msg;
static BOOL g_enabled;
static BOOL g_comInit;
static BOOL g_broken;               /* SetDefaultEndpoint faulted: off for the session */
static int g_available = -1;        /* -1 not checked yet */
static DWORD g_build;
static int g_last = -1;             /* headphones last seen: -1 unknown, 0 unplugged, 1 plugged */
static volatile LONG g_pending;
static IMMDeviceEnumerator *g_enum;
static HRESULT g_lastHr = S_OK;     /* of the last SetDefaultEndpoint */
static wchar_t g_restore[512];      /* the device restored on the last unplug */
static UINT_PTR g_recheck;

/* ------------------------------------------------------------ settings */

static DWORD SettingDword(const wchar_t *name, DWORD dflt)
{
    DWORD v, sz = sizeof(v);

    if (RegGetValueW(HKEY_CURRENT_USER, SET_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    sz = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, SET_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return v;
    return dflt;
}

static void PreviousSave(const wchar_t *id)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SET_KEY, PREV_VALUE, REG_SZ, id,
                    (DWORD)((wcslen(id) + 1) * sizeof(wchar_t)));
}

static BOOL PreviousLoad(wchar_t *out, DWORD cap)
{
    DWORD sz = cap * sizeof(wchar_t);

    out[0] = 0;
    return RegGetValueW(HKEY_CURRENT_USER, SET_KEY, PREV_VALUE, RRF_RT_REG_SZ, NULL, out, &sz) == ERROR_SUCCESS &&
           out[0] != 0;
}

static void PreviousClear(void)
{
    HKEY k;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, SET_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, PREV_VALUE);
        RegCloseKey(k);
    }
}

/* ------------------------------------------------------------ Windows build, IPolicyConfig */

typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);

static DWORD WindowsBuild(void)
{
    RTL_OSVERSIONINFOW vi;
    RtlGetVersionFn fn;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    if (g_build)
        return g_build;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    fn = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (fn && fn(&vi) == 0)
        g_build = vi.dwBuildNumber;
    return g_build;
}

static BOOL BuildSupported(void)
{
    DWORD b = WindowsBuild();

    return (b >= MIN_BUILD && b <= MAX_BUILD) || SettingDword(L"AutoSwitchAnyBuild", 0) != 0;
}

static IPolicyConfig *PolicyConfig(void)
{
    IPolicyConfig *pc = NULL;

    if (FAILED(CoCreateInstance(kClsidPolicyConfig, NULL, CLSCTX_ALL, __uuidof(IPolicyConfig), (void **)&pc)))
        return NULL;
    return pc;
}

extern "C" BOOL AudioSwitchAvailable(void)
{
    if (g_broken)
        return FALSE;
    if (g_available < 0) {
        IPolicyConfig *pc;

        g_available = 0;
        if (BuildSupported() && (pc = PolicyConfig()) != NULL) {
            g_available = 1;
            pc->Release();
        }
    }
    return g_available == 1;
}

/* No C++ objects here, so SEH can guard the call. */
static HRESULT CallSetDefault(IPolicyConfig *pc, PCWSTR id, ERole role)
{
    __try {
        return pc->SetDefaultEndpoint(id, role);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_broken = TRUE;
        return E_UNEXPECTED;
    }
}

static HRESULT SetDefaultAll(const wchar_t *id)
{
    static const ERole roles[] = { eConsole, eMultimedia, eCommunications };
    IPolicyConfig *pc;
    HRESULT hr = S_OK;

    if (!AudioSwitchAvailable())
        return E_NOTIMPL;
    pc = PolicyConfig();
    if (!pc)
        return REGDB_E_CLASSNOTREG;
    for (int i = 0; i < 3 && !g_broken; i++) {
        HRESULT r = CallSetDefault(pc, id, roles[i]);
        if (FAILED(r))
            hr = r;
    }
    pc->Release();
    g_lastHr = hr;
    return hr;
}

/* ------------------------------------------------------------ endpoints */

static BOOL PropString(IPropertyStore *ps, const PROPERTYKEY &key, wchar_t *out, size_t cap)
{
    PROPVARIANT v;
    BOOL ok = FALSE;

    out[0] = 0;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
        wcsncpy_s(out, cap, v.pwszVal, _TRUNCATE);
        ok = TRUE;
    }
    PropVariantClear(&v);
    return ok;
}

static BOOL PropUint(IPropertyStore *ps, const PROPERTYKEY &key, UINT *out)
{
    PROPVARIANT v;
    BOOL ok = FALSE;

    *out = 0;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_UI4) {
        *out = v.ulVal;
        ok = TRUE;
    }
    PropVariantClear(&v);
    return ok;
}

static BOOL ContainsNoCase(const wchar_t *hay, const wchar_t *needle)
{
    size_t n = wcslen(needle);

    for (; *hay; hay++)
        if (_wcsnicmp(hay, needle, n) == 0)
            return TRUE;
    return FALSE;
}

/* The board's headphone endpoint: id (CoTaskMemFree it), state, name. */
static BOOL FindHeadphones(wchar_t **id, DWORD *state, wchar_t *name, size_t cap)
{
    IMMDeviceCollection *col = NULL;
    UINT n = 0;
    BOOL found = FALSE;

    *id = NULL;
    *state = 0;
    if (name && cap)
        name[0] = 0;
    if (!g_enum ||
        FAILED(g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &col)))
        return FALSE;
    col->GetCount(&n);
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *dev = NULL;
        IPropertyStore *ps = NULL;
        wchar_t adapter[256];
        UINT ff;

        if (FAILED(col->Item(i, &dev)))
            continue;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            if (PropUint(ps, kFormFactor, &ff) && ff == FORM_FACTOR_HEADPHONES &&
                PropString(ps, kInterfaceName, adapter, 256) && ContainsNoCase(adapter, MATCH_ADAPTER) &&
                SUCCEEDED(dev->GetState(state)) && SUCCEEDED(dev->GetId(id))) {
                if (name && cap)
                    PropString(ps, kFriendlyName, name, cap);
                found = TRUE;
            }
            ps->Release();
        }
        dev->Release();
    }
    col->Release();
    return found;
}

static wchar_t *DefaultRender(void)
{
    IMMDevice *dev = NULL;
    wchar_t *id = NULL;

    if (g_enum && SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        dev->GetId(&id);
        dev->Release();
    }
    return id;
}

static BOOL DeviceActive(const wchar_t *id)
{
    IMMDevice *dev = NULL;
    DWORD state = 0;

    if (!g_enum || FAILED(g_enum->GetDevice(id, &dev)))
        return FALSE;
    dev->GetState(&state);
    dev->Release();
    return state == DEVICE_STATE_ACTIVE;
}

static BOOL DeviceName(const wchar_t *id, wchar_t *out, size_t cap)
{
    IMMDevice *dev = NULL;
    IPropertyStore *ps = NULL;
    BOOL ok = FALSE;

    out[0] = 0;
    if (!g_enum || !id || FAILED(g_enum->GetDevice(id, &dev)))
        return FALSE;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        ok = PropString(ps, kFriendlyName, out, cap);
        ps->Release();
    }
    dev->Release();
    return ok;
}

/* ------------------------------------------------------------ notifications */

class Notifier : public IMMNotificationClient {
    LONG m_ref;

    static void Kick(void)
    {
        if (g_hwnd && InterlockedExchange(&g_pending, 1) == 0)
            PostMessageW(g_hwnd, g_msg, 0, 0);
    }

public:
    Notifier() : m_ref(1) {}
    virtual ~Notifier() {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0)
            delete this;
        return (ULONG)r;
    }

    /* Called on a system thread: post, do nothing else here. */
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) { Kick(); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) { Kick(); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) { Kick(); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) { return S_OK; }
};

static Notifier *g_notifier;

/* ------------------------------------------------------------ the switch */

/* Once more a second after an unplug: Windows picks its own default when the
 * headphones go, and that may land after the restore. */
static void CALLBACK RecheckProc(HWND, UINT, UINT_PTR id, DWORD)
{
    wchar_t *cur;

    KillTimer(NULL, id);
    g_recheck = 0;
    if (!g_restore[0] || g_last != 0 || !g_enabled)
        return;
    cur = DefaultRender();
    if ((!cur || wcscmp(cur, g_restore) != 0) && DeviceActive(g_restore))
        SetDefaultAll(g_restore);
    CoTaskMemFree(cur);
    g_restore[0] = 0;
}

extern "C" void AudioSwitchPoll(void)
{
    wchar_t *hp = NULL, *cur = NULL;
    DWORD state = 0;
    int plugged, last;

    InterlockedExchange(&g_pending, 0);
    if (!FindHeadphones(&hp, &state, NULL, 0))
        return;
    plugged = state == DEVICE_STATE_ACTIVE;
    last = g_last;
    g_last = plugged;
    if (plugged == last || !g_enabled || !AudioSwitchAvailable())
        goto out;

    if (plugged) {
        /* plugged in (or plugged at start): headphones become the default */
        cur = DefaultRender();
        if (cur && wcscmp(cur, hp) == 0)
            goto out;
        if (cur)
            PreviousSave(cur);
        SetDefaultAll(hp);
    } else if (last == 1) {
        /* unplugged: back to what was the default before */
        wchar_t prev[512];

        if (PreviousLoad(prev, 512) && DeviceActive(prev)) {
            SetDefaultAll(prev);
            wcsncpy_s(g_restore, 512, prev, _TRUNCATE);
            if (!g_recheck)
                g_recheck = SetTimer(NULL, 0, RECHECK_MS, RecheckProc);
        }
        PreviousClear();
    } else {
        /* unplugged at start: whatever was remembered is stale */
        PreviousClear();
    }
out:
    CoTaskMemFree(cur);
    CoTaskMemFree(hp);
}

extern "C" void AudioSwitchSetEnabled(BOOL on)
{
    g_enabled = on;
    if (on) {
        /* as at start: with headphones in, switch to them now */
        g_last = -1;
        AudioSwitchPoll();
    }
}

extern "C" BOOL AudioSwitchStart(HWND hwnd, UINT msg, BOOL enabled)
{
    HRESULT hr;

    g_hwnd = hwnd;
    g_msg = msg;
    g_enabled = enabled;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return FALSE;
    g_comInit = TRUE;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void **)&g_enum))) {
        g_enum = NULL;
        return FALSE;
    }
    g_notifier = new (std::nothrow) Notifier();
    if (g_notifier && FAILED(g_enum->RegisterEndpointNotificationCallback(g_notifier))) {
        g_notifier->Release();
        g_notifier = NULL;
    }
    AudioSwitchPoll();
    return TRUE;
}

extern "C" void AudioSwitchStop(void)
{
    if (g_recheck) {
        KillTimer(NULL, g_recheck);
        g_recheck = 0;
    }
    if (g_enum && g_notifier)
        g_enum->UnregisterEndpointNotificationCallback(g_notifier);
    if (g_notifier) {
        g_notifier->Release();
        g_notifier = NULL;
    }
    if (g_enum) {
        g_enum->Release();
        g_enum = NULL;
    }
    g_hwnd = NULL;
    if (g_comInit) {
        CoUninitialize();
        g_comInit = FALSE;
    }
}

extern "C" void AudioSwitchDescribe(BOOL enabled, wchar_t *out, size_t cap)
{
    BOOL own = FALSE;
    wchar_t *hp = NULL, *cur = NULL;
    wchar_t hpName[128], curName[128];
    DWORD state = 0;
    const wchar_t *hpState;

    if (!g_enum) {
        if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
            own = TRUE;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), (void **)&g_enum)))
                g_enum = NULL;
        }
    }

    if (FindHeadphones(&hp, &state, hpName, 128))
        hpState = state == DEVICE_STATE_ACTIVE ? L"plugged" : L"unplugged";
    else
        hpState = L"no Rockchip headphone endpoint";
    cur = DefaultRender();
    if (!cur || !DeviceName(cur, curName, 128))
        wcscpy_s(curName, 128, cur ? L"(unnamed)" : L"(none)");

    swprintf(out, cap,
             L"audio switch: %ls; Windows build %lu (%ls), IPolicyConfig %ls; headphones: %ls%ls%ls; "
             L"default playback: %ls; last switch 0x%08lx",
             enabled ? L"on" : L"off",
             (unsigned long)WindowsBuild(),
             BuildSupported() ? L"supported" : L"outside the tested range",
             AudioSwitchAvailable() ? L"available" : L"unavailable",
             hpState, hpName[0] && hp ? L", " : L"", hp ? hpName : L"",
             curName, (unsigned long)g_lastHr);

    CoTaskMemFree(cur);
    CoTaskMemFree(hp);
    if (own) {
        if (g_enum) {
            g_enum->Release();
            g_enum = NULL;
        }
        CoUninitialize();
    }
}
