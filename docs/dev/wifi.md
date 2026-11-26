# Wi-Fi driver for the TaishanPi 3M (BCM43456 over SDIO): design

Status, 2026-11-26: W1 to W3 are on woa-rk3576 `main` and passed on the
TaishanPi 3M (BCM43456 rev 9, firmware 7.45.96.2 running, CLM loaded, no
SDIO errors). W4, the Wi-Fi adapter for Windows (WiFiCx 1.0, Stage 3), is
on branch `bcmwlsdio-wificx`: tested on the host against a simulated chip
and access points (`tools/bwltest`), built by CI, **not run on a board**.

[docs/WIFI.md](../WIFI.md) has the hardware, the firmware option and the
W1/W2 board tests. This page is the plan from W3 on: the driver model, the
stages, how each one is accepted on the board, licences and firmware files.

## Contents

- [The choice of driver model](#the-choice-of-driver-model)
- [Architecture](#architecture)
- [SDIO from a Windows function driver](#sdio-from-a-windows-function-driver)
- [Stages](#stages)
- [Registry values (W3)](#registry-values-w3)
- [Licences](#licences)
- [Firmware files](#firmware-files)
- [Tests](#tests)
- [Risks and open questions](#risks-and-open-questions)
- [Other boards: ROCK 4D, Dragon Q6A, CM5-IO](#other-boards-rock-4d-dragon-q6a-cm5-io)

## The choice of driver model

Windows has had three Wi-Fi driver models:

| Model | Shape | State on Windows 11 |
|---|---|---|
| Native Wi-Fi (NDIS 6.x 802.11 miniport) | the driver exposes 802.11 OIDs; Windows' Native Wi-Fi does the rest | deprecated since Windows 10; Windows 11 still loads old drivers, but new ones should not use it |
| WDI (WLAN Device Driver Interface) | an NDIS miniport under Microsoft's WDI "upper edge" (wdiwifi.sys). Windows sends TLV-encoded commands and *tasks* (scan, connect, roam), the driver answers with TLV indications | the Windows 10 model; still loads on Windows 11, but Microsoft moved new development to WiFiCx |
| WiFiCx (Wi-Fi class extension) | a NetAdapterCx client (KMDF). The control path is the same WDI command and task set, carried as `WIFIREQUEST` objects; the data path is NetAdapterCx rings | the current model, Windows 11 and later; headers in WDK 26100 (`wificx/`, `dot11wificxintf.h`) |

All of them assume the device firmware does most of the 802.11 work. The
BCM43456 is FullMAC: its firmware scans, authenticates, associates, roams,
and with `sup_wpa=1` also runs the WPA2 4-way handshake. That matches the
WDI task model well: a WDI *scan task* is one `escan`, a *connect task* is
`wsec`/`wpa_auth`/`sup_wpa`/`C_SET_SSID` plus waiting for `E_SET_SSID` and
`E_LINK`. The parts that need thought are where Windows expects to do the
work itself: by default Windows runs its own supplicant (EAPOL frames go
over the data path and the driver installs the keys), and it wants BSS
lists with the raw IEs. Both are available from the firmware (`sup_wpa=0`,
`wsec_key`; escan results carry the IEs).

**Decision.** WiFiCx is the end point (now W4), because it is what Windows 11
drivers are written to, and because it builds on NetAdapterCx, which this
repository already uses for `dwc_eqos`. What the request called "WDI
integration" is done there: WiFiCx carries WDI's commands, so W4 implements
the WDI command and task set, through WiFiCx rather than through a WDI NDIS
miniport.

**No Ethernet-style step.** The plan had one (W5) before WiFiCx; it was
dropped on 2026-11-26 because aic8800usb's WiFiCx 1.0 front end already
existed and fits this core: W4 below goes straight to WiFiCx.

Why not WDI first: it means an NDIS miniport (not KMDF NetAdapter), a TLV
parser and generator, and Microsoft's IHV-only WDI documentation set, all
before a single frame moves. Why not Native Wi-Fi: deprecated, and the work
is not reusable.

## Architecture

```
  Windows Wi-Fi UI, netsh wlan, WlanSvc
  WiFiCx (wificx.sys) / NetAdapterCx                (W4, Stage 3)
  -----------------------------------------------
  bcmwlsdio.sys (KMDF)
    driver.c      PnP, power, bring-up work item, registry breadcrumbs
    sdbus.c       CMD52/CMD53, properties, card interrupt (W4)
    files.c       firmware files from \SystemRoot\System32\drivers\brcm
    -- portable core, no Windows dependency (built for tools/bwltest too) --
    bwl_sdio.c    function 0/1 registers, backplane window, chip RAM, F2 FIFO, clock
    bwl_chip.c    chip ID, EROM scan, ARM CR4 halt/release, RAM size, save/restore
    bwl_bringup.c attach, firmware + NVRAM download, start (HT, F2, mailbox)
    bwl_fw.c      NVRAM conversion, CLM chunks, version strings
    bwl_proto.c   SDPCM frames, BCDC commands and data header, firmware events
    bwl_ctl.c     control channel (polled in W3), iovars, clmload
    bwl_wlan.c    station: escan, join, keys, events, rate and RSSI (W4)
    bwl_data.c    BSS records, elements, 802.11 <-> Ethernet, frame FIFO (W4)
  wificx.cpp, netadapter.c  WDI commands, NetAdapterCx queues (W4)
  -----------------------------------------------
  sdbus.sys   (inbox) SDIO card init, CIS, one PDO per card or function
  sdport.sys  (inbox)
  dwcmshc.sys (ours, W2) RK3576 dw_mmc as the SDIO host, card interrupt
  ACPI \_SB.SDIO  RKCPFE2D, only with "SDIO Wi-Fi (experimental)" on (W1)
```

The portable core is the larger part and the part most likely to have
bugs, so it is kept free of Windows types: the same files compile into the
driver (MSVC, kernel) and into `tools/bwltest` (gcc, Linux), where CI runs
them against a simulated chip. The kernel files are thin.

## SDIO from a Windows function driver

The documented interface is `ntddsd.h`: `SdBusOpenInterface` on the PDO
that sdbus creates for the card, `InitializeInterface` with the callback
for card interrupts, then `SdBusSubmitRequest` (synchronous, below
DISPATCH_LEVEL) or `SdBusSubmitRequestAsync` with an `SDBUS_REQUEST_PACKET`:

- `SDRF_DEVICE_COMMAND` with `SDCMD_IO_RW_DIRECT` (CMD52, response R5) or
  `SDCMD_IO_RW_EXTENDED` (CMD53, with an MDL and a length).
  `SDTT_SINGLE_BLOCK` is byte mode, `SDTT_MULTI_BLOCK_NO_CMD12` block mode.
  The argument word (function, address, block mode, incrementing address,
  count) is built by the driver.
- `SDRF_GET_PROPERTY` / `SDRF_SET_PROPERTY`: `SDP_FUNCTION_NUMBER`,
  `SDP_FUNCTION_BLOCK_LENGTH`, `SDP_BUS_WIDTH`, `SDP_BUS_CLOCK`,
  `SDP_FUNCTION_INT_ENABLE`, and others.

A Broadcom chip needs three functions: F0 (CCCR, FBR, Broadcom's vendor
bytes 0xF0 to 0xF2), F1 (backplane access and the chip's SDIO registers)
and F2 (the frame FIFO). How sdbus maps that onto devices is the main
unknown, and W3 is written to find out rather than assume:

1. **One PDO or one per function.** The W2 board test lists the SD devices.
   The INF binds both IDs seen so far; an instance whose
   `SDP_FUNCTION_NUMBER` is not 1 records itself and stays idle.
2. **Access to other functions.** The driver puts the function number into
   every CMD52/CMD53 argument itself. Whether sdbus passes F0 and F2
   numbers through from F1's PDO decides whether one instance can drive the
   chip. W3 reads the CCCR through F0 first (`Cccr`, `Cmd52Errors`), and
   needs F2 for the version query, so a W3 pass answers both.
3. **IOEx.** The PDO is opened with `SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE`;
   the driver turns F1 and F2 on itself, F2 only after the firmware runs.
4. **Block size of F2.** For a block-mode CMD53, sdbus programs the host's
   block size from the function's block length, and the F1 PDO's is 64,
   while F2 uses 512. So F2 stays in byte mode (at most 512 bytes per
   command) until the board shows otherwise (`F2BlockMode` = 1). F1 RAM
   writes use block mode with 64-byte blocks, `MaxBlocks` (32) per command.
5. **Interrupts (W4).** The card interrupt comes through dwcmshc (W2 passes
   RINTSTS bit 24 to sdport) to sdbus, which calls the function driver's
   callback with the interrupt masked; the driver clears the cause on the
   chip and calls `AcknowledgeInterrupt`. W3 does not use it and polls.

If 2 fails (sdbus refuses other function numbers), the fallback is to bind
both function PDOs and pass F2 traffic between the two instances through a
private interface. That is more code, not a different design.

## Stages

Each stage is one or two board sessions with a pass/fail reading. Every one
is off by default: the firmware option is off, and the driver's `Stage`
value is 0.

### W3: SDIO access, chip identification, firmware download (code done)

Code: `drivers/net/bcmwlsdio` (KMDF, Class System, no network interface)
and `tools/bwltest`.

What it does, by `Stage`:

| Stage | |
|---|---|
| 0 | open SDBUS, record function number, bus width, clock, block lengths. No card access |
| 1 | + CCCR read; F1 and F2 block sizes; F1 on; ALP clock; chip ID at 0x18000000; EROM core scan; ARM CR4 halted, 802.11 core held in reset; TCM size; KSO; WLAN reset bit; PMU resource reload; F2 off. (bwfm's attach) |
| 2 | + load the three files; NVRAM text converted; firmware to RAM base (0x198000), NVRAM with its length token to the top of RAM, both read back; reset vector to address 0; CPU released; HT clock; mailbox protocol version 4; F2 on and ready; host interrupt mask, watermark 8, save/restore if the chip has it; then over the control channel `C_GET_VERSION`, `cur_etheraddr`, `clmload` + `clmload_status`, `ver`; the firmware console ring |

Acceptance on the board (W3 passes when all hold):

- Stage 0: `SdOpenStatus` 0, `FunctionNumber` 1 on one instance,
  `BusWidth` 4.
- Stage 1: `FwState` = `identified`; `ChipId` 0x4345 (17221), `ChipRev` 9;
  `Cores` lists 800 (chipcommon), 812 (802.11), 829 (SDIO), 83e (ARM CR4)
  among others; `RamSize` non-zero (0xC8000, 819200, is expected but not
  confirmed); `Cmd52Errors` and `Cmd53Errors` 0.
- Stage 2: `FwState` = `running`; `FwVersion` is the firmware's own
  string, e.g. `wl0: ... version 7.45.96 ...`, and `FwVersionNumber`
  matches the file's `FwFileVersion` (7.45.96.0 for the Armbian files);
  `ClmStatus` 0; `MacAddress` set; `FwConsole` has the firmware's start-up
  log.

Board steps: see [Board session for W2 and W3](#board-session-for-w2-and-w3).

### W4: the Wi-Fi adapter (WiFiCx 1.0), Stage 3 (code done)

This replaces the earlier W4 to W6 plan (an Ethernet-style adapter first):
Windows 11 23H2 has WiFiCx 1.0, and aic8800usb already had a WiFiCx 1.0
front end (`docs/dev/wificx.md` on branch `aic8800-wificx`), so bcmwlsdio
uses the same one.

| Stage | |
|---|---|
| 0 to 2 | as above (W3) |
| 3 | + `event_msgs`, `mpc` 0, PM off, `roam_off` 1, infrastructure, optional `Country`, `C_UP`; then a WiFiCx device: escan for scans, `C_SET_SSID` (BSSID and channel) for open and WPA2-PSK/CCMP networks, keys with `wsec_key`, data frames over SDPCM channel 2 (BCDC), firmware events for scan results and link up/down, link rate and RSSI every 5 s |

- Needs `bcmwlwifi.inf` (Class Net) and function 1. Function 2 and 3
  devices bind the same INF and stay idle; under `bcmwlsdio.inf` Stage 3
  runs as 2. Stage is read when the device starts.
- Both INFs set Stage 0 on a new install and keep an existing value:
  installing the Wi-Fi INF does nothing until Stage is raised by hand,
  because the SDIO path is new on the board and a bad start costs a
  rescue boot.
- WPA2: Windows' supplicant by default (`sup_wpa` 0, EAPOL as data frames,
  keys from `SET_ADD_CIPHER_KEYS`). WiFiCx never gives the driver the
  passphrase, so the firmware's supplicant (`sup_wpa` 1, `C_SET_WSEC_PMK`,
  `E_PSK_SUP`, as brcmfmac's FWSUP) is a test path only: `FwSupplicant` 1
  plus `Passphrase` (and optionally `FwSupSsid`) in the registry, reported
  to Windows with the port already authorized. `FwSupCapable` says whether
  the firmware lists `idsup`.
- Polled at PASSIVE_LEVEL by one worker thread (every `PollMs` 2 ms while
  frames move, `IdlePollMs` 10 ms otherwise); nothing at DIRQL. RX
  superframes are switched off (`bus:rxglom` 0) and not taken apart.
- Not in it: hidden networks, WPA3, PMF, TKIP, roaming offload, power
  save, wake on WLAN, `GET_STATISTICS`.

Board steps: see [Board test for W4](#board-test-for-w4).

## Registry values (W3)

All under the device's `Device Parameters`
(`HKLM\SYSTEM\CurrentControlSet\Enum\SD\VID_02D0&PID_....\<instance>\Device Parameters`).

Settings (from the INF; the driver reads them at start):

| Value | Default | |
|---|---|---|
| `Stage` | 0 | 0, 1, 2: see above |
| `MaxBlocks` | 32 | 64-byte blocks per block-mode CMD53 on F1 |
| `F2BlockMode` | 0 | 1: allow block mode on F2 |
| `Verify` | 1 | read RAM back after the download |
| `FirmwareDir` | `\SystemRoot\System32\drivers\brcm` | where the three files are |

Written by the driver:

| Value | |
|---|---|
| `LastStep`, `PrevStep`, `StepSeq` | the bring-up step, flushed at each step |
| `FwState` | `stage 0: ...`, `identified`, `downloaded`, `started`, `running`, or `failed: <step> (<error>)` |
| `LastError`, `ErrorDetail` | the core's error code (-1 I/O, -2 timeout, -3 not the expected chip, -4 bad file or frame, -5 no space, -6 verify, -7 firmware error, -8 state, -9 no credit) and a step-specific word (an address, a register value) |
| `SdOpenStatus`, `FunctionNumber`, `FunctionNumberStatus`, `BusDriverVersion`, `BusWidth`, `BusClock`, `HostBlockLength`, `FunctionBlockLength`, `SetBlockLengthStatus` | what sdbus reports |
| `Cccr` | CCCR bytes 0..3 packed: revision, SD revision, IOEx, IORx; `CccrCaps` byte 8 |
| `ChipIdReg`, `ChipId`, `ChipRev`, `Cores`, `RamBase`, `RamSize`, `PmuRev`, `SrCapable`, `SdioCoreRev` | the chip |
| `FwFileSize`, `NvramFileSize`, `ClmFileSize`, `NvramLength`, `FwFileVersion`, `FileStatus`, `FileMissing` | the files |
| `SharedAddr`, `ConsoleAddr`, `FwConsole` | the firmware's shared block and console log (last 2 KB) |
| `BcdcIoType`, `MacAddress`, `ClmChunks`, `ClmStatus`, `ClmResult`, `FwVersion`, `FwVersionNumber`, `FwCmdStatus` | control channel results |
| `Cmd52Count`, `Cmd53Count`, `Cmd52Errors`, `Cmd53Errors`, `LastSdStatus`, `LastSdArg`, `LastR5Flags` | bus counters |
| `FramesTx`, `FramesRx`, `EventsRx`, `LastEvent`, `RxErrors`, `RxAborts`, `GlomRx`, `MailboxData`, `IntStatus`, `Polls` | frame counters |
| `Cmd53OutsideBounce` | CMD53s refused because their buffer was not inside the bounce buffer (should be 0) |
| `BounceGuardOk` | 1: nothing was written past the bounce buffer during the bring-up |
| `PoolGuardErrors`, `PoolGuardBuffer`, `PoolGuardWhere`, `PoolGuardSize`, `PoolGuardOffset` | a driver buffer written past its end (pool.c); absent if none |

Stage 3 adds (written by the driver): `ActiveMode` (1: Wi-Fi adapter),
`NetClass`, `AddFunction`, `FunctionSource` (0 none, 1 instance ID, 2
hardware IDs, 3 compatible IDs), `WlanInitResult`, `IoType`, `Has5GHz`,
`FwSupCapable`, `WifiInitStatus`, `OsWdiVersion`, `Wifi*CapsStatus`,
`AdapterStartStatus`, the `Wdi*` counters and `WdiTrace` as in
aic8800usb's WiFiCx mode, `LinkState` (0 down, 1 joining, 2
associated, 3 keys in), `ConnStatus`, `ConnEvent`, `DisconnectReason`,
`LinkRateKbps`, `LinkRssi`, `RxData`, `RxEapol`, `TxData`, `TxEapol`,
`FwKeys`, `FlowHold`, `NetTx*`, `NetRx*`, `ServiceErrors`. Settings:
`PollMs`, `IdlePollMs`, `ConnectTimeoutMs`, `Country`, `FwSupplicant`,
`Passphrase`, `FwSupSsid`, `WdiVersion`.

## Board test for W4

TaishanPi 3M, Windows 11 23H2, test signing, Ethernet for access,
firmware with SDIO Wi-Fi on, W3 passing (Stage 2 on function 1). CI
artifact `wifi-bringup-signed-arm64` from branch `bcmwlsdio-wificx`.

1. `pnputil /add-driver bcmwlsdio\bcmwlwifi.inf /install`, then delete the
   old bring-up package (`pnputil /enum-drivers`, `pnputil /delete-driver
   oemNN.inf` for the old `bcmwlsdio.inf`) so only one is staged.
2. On the function 1 device: `reg add "HKLM\SYSTEM\CurrentControlSet\Enum\<FN_1 instance>\Device Parameters" /v Stage /t REG_DWORD /d 3 /f`,
   restart Windows.
3. Pass: one "Broadcom BCM43456 Wi-Fi (SDIO, WiFiCx)" without a problem
   code; `ActiveMode` 1, `FwState` `station up`, `WifiInitStatus` and every
   `Wifi*CapsStatus` 0, `AdapterStartStatus` 0; `netsh wlan show drivers`
   lists it. Function 2 and 3: `ActiveMode` 0, `FwState` `idle: not
   function 1` (or `stage 0`).
4. Taskbar Wi-Fi list: networks appear (`WdiScanTasks`, `WdiBssIndicated`).
5. Join a WPA2-PSK network, then an open one (a phone hotspot): connected,
   DHCP address, `ping` the gateway, a 100 MB download (note the speed);
   `WdiKeys` 2, `LinkState` 3, `netsh wlan show interfaces` shows rate and
   signal. Disconnect and reconnect; router off and on.
6. Optional: the firmware's supplicant: `FwSupplicant` 1, `Passphrase`
   (REG_SZ) on the function 1 key, restart, join that network.
7. CM5-IO (BCM43752): the same with `install-firmware.ps1 -Chip 43752`.

The binary binds NetAdapterCx 2.1 and WiFiCx 1.0 (the KMDF stub does it
before DriverEntry, under either INF). A Windows build must register at
least those: `reg query HKLM\SYSTEM\CurrentControlSet\Control\Wdf\Kmdf\NetAdapterCx\Versions /s`
and `...\WifiCx\Versions /s` (the TaishanPi 3M's 22631 lists 2.1 and
1.1). A 2.2 build bugchecked there before DriverEntry.

Reading a failure: `PreEntry` (same key as below) is written by the
image's entry point before any WDF code, with the versions it binds;
`PreEntryStatus` is 0xFFFFFFFF until the KMDF stub returns. `PreEntry`
without `EarlyTrace` means the stub failed (a class extension bind);
neither means the image never ran. For a bind failure also look at the
System event log (Wdf01000 and WdfLdr sources), and with the kernel
debugger attached set `DbgPrintOn` 1 under `Control\Wdf\Kmdf\Diagnostics`
for the loader's messages, or `VerifierOn` 1 and `VerboseOn` 1 under
`Services\bcmwlsdio\Parameters\Wdf` for KMDF's verifier. Before the device
exists (DriverEntry, DeviceAdd)
nothing can go to Device Parameters, so the driver appends each early step
to `EarlyTrace` in `HKLM\SYSTEM\CurrentControlSet\Services\bcmwlsdio\Parameters`,
flushed (one line per step, every instance; started afresh at each boot).
After that, `FwState` and `LastStep` name the step; no networks
with `ScanResults` 0 means escan gave nothing (`WlanFwStatus`); "can't
connect" with `RxEapol` 0 means message 1 never came, `TxEapol` 1 and no
message 3 points at the RSN element or the message 2 path, `ConnStatus`
holds the firmware's join status. Back out: Stage 2 (or 0), restart.

## Board session for W2 and W3

Prerequisites: firmware from edk2-rk3576 `main` (cbbb844 or later), which
has the SDIO Wi-Fi option. Keep the UEFI variables when reflashing (back up
nvdata first). Full Windows on the eMMC with test signing on.

1. **Firmware (W1).** Setup → ACPI / Device Tree → *SDIO Wi-Fi
   (experimental)* → Enabled; save, reset. On the serial console look for
   `TSPI-3M Wi-Fi: ... probe PASS` and write down the `CIS vendor ...
   device ...` line. That device ID is the one the INF must match.
2. **Packages.** From this branch's CI run, artifact
   `wifi-bringup-signed-arm64`: `dwcmshc\`, `bcmwlsdio\`, `signer.cer`,
   `install-firmware.ps1`. Copy it to the board. Trust the signer:
   `certutil -addstore -f Root signer.cer` and
   `certutil -addstore -f TrustedPublisher signer.cer`.
3. **dwcmshc (W2).** `pnputil /add-driver dwcmshc\dwcmshc.inf`, then a clean
   restart. Check W2 as docs/WIFI.md says: `ACPI\RKCPFE2D\1` started, and a
   new `SD\VID_02D0&PID_....` device with code 28. Record its hardware and
   compatible IDs (`pnputil /enum-devices /connected /ids`). The SD card slot
   must still work.
4. **bcmwlsdio, Stage 0.** If the hardware ID is in the INF:
   `pnputil /add-driver bcmwlsdio\bcmwlsdio.inf /install` (safe here: the
   device has no driver yet, code 28). Read the values (command in the
   driver README). Expect `SdOpenStatus` 0, `FunctionNumber`, `BusWidth`.
   If the ID is different, report it; the INF needs a line, not a guess.
5. **Stage 1.** `reg add "HKLM\SYSTEM\CurrentControlSet\Enum\<instance>\Device Parameters" /v Stage /t REG_DWORD /d 1 /f`,
   restart Windows, read the values: `FwState`, `ChipId`, `Cores`,
   `RamSize`, the error counters.
6. **Stage 2.** `powershell -ExecutionPolicy Bypass -File install-firmware.ps1`
   (downloads the Armbian files; the board needs the internet through
   Ethernet), set `Stage` 2, restart, read the values: `FwState` running,
   `FwVersion`, `ClmStatus`, `FwConsole`.
7. Clean up: delete any staged package that did not bind
   (`pnputil /enum-drivers`, `pnputil /delete-driver oemNN.inf`).

Reading a failure: `FwState` names the step; `Cmd52Errors`/`Cmd53Errors`
with `LastSdStatus` and `LastSdArg` say which command sdbus refused (an
`LastSdArg` with function bits 30:28 = 0 or 2 refused points at question 2
above); `ErrorDetail` is the address or register. Backing out: set `Stage`
0, or turn the firmware option off (Windows then sees no SDIO controller),
or `pnputil /delete-driver <oem>.inf /uninstall`.

## BAD_POOL_CALLER at device removal (0xC2/0x99)

Seen 2026-11-26 when PnP removed the bcmwlsdio devices to switch drivers.
With symbols for ntoskrnl, Wdf01000 and sdbus 22621 the stack reads:
`PnpProcessQueryRemoveAndEject` > `IopRemoveDevice` > KMDF
`FxPkgFdo::ProcessRemoveDeviceOverload` > `FxPkgPnp::CleanupStateMachines`
> `FxPkgFdo::ReleaseReenumerationInterface` > sdbus
`SdbusReenumerationInterfaceDereference` (tail call) > `ExFreePoolWithTag`.
The block being freed is sdbus's own GUID_REENUMERATE_SELF_INTERFACE
context (0x18 bytes, non-paged, tag `SdIc`), which sdbus allocates when
KMDF starts the FDO and frees when KMDF releases the interface at
IRP_MN_REMOVE_DEVICE. `ExFreePoolWithTag` raises 0x99 only for an address
below kernel space or not 16-byte aligned; the address was `...2c8`, and
sdbus had just found its signature there, so the block itself was handed
out misaligned or its pointer was moved: pool damage that predates the
free. No bcmwlsdio
buffer or object is freed on that path. FN_1's teardown crumb (`stopped: CPU halted`) was not
in the log, so the instance going away was most likely FN_2 or FN_3,
which never touch the card (that driver logged a crumb only after a
registry write; the driver now logs first and tags every line `FN<n>`).

To catch the writer (run elevated, then restart; the KD listener reports
0xC1/0xC4/0xD6 at the faulting instruction):

    verifier /flags 0x809 /driver bcmwlsdio.sys sdbus.sys dwcmshc.sys

0x1 special pool, 0x8 pool tracking, 0x800 miscellaneous checks; on
bcmwlsdio.sys this also turns on the KMDF verifier. A second round adds
0x80 (DMA checks) and sdport.sys: `verifier /flags 0x889 /driver
bcmwlsdio.sys sdbus.sys dwcmshc.sys sdport.sys`. sdport.sys also carries
the eMMC boot disk, so keep the rescue Windows at hand; there, undo it
offline by deleting `VerifyDrivers` and `VerifyDriverLevel` under
`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management`
of the loaded main hive. Online: `verifier /reset`, restart.

Repro without a new package: `pnputil /disable-device "<instance>"` on one
of the SD function devices (`SD\VID_02D0&PID_A9BF&FN_n\...`). That removes
only bcmwlsdio's FDO above sdbus's child PDO; the SD host (dwcmshc, sdport)
and sdbus stay up, so this is not the host teardown the SD rules forbid.
A disabled device is not started at the next boot, so a crash does not
loop; `pnputil /enable-device` brings it back. Order: first all three at
Stage 0 and disable FN_2 (no card access at all: a crash here clears the
bring-up code), then FN_1 at Stage 2 after `firmware running`.

## Licences

The repository is GPL-2.0-only (`LICENSE`).

| Source | Licence | Use here |
|---|---|---|
| OpenBSD `bwfm` (`sys/dev/ic/bwfm*.{c,h}`, `sys/dev/sdmmc/if_bwfm_sdio.{c,h}`), Broadcom and Patrick Wildt | ISC | code followed closely in `bwl_*.c`; notice kept (`bwl_regs.h`, named in each file). ISC permits this inside a GPL-2.0 work |
| Linux brcmfmac (`drivers/net/wireless/broadcom/brcm80211/brcmfmac`), Broadcom | **ISC**, every file (`SPDX-License-Identifier: ISC`), not GPL | may be used as code as well as a reference; used so far for checks and the RX abort sequence. The Linux kernel APIs it calls are not ported |
| Linux `mmc` core, `dw_mmc` | GPL-2.0 | behaviour only (W2), as before |
| Windows WDK headers and `sdbus.lib` | WDK licence | linked like every driver here |
| Microsoft driver samples (MS-PL) | MS-PL, incompatible with GPL-2.0 | not used |

So the earlier worry, that brcmfmac being in Linux makes it GPL and
reference-only, does not hold: the brcmfmac files are ISC, like bwfm.

## Firmware files

| File | Armbian (default) | Raspberry Pi (option) |
|---|---|---|
| `brcmfmac43456-sdio.bin` | 482 927 bytes, 7.45.96.0 (2017-06-16, AMPAK/Rockchip build `43455c5-roml/43455_sdio-...-ag-...`), SHA-256 `3167956a...2cc67c` | 593 869 bytes, 7.45.96.252 (2026-11-17, with `extsae`), SHA-256 `dc4429f1...c739fb` |
| `brcmfmac43456-sdio.clm_blob` | 7 163 bytes, SHA-256 `2dbd7d22...25e8a` | identical |
| `brcmfmac43456-sdio.txt` (NVRAM) | 2 099 bytes, `AP6256_NVRAM_V1.1_08252017`, SHA-256 `66c71eb5...cfd9e`; used with both | (Pi 400's: different RF board, not used) |
| Source | armbian/firmware `db8cc456`, `brcm/` | RPi-Distro/firmware-nonfree `b1833f18`, `debian/added-firmware/brcm/` |
| Licence | none stated in the repository | Synaptics binary licence (`debian/copyright`): redistribution in object code only, for use with Synaptics chips, no modification |

Linux on this board loads `brcmfmac43456-sdio.bin/.txt/.clm_blob` (no
board-specific `brcmfmac43456-sdio.lckfb,tspi-3m-rk3576.*` exists), the
Armbian files. linux-firmware itself has no 43456 files.

**Not shipped with the driver.** The Armbian files carry no licence, so
they cannot be redistributed. The Raspberry Pi files could be: the Synaptics
licence allows object-code redistribution for use with their chips, and a
firmware file next to a GPL driver is aggregation, not a combined work.
But the licence is a click-through style EULA, "non-transferable", and
forbids modification, so this repository keeps firmware out of it as it
does for Bluetooth. `install-firmware.ps1` downloads the files on the
board, from the pinned commits above, and refuses any file whose SHA-256
differs. CI checks the same hashes and runs the driver's core over the
files (without storing them). A release could still ship the Raspberry Pi
files in a separate archive with their licence text if that is wanted
later; that is a decision for the maintainer, not a technical one.

### CM5-IO (BCM43752)

`install-firmware.ps1 -Chip 43752` fetches, from the same armbian/firmware
commit `db8cc456`:

| File | |
|---|---|
| `brcmfmac43752-sdio.bin` | 743 607 bytes, 18.35.387.23.7 (`<FW-TAG>4362a2-roml/config_sdio_ampak_release_he`, 2020-07-13), SHA-256 `46f62076...f0bdde` |
| `brcmfmac43752-sdio.clm_blob` | 29 225 bytes, SHA-256 `5143146e...a9a97a` (21 `clmload` chunks) |
| `brcmfmac43752-sdio.txt` (NVRAM) | 8 355 bytes, `AP6275S_NVRAM_V1.7_20210726`, SHA-256 `2d272310...6cc66f` |

These are AMPAK's AP6275S files (the chip, BCM43752 rev 2, is the same;
the module and its RF layout are not: the CM5 carries a BW3752-50B1).
They are what brcmfmac ran on this board under Fedora, so they are known to
load; the NVRAM is not tuned for the module. Same licence situation as the
Armbian 43456 files: none stated, so fetched on the board, not shipped.
The firmware's trailer is a `<FW-TAG> ... Version=` line, not the 43456's
`Version: `; `BwlFwImageVersion` reads both. CI runs the driver's core
over the three files against a simulated BCM43752.

The NVRAM's `macaddr=00:90:4c:c5:12:38` is AMPAK's placeholder, the same on
every AP6256. Later the driver needs a stable per-board address (for
example derived from the RK3576 OTP CPU ID), set with `cur_etheraddr`
before `C_UP`.

## Tests

`tools/bwltest`, run by CI (`bwltest` job) on Linux with gcc, with
AddressSanitizer and UBSan:

- unit tests: NVRAM conversion (comments, CRLF, blanks, no final newline,
  the token), the image trailer and `ver` version parsers, firmware image
  checks, CLM chunking (flags, lengths), SDPCM build/parse (padding,
  checksum, offsets), TX credit arithmetic with wrap-around, BCDC
  command/reply, the BCDC data header, event parsing;
- the CMD53 split (block/byte, block counts) against the simulated chip;
- a full bring-up against `fakechip.c`: attach (EROM with a 64-bit
  region, a size descriptor, a core without wrappers, a master-wrapper
  CPU), download and verify, CPU release (the simulation refuses a boot
  whose RAM or reset vector or NVRAM token is wrong), start (F2 only after
  the mailbox and the running CPU), the start-up event, version,
  `cur_etheraddr`, the CLM upload (checked byte for byte), `ver`, an
  unknown iovar (error status), the shared block and console;
- faults: a corrupted RAM byte (must fail the verify and not start the CPU),
  an I/O error mid-download, an image larger than RAM;
- the same bring-up with the real Armbian and Raspberry Pi files.

The simulation checks the host side only: command shapes (byte mode within
a block, block mode in whole blocks, no CMD53 on F0, 4-byte lengths, no
crossing the backplane window, F2 only when ready, F2 reads at a fixed
address), ordering, and encodings. It cannot say anything about timing,
about what sdbus allows, or about what the real firmware answers. That is
what the board session is for.

`.github/workflows/ci.yml` also builds the driver with the WDK (`build`
matrix, `wifi`) and makes the test-signed package (`wifi-package`).

## Risks and open questions

1. sdbus and functions 0 and 2 from one PDO (above). Decides one or two
   driver instances.
2. CMD53 sizes through dwcmshc: the largest transfer the miniport accepts
   and whether byte-mode transfers that are not a multiple of 4 ever occur
   (the core never issues them).
3. The SDIO device ID (0xA9BF assumed).
4. The controller clock: W2 starts at 25 or 50 MHz. The firmware download
   (about 480 KB) takes about a second at 25 MHz 4-bit, so this matters
   only for throughput.
5. RAM size and base for rev 9: 0x198000 is bwfm's and brcmfmac's value for
   chip 0x4345; the size is read from the chip.
6. A second start without a power cycle (disable/enable): the stop path
   halts the CPU and turns F1/F2 off, but WL_REG_ON is the firmware's, so
   the chip is not reset. Restart Windows between stages until this is
   tried.
7. WiFiCx documentation and the absence of a public sample (W4).
8. The per-board MAC address (above).

## Other boards: ROCK 4D, Dragon Q6A, CM5-IO

Checked on 2026-11-25, from documents only; nothing below has been tried
on a board.

| Board | Wi-Fi/BT | Bus | Windows driver that exists |
|---|---|---|---|
| TaishanPi 3M | Broadcom BCM43456 (AMPAK AP6256) | SDIO | none; this page |
| Radxa ROCK 4D | AICSEMI AIC8800D80 (module listed by CNX Software as FCU760K; Radxa's own pages say only "Wi-Fi 6, BT 5.4") | USB | Mario Bălănică's `aicwlan` (below) |
| Radxa Dragon Q6A (QCS6490) | AICSEMI AIC8800D80 | USB | the same `aicwlan` |
| ArmSoM CM5-IO (CM5 module) | Synaptics/Broadcom BCM43752 (BW3752-50B1 module) | SDIO, BT on UART4 | `bcmwlsdio` (branch `cm5io-wifi-bt`) and `bcm4345bt` |

So the guess was right: the ROCK 4D and the Dragon Q6A carry the same
chip, the AIC8800D80 on USB. Sources: the mainline patch that powers the
ROCK 4D's Wi-Fi ("The production version of the ROCK 4D appears to sport
a AICSEMI AIC8800D80 USB Wi-Fi + BT chipset", linux-rockchip, June 2025;
the power GPIOs it adds, GPIO2 PC7 for `vcc_3v3_wifi` and GPIO2 PD1 for
the rfkill, are the ones edk2-rk3576's ROCK 4D platform code already
drives at boot), and Radxa's forum thread "Wi-Fi/BT support for Windows
on Q6A" ("AIC8800D80 USB Wi-Fi/BT combo chipset found in the Dragon Q6A").

**The Q6A's Windows driver.** Radxa's Windows page for the Q6A says the
default driver package has no Wi-Fi or Bluetooth driver and points to
`aicwlan_arm64_0935761.zip` in worproject/dldserv-mirror, "maintained by
the community (Mario in worproject/dldserv-mirror)", "a test-signed
build", covering "most client-mode Wi-Fi functionality"; forum users
report Bluetooth working too. Looking inside the zip:

- `aicwlan.inf`: "INF file for installing the AIC8800 WiFiCx USB driver",
  Copyright (c) 2026 Mario Bălănică, Class Net, DriverVer 7.47.8.743
  (2026-06-24). Binds `USB\VID_A69C&PID_8D80` (the chip before its
  firmware is loaded) and `USB\VID_A69C&PID_8D81&MI_02` (after).
- `aicwlan.sys`: a KMDF driver binding the WiFiCx and NetAdapterCx class
  extensions, so Windows' own Wi-Fi UI works with it. Binary only.
- `fw/usb/aic8800D80*/...`: AICSEMI firmware, patch tables and RF
  configuration for several AIC8800 variants.
- **No licence** for the driver or the firmware anywhere in the package,
  and no source published. So it can be used by anyone who downloads it
  from the mirror, but this repository cannot redistribute it, modify it,
  or ship it in an image without the author's permission.

**Will it work on the ROCK 4D?** Probably not as it is, for one reason that
has nothing to do with the chip: the INF's models section is
`NTARM64.10.0...26100`, so it installs only on Windows 11 24H2 or later,
and 24H2 cannot boot on the RK3576 (Cortex-A72/A53 are ARMv8.0; 24H2
needs ARMv8.1 atomics). The Q6A's Kryo cores are ARMv8.2, which is why
it is fine there. Whether the binary itself needs 24H2 (a WiFiCx or
NetAdapterCx version that 23H2 lacks) or only the INF says so is not
known. A private test is cheap and says which:

1. ROCK 4D, Windows 11 23H2 (22631) as on the other boards, test signing
   on. Check that `USB\VID_A69C&PID_8D80` enumerates under the DWC3 host
   (`pnputil /enum-devices /connected`); the firmware already powers the
   module.
2. On the board only, edit a copy of `aicwlan.inf` to
   `NTARM64.10.0...22621`, make a new catalog and test-sign it with a
   local certificate, `pnputil /add-driver ... /install`.
3. Pass: the loader device turns into `PID_8D81`, a Wi-Fi adapter
   appears, a scan lists networks. Fail: code 39 / 31 or a class
   extension version error in the event log means the binary needs 24H2.

The modified copy stays on that board; it is not committed or shipped.

**The useful next step is to ask the author.** Mario Bălănică wrote
`dwcmshc` and the other worproject drivers this repository is built on,
under MIT. A 22621-floor build, or the source under a licence that allows
redistribution, would give the ROCK 4D Wi-Fi and Bluetooth on Windows
without writing a driver. The AICSEMI firmware's terms would also need
settling for any image that ships it (radxa-pkg/aic8800 carries it for
Linux).

**Sooner than BCM43456?** For the ROCK 4D, yes, possibly within a day if
the binary runs on 23H2, and in any case far sooner than any from-scratch
driver. For the TaishanPi 3M, no: the AIC8800 driver does nothing for a
Broadcom chip on SDIO, and there is no existing Windows driver for the
BCM43456 to reuse. For the CM5-IO the SYN43752 is in the same Broadcom
FullMAC family as the BCM43456 (bwfm and brcmfmac support it, chip 43752,
RAM base 0x170000), so once `bcmwlsdio` works on the TaishanPi it is the
shortest way there too: a second chip entry, its firmware, its NVRAM and
the CM5-IO's SDIO controller node in ACPI, not a second driver.

That is now done, on branch `cm5io-wifi-bt` (2026-11-25, not run on a
board): the chip table in `bwl_chip.c` has the 43751/43752 (firmware name
`43752`, RAM base 0x170000, save/restore from the retention control, F2
blocks of 256 bytes, F2 watermark 0x40 with the watermark enable and
message-busy control 0xC0, CMD_NODEC instead of CMD14), the INF matches
`SD\VID_02D0&PID_AAE8` and `PID_AAE7`, and `tools/bwltest` has a simulated
BCM43752 (chipcommon with a separate PMU core). The firmware side is
edk2-rk3576 `cm5io-parity`: the "SDIO Wi-Fi" option and the `RKCPFE2D` SSDT,
with the controller and GPIOs the TaishanPi 3M uses (the ArmSoM-CM5-1V1
schematic, page 17: SDMMC1 M0, WL_REG_ON GPIO1_C6, host wake GPIO0_B0, LPO
from the LK8563S). ArmSoM's vendor device tree still says `rtl8852bs`:
that is the V1.0 module (BL-M8852BS2), replaced by the BW3752-50B1 in V1.1.
Check which one a board has: Linux on the user's CM5-IO found a BCM43752.

Sources: [ROCK 4D Wi-Fi patch](https://lists.infradead.org/pipermail/linux-rockchip/2025-June/059994.html),
[Radxa: Windows on the Dragon Q6A](https://docs.radxa.com/en/dragon/q6a/other-system/windows),
[Radxa forum: Wi-Fi/BT support for Windows on Q6A](https://forum.radxa.com/t/wi-fi-bt-support-for-windows-on-q6a/31091),
[the driver package](https://github.com/worproject/dldserv-mirror/releases/download/13%2F02%2F2024/aicwlan_arm64_0935761.zip),
[CNX Software on the ROCK 4D](https://www.cnx-software.com/2025/05/28/radxa-rock-4d-sbc-raspberry-pi-lookalike-powered-by-rockchip-rk3576-edge-ai-soc/).
