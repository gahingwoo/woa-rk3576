# Windows' own Wi-Fi (WiFiCx 1.0) for our Wi-Fi drivers

English | [简体中文](wificx.zh-CN.md)

Status, 2026-11-25: `aic8800usb` (ROCK 4D, AIC8800D80 on USB) has a WiFiCx
mode on branch `aic8800-wificx`: Windows' network list, its WPA2 supplicant
and its profiles, with the driver doing scan, association and key install.
**Nothing here has run on a board.** The core's side is tested on the host
against the simulated chip with the test playing Windows' supplicant
(`tools/aictest`); the WiFiCx front end is built by CI against the WDK and
has never met a real WiFiCx. The Ethernet-style mode
([aic8800.md](aic8800.md)) stays, and the INF that is installed picks the
mode. `bcmwlsdio` (TaishanPi 3M, CM5-IO) does not have this yet; the plan is
at the end.

## Contents

- [The question and the answer](#the-question-and-the-answer)
- [WiFiCx 1.0 on build 22621](#wificx-10-on-build-22621)
- [What WiFiCx expects of a driver](#what-wificx-expects-of-a-driver)
- [Who does WPA2](#who-does-wpa2)
- [The data path](#the-data-path)
- [aic8800usb in WiFiCx mode](#aic8800usb-in-wificx-mode)
- [INF, mode and switching](#inf-mode-and-switching)
- [Registry values](#registry-values)
- [Tests](#tests)
- [Board test plan](#board-test-plan)
- [Risks and open questions](#risks-and-open-questions)
- [bcmwlsdio next](#bcmwlsdio-next)
- [Sources](#sources)

## The question and the answer

Until now our Wi-Fi drivers show Windows a wired-looking adapter, and a
script puts the SSID and passphrase into the registry for the driver's own
supplicant. Windows' Wi-Fi interface (Settings, the taskbar list, connect by
clicking, the password dialog, `netsh wlan`) needs a WLAN miniport, which on
Windows 11 means a WiFiCx client driver.

Feasible on 23H2, with limits:

- Windows 11 22H2/23H2 (build 22621/22631) has **WiFiCx 1.0 only**, with
  NetAdapterCx 2.2. A driver built for 1.0 runs there; the 1.1/1.2 features
  (MLO, the newer capability fields, wake on action frames) are out.
- **Windows runs the WPA2 4-way handshake itself.** The EAPOL frames go
  through the data path as ordinary data frames; Windows hands the keys
  down (`WDI_SET_ADD_CIPHER_KEYS`). So in WiFiCx mode our own supplicant is
  not used. The AIC8800D80 firmware already works this way (it hands
  EAPOL to the host and takes keys with `MM_KEY_ADD`), so nothing in the
  firmware has to change.
- The data path carries **native 802.11 data frames** both ways, not
  Ethernet frames; the driver converts.
- Windows keeps the network list itself; the driver only reports what it
  hears during a scan.

## WiFiCx 1.0 on build 22621

| | Windows 11 22H2/23H2 (22621/22631) | 24H2 (26100) |
|---|---|---|
| KMDF | 1.33 | 1.33 |
| NetAdapterCx | 2.2 for a WiFiCx driver (Microsoft: "has to be forced to 2.2") | 2.2+ |
| WiFiCx | **1.0** | 1.0, 1.1, 1.2 |
| `wificx.sys` | 10.0.22621.2215 in our 22621.2428 image | |

From Microsoft's "Building a WiFiCx driver for multiple versions of
Windows". A 1.1/1.2 driver would need `WIFI_MINIMUM_VERSION_REQUIRED=0` and
run-time checks around every newer call; building against 1.0 directly
avoids both. That is what `aic8800usb` does:

```xml
<KMDF_VERSION_MAJOR>1</KMDF_VERSION_MAJOR><KMDF_VERSION_MINOR>33</KMDF_VERSION_MINOR>
<NetAdapterDriver>true</NetAdapterDriver>
<NETADAPTER_VERSION_MAJOR>2</NETADAPTER_VERSION_MAJOR><NETADAPTER_VERSION_MINOR>2</NETADAPTER_VERSION_MINOR>
<WifiDriver>true</WifiDriver>
<WIFI_VERSION_MAJOR>1</WIFI_VERSION_MAJOR><WIFI_VERSION_MINOR>0</WIFI_VERSION_MINOR>
```

**What the CI's WDK (10.0.26100.0) has.** `km/wificx/1.0/` (`wificx.h`,
`wificxtypes.h`, `wififuncenum.h`: 39 functions), also 1.1 and 1.2;
`km/wlan/2.0/` with Microsoft's WDI TLV generator and parser
(`TlvGeneratorParser.hpp`, `TlvGenerated_.hpp`, `WiFiCxModel.xml`) and its
library `km/<arch>/wlan/2.0/WificxTLVGenParse.lib`; the WDI message IDs and
types in `km/dot11wificxintf.h` and `km/dot11wificxtypes.hpp`. The library
is C++ and needs `operator new(size_t, ULONG_PTR)` and `operator delete`
from the driver, `libcntpr.lib` and `/Zc:sizedDealloc-`.

**Is the 26100 WDK's 1.0 header the one 22621 expects?** WiFiCx checks
the `Size` of each capability structure. The 22621 WDK's
`km/wificx/1.0/wificx.h` (third-party mirror
WindowsDatabase/WdkCollection) has the same fields in the same order as the
26100 copy, including the multi-link fields at the end of
`WIFI_STATION_CAPABILITIES`; the only difference is a rename
(`HostWPA3FIPSModeEnabled` to `..._Deprecated`). The 22621 header has no
`*_INIT` functions, so the driver sets `Size = sizeof(...)` itself. (The
21H2 header, 22000, is shorter: three fields fewer in the station
capabilities. 21H2 is not a target.)

**The WDI version.** The TLVs are versioned (fields added at WDI 1.1.4,
2.0.5, ...). The driver declares the version it was built for
(`WDI_VERSION_LATEST`, 2.0.12 in this WDK) and parses and generates TLVs at
the version the OS reports (`WifiDeviceGetOsWdiVersion`), as Microsoft's
sample and rtw89-win do. Which WDI version 22621 reports is not known yet;
the driver records it (`OsWdiVersion`).

**No public 1.0 sample.** Microsoft's sample
(Windows-driver-samples `network/wlan/wificx`, MS-PL) is WiFiCx 1.2 on
NetAdapterCx 2.5/2.6 with simulated hardware; it uses 1.1+ fields and does
not handle keys. The closest real driver is rtw89-win (RTL8852BE on an
RK3588, BSD-3-Clause, WiFiCx 1.1, build 26100), whose
`docs/en/observed-behavior.md` records how WiFiCx actually behaves; the
IDs below (SCAN-3, CONN-5, ...) refer to it. No code is copied from either.

## What WiFiCx expects of a driver

**Set-up.**

1. `EvtDriverDeviceAdd`: `NetDeviceInitConfig`, then `WifiDeviceInitConfig`,
   then `WdfDeviceCreate`; then `WIFI_DEVICE_CONFIG_INIT` (WDI version,
   `EvtWifiDeviceSendCommand`, `EvtWifiDeviceCreateAdapter`,
   `EvtWifiDeviceCreateWifiDirectDevice`) and `WifiDeviceInitialize`.
2. `EvtDevicePrepareHardware`: the capabilities, with
   `WifiDeviceSetDeviceCapabilities`, `...StationCapabilities`,
   `...BandCapabilities`, `...PhyCapabilities` (WiFiCx copies them).
   WiFiCx 1.0 never sends `WDI_TASK_OPEN`, `WDI_GET_ADAPTER_CAPABILITIES`
   or `WDI_TASK_CREATE_PORT`.
3. WiFiCx calls `EvtWifiDeviceCreateAdapter` for the station port: the
   driver does `NetAdapterCreate`, `WifiAdapterInitialize`, its
   NetAdapterCx capabilities, `NetAdapterStart`. WiFiCx reports the link
   state of that adapter itself (CONN-6).

**Commands.** Each `WIFIREQUEST` holds a `WDI_MESSAGE_HEADER` (port,
status, transaction ID, IHV ID) and TLVs (type and length, 16 bits each,
little endian). `EvtWifiDeviceSendCommand` runs at up to DISPATCH_LEVEL, so
the driver queues the request and handles it on its own thread, one at a
time, completing each exactly once (INIT-3).

- A **property** (`WDI_SET_*`, `WDI_GET_*`) is done with its completion,
  `WifiRequestComplete` (M3).
- A **task** (`WDI_TASK_*`) gets its M3 when accepted, then its completion
  indication (M4, `WifiDeviceReceiveIndication` with the same transaction
  ID), always in that order. A failed M3 has no M4.
- **Unsolicited indications** (BSS list, association result, link state,
  disassociation) have transaction ID 0.

**What Windows sends a station** (rtw89-win SCAN-1, CONN-1, on 26100):

| When | Messages |
|---|---|
| WLAN service start | `SET_ADAPTER_CONFIGURATION` (85), `TASK_DOT11_RESET` (7), `GET_STATISTICS` (34), `TASK_SCAN` (4) aborted by `ABORT_TASK` (75), `SET_LOCATION_PRIVACY` (138), `TASK_SCAN` |
| idle | `TASK_SCAN` every 60 s |
| connect, WPA2-Personal | `TASK_DOT11_RESET`, `GET_STATISTICS`, `SET_PRIVACY_EXEMPTION_LIST` (28, EtherType 0x888E), `TASK_CONNECT` (6); after association `SET_ADD_CIPHER_KEYS` (29) pairwise, then group |
| disconnect | `TASK_DISCONNECT` (8), `TASK_DOT11_RESET` |
| radio switch | `TASK_SET_RADIO_STATE` (11) |

**Scan.** Every beacon and probe response heard goes up in
`WDI_INDICATION_BSS_ENTRY_LIST` (BSSID, signal, channel and band, the frame
body from the Timestamp on, no 802.11 header: SCAN-3), then
`WDI_INDICATION_SCAN_COMPLETE`, also after an abort
(`NDIS_STATUS_REQUEST_ABORTED`, SCAN-5). With `BSSListCachemanagement`
FALSE Windows keeps the list and never asks for it (SCAN-2). `ABORT_TASK`
names the task by its bare message ID, not the OID form (SCAN-4).

**Connect.** `TASK_CONNECT` carries the connection parameters (SSID,
authentication and cipher lists, allowed and disallowed BSSIDs, where the
broadcast address means "any": CONN-2) and the candidate BSSs with their
beacon or probe response bodies. The driver reports
`WDI_INDICATION_ASSOCIATION_RESULT` for what it tried; on success it
carries the association request body (Windows takes our RSN element from
it for message 2), the response body, the AP's beacon body (checked
against message 3), `PortAuthorized` FALSE (CONN-5); then
`WDI_INDICATION_LINK_STATE_CHANGE` and `WDI_INDICATION_CONNECT_COMPLETE`.
When the AP goes away: `WDI_INDICATION_DISASSOCIATION`; Wlansvc then
reconnects with `RoamRequest` set (CONN-7).

## Who does WPA2

Windows. For a WPA2-Personal network it runs the 4-way and group key
handshakes over the data path (EAPOL frames as 802.11 data frames, EtherType
0x888E, exempt from encryption by the privacy exemption list) and installs
the keys with `WDI_SET_ADD_CIPHER_KEYS`: the pairwise key about 0.4 s after
association with no key ID, then the group key with the AP's key ID
(rtw89-win CONN-1, on hardware). WPA3-SAE runs on the host too, through
`SAE_AUTH_PARAMS_NEEDED` and `SET_SAE_AUTH_PARAMS`. There is no capability
for a firmware 4-way handshake in WiFiCx 1.0; `HostFIPSModeImplemented`
stays 0.

So the WiFiCx mode drops our supplicant: `aic_wpa.c` stays in the binary
for the Ethernet-style mode and is idle in WiFiCx mode. The AIC8800D80
firmware needs nothing new: with `CONTROL_PORT_HOST` it already passes
EAPOL up and keeps the port closed to other frames until the host opens it
(`ME_SET_CONTROL_PORT`), and it takes keys with `MM_KEY_ADD`, which is what
Linux's wpa_supplicant path does.

One race to watch: Windows sends message 4 and hands the pairwise key down
right after; if the key reaches the firmware before message 4 has left, the
AP gets message 4 encrypted and drops it. The driver waits for the USB data
queue to drain (up to 50 ms, then 10 ms more) before `MM_KEY_ADD`.

## The data path

WiFiCx moves **native 802.11 data frames** through NetAdapterCx's queues
(`NetPacketLayer2TypeIeee80211`; the INF says `*MediaType` 16,
NdisMediumNative802_11), and Windows converts to and from Ethernet above
(rtw89-win, on hardware). One TX queue and one RX queue are enough; TX
demux is optional and `WifiAdapterAddPeer` without it bugchecks (HS-8).

The AIC8800D80 firmware takes Ethernet payloads and builds the 802.11
header itself, and hands up decrypted 802.11 frames that the core already
turns into Ethernet (header, security header and LLC/SNAP stripped,
A-MSDUs split). So the WiFiCx data path is two small conversions around the
existing one:

- **RX**: `aic_eth_to_80211` gives each Ethernet frame a 24-byte data
  header with From DS set (A1 the destination, A2 the AP, A3 the source)
  and an RFC 1042 header (802.1H for AppleTalk ARP and IPX), one MSDU per
  buffer, as rtw89-win does.
- **TX**: `aic_80211_to_eth` reads Windows' To DS frame (QoS and +HTC
  headers allowed, protected frames refused) back into an Ethernet frame
  for `aic_build_tx`.

Frames move from association on (EAPOL first). The association is reported
to Windows after the firmware's `SM_CONNECT_IND`, but message 1 of the
handshake can arrive in the same USB transfer, so received frames wait in
the driver's FIFO until `ASSOCIATION_RESULT` has gone up (`RxHold`).

## aic8800usb in WiFiCx mode

```
  Settings / taskbar / netsh wlan (in-box: wlansvc, nwifi)
  WiFiCx 1.0 (wificx.sys)  +  NetAdapterCx 2.2
  ------------------------------------------------------------
  aic8800usb.sys (service aic8800usb, Mode 1)
    wificx.cpp   WDI commands: queue, dispatch on the worker thread,
                 capabilities, the station adapter, indications
                 (WificxTLVGenParse.lib)
    netadapter.c the same TX/RX queues, 802.11 frames in this mode
    driver.c     PnP, Mode, the worker thread (runs AicWifiRun)
    usb.c        bulk pipes, continuous readers, ops->scan_bss
    -- portable core (host-tested) --
    aic_wlan.c   aic_scan_start, aic_connect_start, aic_bss_from_body,
                 aic_assoc_req_body/rsp_body, aic_host_set_key, events
    aic_data.c   aic_eth_to_80211, aic_80211_to_eth; EAPOL up as data
    (aic_wpa.c   not used in this mode)
```

| WDI message | What the driver does |
|---|---|
| `TASK_SET_RADIO_STATE` | off: ends scan and connection; `RADIO_STATUS` if it changed; M4 |
| `TASK_DOT11_RESET` | ends scan and connect tasks, leaves the network (`SM_DISCONNECT`), M4 |
| `TASK_SCAN` | `SCANU_START` (every channel, wildcard SSID); each result goes to a buffer through `ops->scan_bss`; at `SCANU_START_CFM`: `BSS_ENTRY_LIST` (16 entries each), `SCAN_COMPLETE`. Waits for a firmware scan still running |
| `ABORT_TASK` | scan: what was heard, `SCAN_COMPLETE` aborted at once (the firmware finishes on its own); connect: `SM_DISCONNECT`, `CONNECT_COMPLETE` aborted |
| `TASK_CONNECT` | first candidate that is open, or RSN with PSK and CCMP, rebuilt from the body Windows hands back (`aic_bss_from_body`); `SM_CONNECT` with our RSN element and `CONTROL_PORT_HOST`; at `SM_CONNECT_IND`: `ASSOCIATION_RESULT`, `LINK_STATE_CHANGE` (65 Mbit/s, HT20), `CONNECT_COMPLETE`; failure or timeout (`ConnectTimeoutMs`): failed `ASSOCIATION_RESULT`, `CONNECT_COMPLETE` failed. Nothing usable: M3 `STATUS_NOT_SUPPORTED` |
| `TASK_DISCONNECT` | `SM_DISCONNECT`, `DISASSOCIATION` (by host), `DISCONNECT_COMPLETE` |
| `SET_ADD_CIPHER_KEYS` | CCMP only: pairwise (after the TX queue drains) `MM_KEY_ADD` and `ME_SET_CONTROL_PORT` open; group `MM_KEY_ADD` with its key ID |
| `SET_ADAPTER_CONFIGURATION`, `SET_PRIVACY_EXEMPTION_LIST`, `SET_DELETE_CIPHER_KEYS`, `SET_DEFAULT_KEY_ID`, `SET_CONNECTION_QUALITY`, `SET_RECEIVE_PACKET_FILTER`, `SET_MULTICAST_LIST`, `SET_FLUSH_BSS_ENTRY` | accepted, nothing to do (the firmware gates EAPOL itself and filters by address; keys go with the station entry) |
| anything else (`GET_STATISTICS`, `SET_LOCATION_PRIVACY`, P2P, AP, ...) | `STATUS_NOT_SUPPORTED` (fine for a station: SCAN-1) |
| firmware `SM_DISCONNECT_IND` | `DISASSOCIATION` (peer deauthenticated, or unable to maintain); Windows reconnects |

**Capabilities reported**: radio on, firmware version, 1x1, Bluetooth
coexistence by the firmware; scan SSID list 1, privacy exemption list 8,
no WEP, no MFP, no auto power save (power save is off), the OS keeps the
BSS list; open/none and RSNA-PSK/CCMP for unicast and multicast data,
open/none for management; 2.4 GHz channels 1 to 13 (HR/DSSS, ERP, HT) and
5 GHz 36 to 165 when the chip has it (OFDM, HT), 20 MHz only; legacy rates
per PHY. No Wi-Fi Direct capabilities, so no Wi-Fi Direct device and no
mobile hotspot; `EvtWifiDeviceCreateWifiDirectDevice` refuses.

Not in this first version: hidden networks (the scan uses the wildcard
SSID only), WPA3, PMF, TKIP groups, 40/80 MHz and VHT/HE, roaming between
APs of one network by the driver, power save, `GET_STATISTICS`, wake on
WLAN.

## INF, mode and switching

`aic8800wifi.inf` is `aic8800usb.inf` with the Wi-Fi network keywords:

| | `aic8800usb.inf` (Mode 0) | `aic8800wifi.inf` (Mode 1) |
|---|---|---|
| `*IfType` | 6 (Ethernet) | 71 (IEEE 802.11) |
| `*MediaType` | 0 (802.3) | 16 (native 802.11) |
| `*PhysicalMediaType` | 14 (802.3) | 9 (native 802.11) |
| `Characteristics` | 0x04 | 0x84 (as Microsoft's sample) |
| `LowerRange` | `ethernet` | `wlan,ethernet` |
| `NumberOfNetworkInterfaces` | | 1 (station only; the class installer reserves this many adapters, rtw89-win INST-1) |
| Device Parameters `Mode` | 0 | 1 |

No `Include`/`Needs` line is needed (none of Microsoft's pages or sample
INFs has one); `KmdfService` as for any KMDF driver. Both INFs bind
`USB\VID_A69C&PID_8D81&MI_02`, install the same service and binary, and
write `Mode` without NOCLOBBER, because Device Parameters outlives a driver
change. The driver reads `Mode` in `EvtDriverDeviceAdd` (it decides whether
WiFiCx is initialised before the device exists) and records `ActiveMode`.

Only one of the two may be in the driver store (they rank the same).
`aic-bringup.ps1 -Mode WiFiCx` removes `aic8800usb.inf`'s package from the
device and the store, stages `aic8800wifi.inf` and raises `Stage` to 4
(the WiFiCx mode needs the firmware init, Stage 2 or more); restart Windows.
`-Mode Ethernet` goes back. A full uninstall and reinstall changes the
interface GUID, and Windows' profiles go with it (INST-2): expect to type
the passphrase again after a switch.

## Registry values

In addition to [those of the Ethernet-style mode](aic8800.md#registry-values),
in the runtime device's Device Parameters:

| Value | Meaning |
|---|---|
| `Mode` | written by the INF: 0 Ethernet-style, 1 WiFiCx |
| `ActiveMode` | the mode the driver started in |
| `WifiInitStatus`, `OsWdiVersion`, `DriverWdiVersion` | `WifiDeviceInitialize`; the WDI version the OS speaks and the one declared |
| `WifiCapsStatus`, `WifiDeviceCapsStatus`, `WifiStationCapsStatus`, `WifiBandCapsStatus`, `WifiPhyCapsStatus` | each capability call (0 = success) |
| `AdapterStartStatus`, `WdiPortId` | the station adapter |
| `WdiTrace` | the first 64 WDI message IDs received, in order |
| `WdiMessages`, `WdiUnsupported`, `WdiRefused`, `WdiIndications`, `WdiIndicationErrors` | counters |
| `WdiScanTasks`, `WdiBssIndicated`, `WdiConnectTasks`, `WdiLastConnectStatus`, `WdiKeys`, `WdiKeyErrors`, `WdiLastKeyStatus`, `WdiDisassociations`, `WdiRadioOn`, `WdiConnected` | what happened |

Setting (service key): `WdiVersion` (DWORD, default 0 = the WDK's
`WDI_VERSION_LATEST`) overrides the version the driver declares, in case
22621's WiFiCx refuses a newer number. `ConnectTimeoutMs` bounds a connect
task. `Ssid`, `Passphrase` and `Pmk` are not used in this mode.

## Tests

`tools/aictest` (CI job `aictest`, also under ASan/UBSan) plays Windows
against the simulated chip and AP:

- a scan without blocking: every result reaches `scan_bss`, bodies start at
  the Timestamp (beacon interval at 8, capability at 10, SSID at 12);
- the network list emptied, then rebuilt from the body Windows would hand
  back (`aic_bss_from_body`), found by BSSID;
- connect without a passphrase; the association event; message 1 left to
  the OS; the association request body carries SSID and our RSN element,
  the response body status 0 and the AID;
- the test's own supplicant (PTK from PRF-384, MIC, AES key unwrap) runs
  the 4-way handshake over 802.11 frames built and read with the same
  conversions as the driver, using only what the driver reports: message 2
  carries the RSN element from the reported association request (the AP
  checks it), message 3's RSN element equals the beacon's; then
  `aic_host_set_key` pairwise (the port opens) and group;
- ARP to the gateway as 802.11 frames both ways; the AP's deauthentication
  gives the disconnect event;
- the conversions alone: full-size frames, 802.1H for IPX, QoS and +HTC
  headers, protected and management frames refused, LLC frames without
  SNAP, short frames.

Not covered on the host: anything of WiFiCx itself (the TLVs come from
Microsoft's library at run time). The Windows job builds `wificx.cpp`
against the WDK's WiFiCx 1.0 and TLV headers and links the library.

## Board test plan

ROCK 4D, Windows 11 23H2, test signing on, Ethernet cable for access, CI
artifact `aic8800-bringup-signed-arm64` from branch `aic8800-wificx`. Do the
Ethernet-style stages of [aic8800.md](aic8800.md#board-test-plan) first, up
to stage 2 at least (the firmware download and init must work before
WiFiCx can). Turn location services on for the session (`netsh wlan show
networks` needs them, SCAN-7). Keep a serial log of the firmware boot.

1. **Switch.** `aic-bringup.ps1 -Mode WiFiCx`, restart. Pass: the device is
   "AIC8800D80 USB Wi-Fi (WiFiCx)" without a problem code; `-Status` shows
   `ActiveMode` 1, `WifiInitStatus` 0, every `Wifi*CapsStatus` 0,
   `AdapterStartStatus` 0; note `OsWdiVersion`. `Get-NetAdapter` lists it
   as "Wi-Fi" (media type native 802.11); `netsh wlan show interfaces`
   lists it, and `netsh wlan show drivers` names the driver with "Radio
   types supported" 802.11n and "WPA2-Personal CCMP" among the
   authentication methods. Fail cases to note: Code 10 or 31 (the event
   log and `WifiCapsStatus`; a `Size` refusal shows as a non-zero
   `WifiStationCapsStatus`); no WLAN interface although the adapter starts
   (then try `NumberOfNetworkInterfaces` 3 as rtw89-win has it).
2. **Scan.** Open the taskbar Wi-Fi list. Pass: the home network and the
   neighbours appear with sensible signal bars within 10 s, and again
   after a refresh; `WdiTrace` starts like SCAN-1 (85 7 34 4 75 ...);
   `WdiScanTasks` grows about once a minute when idle, `WdiBssIndicated`
   with it. Compare with Armbian's `iw dev wlan0 scan`.
3. **Connect, WPA2-Personal.** Click the home network, type the passphrase.
   Pass: "Connected, secured" within 15 s; `WdiConnected` 1, `WdiKeys` 2,
   `WdiKeyErrors` 0, `LinkState` 3; `ipconfig` shows a DHCP address;
   `curl https://example.com` works with the cable unplugged; a 100 MB
   download completes (record the speed). n = 5 connects over 5 cold boots,
   all must pass, and Windows reconnects by itself at each boot (the
   profile). A wrong passphrase: Windows says so (no driver change).
4. **Open network** (a phone hotspot without a password): connects, DHCP.
5. **Disconnect and reconnect**: "Disconnect" in the list, then connect
   again; turn the router off and on (Windows reconnects by itself:
   `WdiDisassociations` grows, then `WdiConnected` 1 again); airplane mode
   on and off (`WdiRadioOn`).
6. **Back to Ethernet-style**: `aic-bringup.ps1 -Mode Ethernet`, restart,
   stage 4 works as before.

If the handshake never completes (Windows: "Can't connect"; `WdiKeys` 0):
check `RxEapol` (message 1 reached the driver) and `NetRxFrames` (and
Windows), `NetTxFrames` (message 2 left); a wrong RSN element in message 2
shows as the AP never sending message 3. If the pairwise key goes in but
traffic does not flow, suspect the message 4 race (the AP log shows message
4 missing): raise the delay in `SetAddCipherKeys`.

## Risks and open questions

- **WDI version.** 22621's `OsWdiVersion` is unknown until the board says;
  the TLVs follow it. If `WifiDeviceInitialize` fails, try
  `Parameters\WdiVersion` = 0x10100 (WDI 1.1.0) or the reported value.
- **Capability sizes and values.** The structures are the 22621 layout,
  but 22621's validation of individual fields is unknown (rtw89-win: bad
  combinations are dropped silently, HS-2: some Wi-Fi Direct values make
  nwifi refuse the whole adapter; we report no Wi-Fi Direct at all).
- **`NumberOfNetworkInterfaces` 1** is untested (rtw89-win uses 3 with
  Wi-Fi Direct).
- **Hidden networks** are not found (wildcard SSID only); `TASK_SCAN`'s
  SSID list is not passed to the firmware yet.
- **Message 4 race** (above).
- **Link speed and signal** are fixed estimates (65 Mbit/s, the RSSI of
  the scan); the firmware's rate and signal reports are not read yet.
- **The firmware's own scan while connected** may cost the link; Windows
  scans every minute or so when idle only, more when the list is open.

## bcmwlsdio next

The same front end fits `bcmwlsdio` (BCM43456 on the TaishanPi 3M, BCM43752
on the CM5-IO; branches `tspi3m-wifi-driver` and `cm5io-wifi-bt`) once its
core offers the same few calls; the WDI half (`wificx.cpp`) would move
almost as it is, with the core calls renamed. What the Broadcom core needs
(following OpenBSD bwfm, as the rest of it does):

- scan: `escan` results carry a `wl_bss_info` with the elements; build the
  frame body for Windows from it (8 zero bytes of Timestamp, beacon
  interval, capability, then the elements);
- connect: `wsec`/`wpa_auth` for WPA2-PSK, `sup_wpa` 0 so the firmware
  leaves the handshake to the host (FullMAC firmware can do it itself
  with `sup_wpa` 1 and a PMK, which WiFiCx has no use for), then `join`;
  the association elements from `assoc_req_ies`/`assoc_resp_ies`;
- EAPOL as data (the firmware passes 0x888E up when its supplicant is
  off); keys with `wsec_key`;
- data: Broadcom's firmware also speaks Ethernet (BDC header), so the same
  two 802.11 conversions apply.

## Sources

- Microsoft: [WiFiCx overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/wifi-wdf-class-extension-wificx),
  [Writing a WiFiCx client driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver),
  [Building a WiFiCx driver for multiple versions of Windows](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/building-a-wificx-driver-for-multiple-versions-of-windows),
  [WiFiCx message structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/wificx-message-structure),
  [OID_WDI_TASK_CONNECT](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/oid-wdi-task-connect),
  [OID_WDI_TASK_SCAN](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/oid-wdi-task-scan),
  [OID_WDI_SET_ADD_CIPHER_KEYS](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/oid-wdi-set-add-cipher-keys),
  [INF files for NetAdapterCx client drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/inf-files-for-netadaptercx-client-drivers).
- The WDK 10.0.26100.0 headers named above; the 22621 WDK's
  `km/wificx/1.0/wificx.h` from the WindowsDatabase/WdkCollection mirror,
  compared field by field.
- [Microsoft's WiFiCx sample](https://github.com/microsoft/Windows-driver-samples/tree/main/network/wlan/wificx) (MS-PL).
- [rtw89-win](https://github.com/Meari-Prototype/rtw89-win) (BSD-3-Clause),
  `docs/en/observed-behavior.md`: WiFiCx on hardware (build 26100).
