# aic8800usb: AIC8800D80 USB Wi-Fi (Radxa ROCK 4D), bring-up driver

**Not tested on a board.** The protocol code passes host tests against a
simulated chip and access point (`tools/aictest`), and the download runs
against the real firmware files in CI. The design, the stages and the board
procedure are in [docs/dev/aic8800.md](../../../docs/dev/aic8800.md).

What it does: downloads the firmware into the chip through its ROM loader
(USB `VID_A69C&PID_8D80`, `aic8800ldr.inf`), then on the running firmware
(`PID_8D81`, interface 2, `aic8800usb.inf`) initialises it, scans, joins one
WPA2-PSK network named in the registry with its own supplicant, and shows
Windows a network adapter that carries Ethernet frames over that link
(DHCP, browsing): the Ethernet-style mode. With `aic8800wifi.inf` instead
of `aic8800usb.inf` the same driver is a WiFiCx 1.0 Wi-Fi adapter: Windows'
network list and taskbar, its own WPA2 supplicant and profiles
([docs/dev/wificx.md](../../../docs/dev/wificx.md)); the INF installed picks
the mode (Device Parameters `Mode`).

Bluetooth needs no driver from here: once the firmware runs, interfaces 0
and 1 of `PID_8D81` are a standard Bluetooth HCI (class E0/01/01) and the
in-box BthUsb binds them.

## Package

From CI, artifact `aic8800-bringup-signed-arm64`:

| File | |
|---|---|
| `aic8800usb\aic8800usb.sys` | the driver, test-signed |
| `aic8800usb\aic8800ldr.inf`, `.cat` | the loader face (PID_8D80), Class System |
| `aic8800usb\aic8800usb.inf`, `.cat` | the WLAN face (PID_8D81 MI_02), a network adapter |
| `aic8800usb\aic8800wifi.inf`, `.cat` | the WLAN face as a WiFiCx Wi-Fi adapter (Mode 1); install it instead of `aic8800usb.inf` |
| `signer.cer` | the certificate of that CI run |
| `install-firmware.ps1` | fetches the AICSemi firmware from Radxa's package (pinned commit, SHA-256 checked) into `C:\Windows\System32\drivers\aic8800` |
| `aic-bringup.ps1` | `-Connect -Ssid <name>` (asks for the passphrase without echo), or one `-Stage`; `-Mode WiFiCx` or `-Mode Ethernet` switches the INF; `-Status` prints what the driver recorded and the adapter's IP |

## Install (test signing on, administrator)

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
powershell -ExecutionPolicy Bypass -File install-firmware.ps1
pnputil /add-driver aic8800usb\aic8800ldr.inf
pnputil /add-driver aic8800usb\aic8800usb.inf
powershell -ExecutionPolicy Bypass -File aic-bringup.ps1 -Connect -Ssid <your network>
shutdown /r /t 0
```

`pnputil /add-driver` only stages the packages (no `/install`); Windows
binds them at the next boot. `-Connect` turns every stage on (4); for the
step-by-step session use `-Stage 1`, then `-Stage 2`, then
`-Connect -Ssid <name> -Stage 3`, then `-Stage 4`, one per boot. After each boot: `aic-bringup.ps1 -Status`.

The passphrase is typed at a prompt and never echoed, logged or printed;
the driver turns it into the PMK on first use and deletes it, and
`-Connect` limits the settings key to SYSTEM and Administrators. (Giving
`-Passphrase` on the command line also works, but leaves it in the console
history.)

For Windows' own Wi-Fi, after stage 2 has worked in the Ethernet-style
mode: `aic-bringup.ps1 -Mode WiFiCx`, restart, connect from the taskbar
(the board plan is in [docs/dev/wificx.md](../../../docs/dev/wificx.md#board-test-plan)).

Settings (service key `HKLM\SYSTEM\CurrentControlSet\Services\aic8800usb\Parameters`):

| Value | Default | |
|---|---|---|
| `Stage` | 0 | 0 open only, 1 firmware, 2 scan, 3 join, 4 traffic through the adapter |
| `Ssid` | | the network to join (stage 3 and up) |
| `Passphrase` | | 8 to 63 characters, or 64 hex digits; replaced by `Pmk` on first use |
| `FirmwareDir` | `\SystemRoot\System32\drivers\aic8800` | |
| `ScanTimeoutMs` | 8000 | |
| `ConnectTimeoutMs` | 15000 | association, then the 4-way handshake, each |
| `WdiVersion` | 0 | WiFiCx mode: the WDI version the driver declares (0: the WDK's latest) |

Licences: [LICENSING.md](LICENSING.md).
