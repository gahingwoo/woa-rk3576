# Wi-Fi on Windows 10 (WDI)

Windows 10 2004..22H2 (build 19041..19045) has no WiFiCx, so the Windows 11
drivers (`bcmwlwifi.inf`, `aic8800usb`) cannot load there. `bcmwlwdi.sys` is
the Broadcom SDIO driver (TaishanPi 3M BCM43456, CM5-IO BCM43752) as a **WDI
miniport**, built from the same sources.

Status: builds and is test-signed in CI (`wifi-win10-signed-arm64`). Not run
on a board yet.

## Why WDI

- WiFiCx is WDI messages in a WDF wrapper: scan, connect, keys, radio, reset
  and abort are the same `WDI_MESSAGE_HEADER` + TLVs in both. The handlers
  move unchanged into `wdicore.cpp` and serve both drivers.
- A native 802.11 (Native Wi-Fi) miniport would mean ~100 `OID_DOT11_*`
  handlers written from scratch, shared with nothing.

## Layers

| Layer | Windows 11 (`bcmwlsdio.sys`) | Windows 10 (`bcmwlwdi.sys`) |
|---|---|---|
| chip, SDIO, firmware, fullmac commands | `bwl_*.c` | same |
| SD bus, files, bring-up, worker | `sdbus.c files.c pool.c bringup.c` | same |
| WDI messages (scan, connect, keys...) | `wdicore.cpp`, TLV lib `wlan\2.0` | same file, TLV lib `wlan\1.0` |
| OS glue | `driver.c` (KMDF FDO), `wificx.cpp` | `wdi.cpp` (NDIS 6.50 WDI miniport, KMDF 1.31 miniport mode) |
| data path | `netadapter.c` (NetAdapterCx rings) | `wdital.c` (WDI TAL) |

Only WDI has: open/close of the adapter, `WDI_GET_ADAPTER_CAPABILITIES` (one
TLV reply), `WDI_TASK_CREATE_PORT` / `DELETE_PORT`, and the TAL (WDI pulls TX
frames with `TxDequeueIndication`, RX goes up with `RxInorderDataIndication`
and `RxGetMpdus`). The INF binds `SD\VID_02D0&PID_xxxx&FN_1` and has an empty
model section for build 22000+, so Windows 11 keeps the WiFiCx driver.

## What works, what is not there

Implemented: open (bring-up on the worker, then `OpenAdapterComplete`),
capabilities, station port, scan, connect (open and WPA2-PSK/CCMP, Windows'
supplicant over the data path, or the firmware's with `FwSupplicant=1`),
keys, disconnect, radio on/off, dot11 reset, abort, link state, TX/RX.

Not there: Wi-Fi Direct, hotspot, WPA3, PMF, power save, wake on LAN,
statistics (`WDI_GET_STATISTICS` answers not supported), AIC8800 on Windows
10 (the same split fits `aic8800usb`; not done).

## Risks to check on the board

1. Hardware ID: assumes sdbus names function 1 `SD\VID_02D0&PID_A9BF&FN_1`
   (the instance paths seen on Windows 11 say so). If it does not bind, read
   `pnputil /enum-devices /class Net /ids` and adjust the INF.
2. Windows 10's sdbus letting the F1 device address F2 (proven on 22621 only).
3. The WDI version: `WdiVersion` under `Services\bcmwlwdi\Parameters`
   overrides what the driver declares (default `WDI_VERSION_LATEST`).
4. RX frames all go to one group peer, TX in port-queueing mode, as
   Microsoft's SDIO WDI sample does for a station.
5. Unaligned 8-byte accesses fault on this SoC under 19041 when the memory is
   not normal cacheable. This driver allocates only pool memory and does no
   DMA of its own; WDI's own `NDIS_WDI_DATA_API` is a packed struct, copied
   into pool memory before use.

## Install (Windows 10, test signing on)

```
certutil -addstore Root signer.cer
certutil -addstore TrustedPublisher signer.cer
powershell -ExecutionPolicy Bypass -File install-firmware.ps1
pnputil /add-driver bcmwlwdi\bcmwlwdi.inf /install
```

Use `install-firmware.ps1 -Chip 43752` on the CM5-IO. The SDIO host must run
the `dwcmshc` from the same package.

## Board test

Records are under the device's `Device Parameters` key
(`HKLM\SYSTEM\CurrentControlSet\Enum\SD\VID_02D0&PID_A9BF&FN_1\*\Device Parameters`),
and before the device exists under `Services\bcmwlwdi\Parameters`
(`PreEntry`, `RegisterStatus`, `MiniportCreateStatus`, `DevKeyStatus`).

1. Bind: `pnputil /enum-devices /connected /class Net` shows the adapter;
   `PreEntry` and `RegisterStatus` = 0 in `Services\bcmwlwdi\Parameters`.
2. Open: `LastStep` reaches `wdi: open complete`, `FwState` = `station up`,
   `WdiCapsStatus` = 0, `LastStep` later `wdi: station port created`.
   A failure leaves `FwState` = `failed: <step>`.
3. Scan: `netsh wlan show networks` lists networks; `WdiBssIndicated` > 0.
4. Connect to a WPA2-PSK network from Settings; `WdiLastConnectStatus` = 0,
   `KeyEapolWaitMs` well below 200, `ipconfig` gets an address, ping the
   gateway. `TalTxSends`, `TalRxIndications` grow.
5. Disconnect, reconnect, airplane mode on/off, reboot with Wi-Fi on.
6. Same on the CM5-IO (BCM43752).

Before the first install keep a way back (rescue Windows, or the SDIO Wi-Fi
switch in the firmware setup off) and turn on `AutoReboot=0` so a bugcheck
stays on screen.
