# RK3576 Windows driver package

English | [简体中文](README.zh-CN.md)

Every Windows driver for the RK3576 boards in one zip,
`rk3576-drivers-arm64.zip`, with one version and one install script.
Maskrom Studio installs it for the chosen board. The CI build is on the
[`ci` pre-release](https://github.com/gahingwoo/woa-rk3576/releases/tag/ci)
and is untested on boards.

## What is in it

`drivers.tsv` says which driver goes onto which board. In short:

- All boards: eMMC, SD/SDIO, GPIO, I2C, DMA, Ethernet, SPI, video decoder
  (VDPU383), SARADC keys, 40-pin UART, LEDs, audio, GPU (maliwddm) and NPU
  (rknpu).
- TaishanPi 3M: also the GT9271 touch screen and Bluetooth.
- CM5-IO and ROCK 4D: also the RK806 power button. ROCK 4D: the HAT EEPROM.
- Optional (`/optional`): Bluetooth on the CM5-IO (untested).

A driver whose device the firmware does not publish binds nothing.

The packages are test-signed; `certs\` holds the signers. `manifest.json`
names the builds each driver came from.

## Install

Test signing must be on (`bcdedit /set testsigning on`).

Into a running Windows, from an administrator prompt:

    install.cmd /online tspi3m

Into a Windows that is not running (from WinPE, `W:` holds `W:\Windows`):

    install.cmd W:\ cm5io

The board is `tspi3m`, `cm5io` or `rock4d`. Restart afterwards.

## Building it

`.github/workflows/drivers.yml` runs `assemble.py` over the latest
successful builds of the WinPE workflow, wddm-mali-g52 and woa-rk3576-npu.
