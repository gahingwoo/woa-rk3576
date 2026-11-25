# aic8800usb: AIC8800D80 USB Wi-Fi (Radxa ROCK 4D), bring-up driver

**Not tested on a board.** The protocol code passes host tests against a
simulated chip and access point (`tools/aictest`), and the download runs
against the real firmware files in CI. The design, the stages and the board
procedure are in [docs/dev/aic8800.md](../../../docs/dev/aic8800.md).

What it does: downloads the firmware into the chip through its ROM loader
(USB `VID_A69C&PID_8D80`), then on the running firmware (`PID_8D81`,
interface 2) initialises it, scans, and joins one WPA2-PSK network named in
the registry, with its own supplicant. What it does not do yet: present a
network adapter, so Windows shows no Wi-Fi and no traffic flows to the
operating system. That is the next stage.

Bluetooth needs no driver from here: once the firmware runs, interfaces 0
and 1 of `PID_8D81` are a standard Bluetooth HCI (class E0/01/01) and the
in-box BthUsb binds them.

## Package

From CI, artifact `aic8800-bringup-signed-arm64`:

| File | |
|---|---|
| `aic8800usb\aic8800usb.inf`, `.sys`, `.cat` | the driver, test-signed |
| `signer.cer` | the certificate of that CI run |
| `install-firmware.ps1` | fetches the AICSemi firmware from Radxa's package (pinned commit, SHA-256 checked) into `C:\Windows\System32\drivers\aic8800` |
| `aic-bringup.ps1` | sets Stage, Ssid and Passphrase; `-Status` prints what the driver recorded |

## Install (test signing on, administrator)

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
powershell -ExecutionPolicy Bypass -File install-firmware.ps1
pnputil /add-driver aic8800usb\aic8800usb.inf
powershell -ExecutionPolicy Bypass -File aic-bringup.ps1 -Stage 1
shutdown /r /t 0
```

`pnputil /add-driver` only stages the package (no `/install`); Windows binds
it at the next boot. Raise the stage one step per boot: 1 (firmware), 2
(scan), 3 (`-Stage 3 -Ssid <name> -Passphrase <passphrase>`). After each
boot: `aic-bringup.ps1 -Status`.

Settings (service key `HKLM\SYSTEM\CurrentControlSet\Services\aic8800usb\Parameters`):

| Value | Default | |
|---|---|---|
| `Stage` | 0 | 0 open only, 1 firmware, 2 scan, 3 join |
| `Ssid` | | the network to join (stage 3) |
| `Passphrase` | | 8 to 63 characters, or 64 hex digits; replaced by `Pmk` on first use |
| `FirmwareDir` | `\SystemRoot\System32\drivers\aic8800` | |
| `ScanTimeoutMs` | 8000 | |
| `ConnectTimeoutMs` | 15000 | association, then the 4-way handshake, each |

Licences: [LICENSING.md](LICENSING.md).
