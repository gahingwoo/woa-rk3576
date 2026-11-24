# woa-rk3576

[![CI](https://github.com/gahingwoo/woa-rk3576/actions/workflows/ci.yml/badge.svg)](https://github.com/gahingwoo/woa-rk3576/actions/workflows/ci.yml)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

[简体中文](README.zh-CN.md)

Windows on Arm drivers for Rockchip RK3576 boards, and the WinPE image and
scripts to install Windows 11 23H2 on one. Run on the ArmSoM CM5-IO and the
LCKFB TaishanPi 3M.

New to these boards? Start at
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576#from-a-new-board-to-windows-or-linux):
the firmware comes first, and its README has the whole path from a new board
to Windows or Linux.

![Windows 11 on the TaishanPi 3M's DSI screen in landscape, reporting 10 touch points](docs/imgs/tspi3m-win11-touch-landscape.png)

## Install

1. Write [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576) firmware to
   the eMMC as the whole image.
2. Follow [docs/INSTALL.md](docs/INSTALL.md): a WinPE stick from the
   [latest release](https://github.com/gahingwoo/woa-rk3576/releases/latest),
   `install.wim` from a Windows 11 23H2 ARM64 ISO, one script.

Or install from a PC without a stick with
[Maskrom Studio](https://github.com/gahingwoo/maskrom-studio) (not yet run on
a board, 2026-11-24).

Then: [using the hardware](docs/features.md) (CPU, GPU and NPU load and the
temperatures: the [RK3576 tools](tools/rk3576-tools/README.md)),
[adding and replacing drivers](docs/drivers.md),
[troubleshooting](docs/troubleshooting.md).

## Status

On 2026-11-24, under Windows 11 23H2 (22631). "ci firmware" means
edk2-rk3576 newer than 0.4.1 (its `ci` build until the next release).

| | ArmSoM CM5-IO | LCKFB TaishanPi 3M | Driver |
|---|---|---|---|
| eMMC (boot disk) | works | works | dwcsdhc |
| SD card | works, including eject and reinsert | driver starts; no card tried | dwcmshc |
| NVMe | works | no M.2 slot | inbox stornvme |
| USB-A | works | works | inbox usbxhci |
| USB-C | USB 2.0 | works for a device attached at power-on (ci firmware) | inbox usbxhci |
| Ethernet | works, DHCP | works, 1 Gbps | dwc_eqos |
| GPIO, I2C, SPI, DMA | start | start | rk3xgpio, rk3xi2c, rk3xspi, pl330dma |
| Display | firmware framebuffer, one mode | GPU desktop with [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52); otherwise the framebuffer | inbox BasicDisplay, or wddm-mali-g52 |
| Touch (DSI screen) | not tried | works, 10 points (ci firmware) | gt9271 |
| Headphone jack and microphone | no jack on the carrier | works (ci firmware) | rk3xi2sbus, csaudiork3x, es8323 |
| HDMI audio | not tried | works with wddm-mali-g52 (ci firmware) | the same |
| Bluetooth | not written | works: pairing, file transfer; no voice (ci firmware) | bcm4345bt |
| WiFi | not working | not working | none |
| NPU | not available | works with [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) | rknpu |
| Video decoder (VDPU383) | not tried | hardware decoder works with vdpu383test (H.264 conformance streams match FFmpeg, 2026-11-24); players not yet ([video decode](docs/dev/video-decode.md)) | vdpu383 |
| RTC, PMIC, pin controller, UART2 | no driver needed | no driver needed; named, no yellow mark (2026-11-24 on the TaishanPi 3M; UART2 no longer declared there) | [null](drivers/null) |

Each result is from one board. Most of the CM5-IO column was seen on
2026-10-01. Details per feature: [features](docs/features.md).

## Releases and the rolling ci build

Tagged releases carry the WinPE image (`woa-rk3576-winpe-<tag>.img.xz`),
the drivers and `SHA256SUMS.txt`. The latest is 0.1.1 (2026-11-13); the
touch, Bluetooth and audio drivers are newer than it.

The pre-release tagged
[`ci`](https://github.com/gahingwoo/woa-rk3576/releases/tag/ci) holds the
WinPE image of the latest successful build of `main`
(`woa-rk3576-winpe-ci-<sha>.img.xz` and `SHA256SUMS.txt`). It is never marked
latest, every successful build replaces it, and it is untested as a whole.
Use a tagged release unless you need a change that is only on `main`.
Maskrom Studio's CI tier uses the `ci` release.

## For developers

- [Architecture](docs/dev/architecture.md): which driver binds what, what
  changed from the RK3588 drivers, the layout
- [Building](docs/dev/building.md)
- [Debugging](docs/dev/debugging.md): collecting data in WinPE and Windows,
  the kernel debugger
- [Audio](docs/dev/audio.md), [display](docs/dev/display.md),
  [wireless](docs/dev/wireless.md)
- [History](docs/history/README.md): dated notes kept as written

```cmd
msbuild build\RockchipDrivers.sln /p:Configuration=Release /p:Platform=ARM64
```

CI builds every driver on every push; the WinPE workflow also test-signs
them and puts them in a WinPE image.

## Acknowledgements

Thanks to [JLC](https://jlc.com/) (嘉立创) for sponsoring the LCKFB TaishanPi 3M, its MIPI DSI screen, the fan and the EC20 4G module used for this work, and to [ArmSoM](https://www.armsom.org/) for sponsoring the CM5 module and the CM5-IO board.

The ported drivers are the work of the worproject Rockchip-Windows-Drivers
authors: Mario Bălănică (SD/eMMC), CoolStar (GPIO, I2C, DMA, audio) and Doug
Cook (Ethernet), building on Microsoft's driver samples.

## License

Code written for this repository is GPL-2.0-only ([LICENSE](LICENSE)).
Releases before this change were MIT and stay MIT. The ported drivers keep
their own licences, and RK3576 changes to them are made under the same
licence as the driver they change; see [THIRD_PARTY.md](THIRD_PARTY.md).
