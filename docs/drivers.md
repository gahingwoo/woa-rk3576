# Adding and replacing drivers

English | [简体中文](drivers.zh-CN.md)

How to put a driver into an installed Windows on these boards, and how to
replace one that is already running. The same rules apply to the drivers of
this repository, [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)
and [woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu).

## All of them at once

The [driver package](../tools/rk3576-drivers/README.md)
(`rk3576-drivers-arm64.zip` on the releases) holds every driver for the
three boards and an `install.cmd` that installs the board's list.

## Before anything: the clock

Sync the clock first (`w32tm /resync /force`). The TaishanPi 3M's RTC holds
no valid time, and pnputil refuses a catalog signed "in the future" with "a
required certificate is not within its validity period". See
[troubleshooting](troubleshooting.md#the-clock-is-wrong-after-every-boot).

## From WinPE, offline

Boot the WinPE stick and run `woa-deploy\add-drivers.cmd`. It adds or
updates the drivers on the stick in the installed Windows without erasing
anything.

## In the running Windows

Take the driver from the WinPE artifact's `woa-drivers\` (or the driver
project's own signed package), not from the CI build artifact: only the
WinPE workflow signs the catalogs.

Each WinPE run signs with a new test certificate, so trust that certificate
first:

1. Take the certificate out of the catalog. On Linux:

   ```bash
   openssl pkcs7 -inform DER -in gt9271.cat -print_certs -out signer.pem
   openssl x509 -in signer.pem -outform DER -out signer.cer
   ```

2. On the board, from an administrator prompt:

   ```
   certutil -addstore -f Root signer.cer
   certutil -addstore -f TrustedPublisher signer.cer
   ```

3. Install a driver that is not yet running:

   ```
   pnputil /add-driver gt9271.inf /install
   ```

## Replacing a driver that is already bound and running

`pnputil /add-driver` followed by a restart keeps the old driver, and
`/install` on a running audio, DMA, display or Bluetooth driver has
bugchecked the TaishanPi 3M. Instead:

1. Stage the package only: `pnputil /add-driver <name>.inf`.
2. As SYSTEM, set bit 0x20 (`CONFIGFLAG_REINSTALL`) in `ConfigFlags` under
   the device's `HKLM\SYSTEM\CurrentControlSet\Enum\<instance>` key.
3. Restart. The device comes up on the newest staged package.

Rules learnt on the board:

- pnputil keeps the old binary when `DriverVer` has not changed. Install a
  build with a newer `DriverVer` instead.
- Do not use `pnputil /delete-driver /uninstall` to force a replacement: on
  es8323 it set the service to disabled and removed the `.sys`.
- After reinstalling an audio driver, check the default playback device in
  the taskbar's sound menu. Windows can keep pointing at the endpoint of the
  previous install, which is gone, and then plays to nothing.
- Delete staged packages that nothing uses any more
  (`pnputil /enum-drivers`, then `pnputil /delete-driver oemNN.inf`): on the
  TaishanPi 3M's eMMC they add up quickly.
