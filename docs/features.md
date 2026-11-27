# Using the hardware under Windows

English | [简体中文](features.zh-CN.md)

What each feature needs under Windows 11 23H2. Status per board is in the
[README](../README.md#status). Firmware: [edk2-rk3576 0.5.0](https://github.com/gahingwoo/edk2-rk3576/releases/tag/0.5.0) or later.

## Storage, USB, Ethernet

| Feature | Driver | Notes |
|---|---|---|
| eMMC | `dwcsdhc` | Windows boots from it on the CM5-IO and the TaishanPi 3M |
| SD card | `dwcmshc` | eject and reinsert work (CM5-IO) |
| NVMe (CM5-IO, ROCK 4D) | inbox `stornvme` | the ROCK 4D boots Windows from it |
| USB-A | inbox `usbxhci` | |
| USB-C, CM5-IO | inbox `usbxhci` | USB 2.0 |
| USB-C, TaishanPi 3M | inbox `usbxhci` | Only a device plugged in before power-on works (USB 3, 329 MB/s on a disk); one plugged in later gets no power until the next boot. Reboot before connecting a charger or a PC there |
| Ethernet | `dwc_eqos` | DHCP; 1 Gbps on the TaishanPi 3M |

## Display

Without a GPU driver, the inbox BasicDisplay scans out the firmware's
framebuffer: one mode, no hot-plug, drawn on the CPU.

With [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) (TaishanPi
3M, ROCK 4D) the desktop is drawn on the Mali-G52, the DSI screen can be turned to
landscape and has a brightness slider, and HDMI hot-plug and HDMI audio
work (DSI on the TaishanPi 3M only). Its settings are in that repository.

## Touch (TaishanPi 3M)

[gt9271](../drivers/input/gt9271) drives the DSI screen's GT9271, ten touch
points. The firmware declares the controller (`\_SB.I2C0.TCH0`,
`RKCP9271`) only when the screen is attached.

## Audio (TaishanPi 3M, ROCK 4D)

- Headphone jack and jack microphone: rk3xi2sbus, es8323 and csaudiork3x
  1.0.0.135 or later. Keep the installed pl330dma.
- HDMI audio: the same drivers, with wddm-mali-g52 driving HDMI (its
  `HdmiHpd` setting at 3).
- ROCK 4D: headphones, microphone and HDMI audio work with the same
  drivers. Its jack has no detect switch; detection through rk3xsaradc is
  not tried yet (see below).
- CM5-IO HDMI audio is declared by the firmware, not tried.

Details: [audio](dev/audio.md).

## Bluetooth (TaishanPi 3M)

[bcm4345bt](../drivers/bluetooth/bcm4345bt): pairing and file transfer.
No voice calls or headset microphones (SCO goes over a PCM link with no
driver).

1. Fetch the patchram once with the driver's `install-firmware.ps1` (it
   downloads it and checks its SHA-256; it cannot be shipped here).
2. Install the bcm4345bt package ([drivers](drivers.md)).

## Video decoder

[vdpu383](../drivers/video/vdpu383) and its test tool `vdpu383test` decode
H.264 on the TaishanPi 3M, matching FFmpeg bit for bit on the conformance
streams, and work on the ROCK 4D too. Needs *Video Decoder (VDPU383)* enabled in firmware
setup (on by default since 0.5.0). Players do not use it yet: [video decode](dev/video-decode.md).

## Buttons on the TaishanPi 3M (CI only)

The board has RECOVERY and RESET, and nothing on the PMIC's power-on line.

| Button | Under Windows |
|---|---|
| RECOVERY | the power button: a short press does what Control Panel, Power Options, "Choose what the power buttons do" says |
| RESET | resets the board |

Needs edk2-rk3576 0.5.0 (setup option "ADC keys for the OS", on by default
on this board) and the rk3xsaradc driver. A key counts only after it has
been seen released since boot or resume, and must be held for two polls
(about 200 ms), so holding RECOVERY at power-on does nothing.

Check: `reg query
"HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0ADC\0\Device Parameters"`
shows `DiagKeysArmed 1`, and `DiagHidReports` grows after a press. If the ADC
device causes trouble, turn the setup option off; the firmware then leaves
the ADC alone.

## ADC keys and headphone jack on other boards (CI only)

[rk3xsaradc](../drivers/adc/rk3xsaradc) binds `ACPI\RKCP0ADC` (ROCK 4D:
firmware 0.5.0; CM5-IO: a firmware branch). Their Maskrom keys become HID buttons
on vendor page 0xFF00, which Windows ignores by itself. The ROCK 4D's
headphone jack has no detect switch; it is read on ADC channel 3 and drives
the Headphones endpoint through es8323. `tools/adctest` reads any channel.

## NPU

[woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) (TaishanPi 3M,
ROCK 4D).

## Load and temperatures

Task Manager on 23H2 shows no NPU and no temperature. rk-monitor shows them:
a tray icon with the hottest temperature, a popup, and a desktop widget with
CPU per cluster, GPU (use, temperature, clock), NPU per core and the thermal
zones. Without maliwddm the GPU rows say "Mali driver not installed".

Install the RK3576 tools (CI artifact `rk3576-tools-arm64`): unpack it and
run `install.cmd`. It puts rk-monitor, `rknputest` and `charsiu_run` into
`%ProgramFiles%\RK3576`, starts rk-monitor at every logon with the widget
on, and registers the GPU and NPU performance counters again after every
driver update. `uninstall.cmd` removes all of it. Details:
[tools/rk3576-tools](../tools/rk3576-tools/README.md),
[tools/rk-monitor](../tools/rk-monitor/README.md).

## Not available

- Wi-Fi on any board ([wireless](dev/wireless.md)), and Bluetooth on the
  ROCK 4D.
- Mobile data from the EC20 modem on the TaishanPi 3M: this firmware never
  answers MBIM, and RNDIS mode forwards nothing. The module stays in QMI
  mode until Quectel firmware is available ([EC20](dev/ec20.md)).
- A valid RTC time on the TaishanPi 3M
  ([troubleshooting](troubleshooting.md#the-clock-is-wrong-after-every-boot)).
