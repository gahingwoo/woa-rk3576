# Using the hardware under Windows

What works on each board under Windows 11 23H2, and what a feature needs.
As of 2026-11-24. [简体中文](zh-CN/features.md)

"ci firmware" means the edk2-rk3576
[`ci` build](https://edk2.gahingwoo.com/flash.html#the-rolling-ci-build): on
2026-11-24 no release after 0.4.1 has these features.

## Storage

- eMMC: Windows boots from it on both boards (`dwcsdhc`).
- SD card: works on the CM5-IO, including eject and reinsert (`dwcmshc`). On
  the TaishanPi 3M the driver starts; no card was tried.
- NVMe (CM5-IO): inbox `stornvme`.

## USB

- USB-A: works on both boards (inbox `usbxhci`).
- USB-C on the CM5-IO: USB 2.0.
- USB-C on the TaishanPi 3M (ci firmware): works for a device that is
  plugged in before power-on, at USB 3 speed with the disk tried (329 MB/s).
  A device plugged in later gets no power until the next boot. Reboot before
  connecting a charger or a PC there.

## Ethernet

Works on both boards (`dwc_eqos`), with a DHCP lease; 1 Gbps on the
TaishanPi 3M.

## Display

Without a GPU driver, Windows uses the inbox BasicDisplay on the framebuffer
the firmware set up: one mode, no hot-plug, drawn on the CPU.

With [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) (TaishanPi
3M): the desktop is drawn on the Mali-G52, the DSI screen can be turned to
landscape in Settings and has a brightness slider, and HDMI hot-plug works
with HDMI audio. Its settings are in that repository.

## Touch (TaishanPi 3M, ci firmware)

The DSI screen's GT9271 works with [gt9271](../drivers/input/gt9271), ten
touch points. The firmware declares the controller (`\_SB.I2C0.TCH0`,
`RKCP9271`) only when the screen is attached; with no screen, nothing appears
in Device Manager.

## Audio (TaishanPi 3M, ci firmware)

- Headphone jack: playback and the jack microphone work with rk3xi2sbus,
  es8323 and csaudiork3x 1.0.0.135 or later (since 2026-11-22). Keep the
  pl330dma that is already installed.
- HDMI audio: works with wddm-mali-g52 driving HDMI (its `HdmiHpd` setting at
  3), seen on 2026-11-23.
- On the CM5-IO, HDMI audio is declared by the firmware and has not been
  tried.

Details: [audio](dev/audio.md).

## Bluetooth (TaishanPi 3M, ci firmware)

Works with [bcm4345bt](../drivers/bluetooth/bcm4345bt) since 2026-11-22: the
radio comes up in Settings, pairing and file transfer work. Voice calls and
headset microphones do not (SCO goes over a PCM link that has no driver).

1. Fetch the patchram once with the driver's `install-firmware.ps1` (it
   downloads it and checks its SHA-256; it cannot be shipped here).
2. Install the bcm4345bt package ([drivers](drivers.md)).

## Not available

- WiFi on both boards ([wireless](dev/wireless.md)).
- Bluetooth on the CM5-IO.
- The EC20 modem's network interface on the TaishanPi 3M.
- A valid RTC time on the TaishanPi 3M ([troubleshooting](troubleshooting.md#the-clock-is-wrong-after-every-boot)).

## Video decoder

The RK3576's VDPU383 decodes H.264 under Windows with the bring-up driver
[vdpu383](../drivers/video/vdpu383) and its test tool `vdpu383test`: on
2026-11-24 the H.264 conformance streams tried on the TaishanPi 3M all
matched FFmpeg's output bit for bit. It needs edk2-rk3576 with *Video
Decoder (VDPU383)* enabled in setup (off by default). Video players do not
use it yet; the plan is in [video decode](dev/video-decode.md).

## NPU

[woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) (TaishanPi 3M).

## Load and temperatures

Windows 11 23H2 shows no NPU and no temperature in Task Manager.
rk-monitor does: a tray icon with the hottest temperature, a popup, and a
desktop widget with CPU per cluster, GPU (use, temperature, clock), NPU per
core and the thermal zones. Install it with the RK3576 tools, the CI
artifact `rk3576-tools-arm64`: unpack it and run `install.cmd`. That puts
rk-monitor, `rknputest` and `charsiu_run` into `%ProgramFiles%\RK3576`,
starts rk-monitor at every logon with the widget on, and registers the GPU
and NPU performance counters again after every driver update.
`uninstall.cmd` removes all of it. Details:
[tools/rk3576-tools](../tools/rk3576-tools/README.md) and
[tools/rk-monitor](../tools/rk-monitor/README.md).
