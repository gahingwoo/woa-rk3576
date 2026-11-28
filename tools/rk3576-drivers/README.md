# RK3576 Windows driver package

English | [简体中文](README.zh-CN.md)

Every Windows driver for the RK3576 boards in one zip,
`rk3576-drivers-arm64-<version>.zip`, with one version and one install
script. Maskrom Studio installs it for the chosen board. Get it from
[release 0.5.0](https://github.com/gahingwoo/woa-rk3576/releases/tag/0.5.0)
(the [`ci` pre-release](https://github.com/gahingwoo/woa-rk3576/releases/tag/ci)
has the untested build of `main`).

## What is in it

`drivers.tsv` says which driver goes onto which board. In short:

- All boards: eMMC, SD/SDIO, GPIO, I2C, DMA, Ethernet, SPI, video decoder
  (VDPU383), SARADC keys, 40-pin UART, LEDs, audio (SAI bus, ES8388 codec,
  HDMI and codec endpoints), the no-driver devices (`rk3576-null`: RK806,
  pin controller, RTC, UART2, PCIe DBI), GPU (maliwddm) and NPU (rknpu).
- TaishanPi 3M: also the GT9271 touch screen and Bluetooth.
- CM5-IO and ROCK 4D: also the RK806 power button. ROCK 4D: the HAT EEPROM.
- CM5-IO and ROCK 4D on Windows 10 only: NVMe (`rk3xnvme`; the inbox
  stornvme faults there). Windows 10 installs also remove `stornvme.inf` from
  the DriverDatabase entry for `PCI\CC_010802` (`win10-nvme.cmd W:\`
  offline, the SYSTEM task online), see
  [its README](../../drivers/storage/rk3xnvme/README.md). A tool that runs
  DISM itself instead of `install.cmd` runs `win10-nvme.cmd W:\` after it.
- Optional (`/optional`): Bluetooth on the CM5-IO (untested).

A driver whose device the firmware does not publish binds nothing.
The packages are test-signed; `certs\` holds the signers.

## Install

Test signing must be on (`bcdedit /set testsigning on`).

Into a running Windows, from an administrator prompt, then restart:

    install.cmd /online rock4d
    shutdown /r /t 0

This stages the drivers (`pnputil /add-driver`, never `/install`: nothing
starts now) and marks the present devices with no driver or an older one
for reinstall at the next boot. It never marks the SD/eMMC hosts (dwcsdhc,
dwcmshc), which Windows may run from, nor the Ethernet adapter unless it has
no driver; their new drivers are staged only and come in with an offline
install.

Two minutes after that restart a SYSTEM task (`rk3576-drivers-verify`)
checks that every marked device took its new driver. One that did not is
marked once more and needs one more restart: the task restarts Windows by
itself only when nobody is signed in, and otherwise says so (a message, a
notice at sign-in, the Application event log). Then it removes itself. Its
log is `%ProgramData%\RK3576\drivers\verify-bound.log`.

To see what would be marked, without changing anything:

    powershell -ExecutionPolicy Bypass -File install-online.ps1 -Board rock4d -Plan

Into a Windows that is not running (from WinPE, `W:` holds `W:\Windows`):

    install.cmd W:\ cm5io

The board is `tspi3m`, `cm5io` or `rock4d`.

## Building it

`.github/workflows/drivers.yml` builds and signs this repository's drivers
from the same commit (`build-woa.ps1`), takes maliwddm and rknpu from the
latest successful builds of wddm-mali-g52 and woa-rk3576-npu, and runs
`assemble.py`. `test_assemble.py` tests it; the workflow also plans an
online install of each board on a Windows runner, and `test_online.ps1`
runs the follow-up check against made-up devices.
