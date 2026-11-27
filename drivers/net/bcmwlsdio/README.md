# bcmwlsdio: BCM43456 and BCM43752 Wi-Fi bring-up (TaishanPi 3M, CM5-IO)

KMDF function driver for the Broadcom BCM43456 in the AMPAK AP6256 module
(TaishanPi 3M) and the BCM43752 in the BW3752-50B1 module (ArmSoM CM5 on
the CM5-IO), on the RK3576 SDIO controller. Both boards wire the module to
the same controller and the same GPIOs. Stages 0 to 2 (W3 of [docs/dev/wifi.md](../../../docs/dev/wifi.md)) talk
to the chip over SDIO, identify it, start its firmware and read its
version; on the TaishanPi 3M they passed on the board on 2026-11-26.
Stage 3 makes it a WiFiCx 1.0 Wi-Fi adapter (scan, connect, data) and
**has not run on a board yet**.

## What it needs

- Firmware (edk2-rk3576) with *ACPI / Device Tree → SDIO Wi-Fi
  (experimental)* turned on. Without it Windows has no SDIO controller.
- `dwcmshc` from this branch (W2), which drives that controller
  (`ACPI\RKCPFE2D`). Windows' own sdbus then enumerates the card.
- The firmware files, which this package does not contain. Run
  `install-firmware.ps1` as administrator (on the CM5-IO with
  `-Chip 43752`); it downloads `brcmfmac43456-sdio.bin`, `.txt` and
  `.clm_blob` (or the `brcmfmac43752-sdio` ones) from pinned commits,
  checks their SHA-256 and puts them in `%SystemRoot%\System32\drivers\brcm`.
  The driver picks the names from the chip ID it reads.

## Stages

Under the device's `Device Parameters` key, `Stage` (DWORD) decides how far
the driver goes. Both INFs set 0 on a new install and keep a value that is
already there. Two INFs, one binary: `bcmwlsdio.inf` (Class System, the
bring-up) and `bcmwlwifi.inf` (Class Net, the Wi-Fi adapter). Install one.

| Stage | Does |
|---|---|
| 0 | opens the SDBUS interface and records what sdbus reports; the card is not touched |
| 1 | also reads the CCCR, turns on function 1, reads the chip ID, scans the cores, halts the chip's CPU and sizes its RAM |
| 2 | also writes the firmware and NVRAM to RAM, verifies them, starts the CPU, waits for function 2, loads the CLM blob and asks for `ver` |
| 3 | also sets up the firmware's station and becomes a WiFiCx Wi-Fi adapter: needs `bcmwlwifi.inf`, function 1 only; read when the device starts |

The driver writes what it found next to `Stage` and flushes it (`LastStep`,
`FwState`, `ChipId`, `FwVersion`, `FwConsole` and more; the full list is
in docs/dev/wifi.md). Read it with:

```powershell
$d = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'SD\VID_02D0*'
$d | ForEach-Object { reg query "HKLM\SYSTEM\CurrentControlSet\Enum\$($_.InstanceId)\Device Parameters" }
```

## Layout

| File | |
|---|---|
| `driver.c` | PnP, the bring-up work item, the registry breadcrumbs |
| `sdbus.c` | CMD52/CMD53 and properties through `SdBusSubmitRequest` |
| `files.c` | reading the firmware files |
| `wificx.cpp` | stage 3: the WDI commands from WiFiCx, on the worker thread (aic8800usb's front end) |
| `netadapter.c` | stage 3: the NetAdapterCx queues, native 802.11 frames, the TX and RX FIFOs |
| `bwl*.c`, `bwl.h`, `bwl_regs.h` | the chip and protocol code, plain C with no Windows dependency; CI builds it into `tools/bwltest` and runs it against a simulated chip |

The chip and protocol code follows OpenBSD's `bwfm` (ISC licence, Patrick
Wildt and Broadcom), whose notice is kept in the files that follow it; see
[LICENSING.md](LICENSING.md).
