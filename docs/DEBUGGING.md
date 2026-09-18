# Getting data out of Windows on this board

WinPE has no serial console. The thing that would give one — SAC, behind
`bcdedit /ems on` — needs `sacdrv.sys` and `sacsvr`, and neither is in WinPE.
SPCR only marks a debug port; it does not create a console. So the serial line
goes quiet the moment `bootmgfw.efi` takes over, and everything after that is
screen-and-keyboard unless something writes to disk for us.

`tools/woa-debug/` is that something.

## What it is

| File | Role |
|---|---|
| `autounattend.xml` | The hook. Windows Setup reads it from the root of removable media during the windowsPE pass and runs one command. |
| `collect.cmd` | The part that changes. Add probes here. |

Output lands in `woa-debug\out\` on the stick, which you read from Linux
afterwards.

> **The XML must never grow a `<DiskConfiguration>` or `<ImageInstall>`
> section.** Either one turns Setup unattended, and an unattended Setup
> partitions disks by itself. On this board that wipes whatever is on the
> NVMe, and the firmware lives in the first 32 MB of the eMMC. As written the
> file contains `RunSynchronous` and nothing else, so Setup runs the commands
> and then falls through to the normal interactive UI, touching no disk.

## Installing it

With the stick mounted (as root, adjust the path):

```sh
cp tools/woa-debug/autounattend.xml /mnt/usb/
mkdir -p /mnt/usb/woa-debug
cp tools/woa-debug/collect.cmd /mnt/usb/woa-debug/
sync
```

`autounattend.xml` goes in the **root**; `collect.cmd` goes in `woa-debug\`.
The XML scans drive letters for it rather than hardcoding one, because WinPE
does not give the stick the letter Linux did.

Boot the stick. Setup runs the probes before its first screen, then carries on
normally. Shut down, move the stick back to Linux, and read
`woa-debug/out/`.

## What it collects

| File | Answers |
|---|---|
| `10-devices.txt` | `pnputil /enum-devices` — every device Windows enumerated and whether a driver bound. A device present with no driver is a different problem from one that never appeared. |
| `11-enum-acpi.txt` | The ACPI branch of the device tree, by `_HID`. `RKCP0D40` eMMC, `RKCPFE2C` SD, `RKCP6543` GMAC, `RKCP300x` GPIO/I²C/SPI. |
| `12-enum-pci.txt` | The PCI branch. Empty means the root bridge enumerated nothing behind it — a firmware/ECAM question, not a driver one. |
| `20-diskpart.txt` | `list disk` and `list volume`. |
| `30-setupact.log`, `31-setuperr.log` | Setup's own logs. `setuperr.log` is short and names what Setup objected to. |
| `40-drivers.txt` | Driver packages already staged in this WinPE. |
| `50-drvload.txt`, `51-devices-after-drvload.txt` | Only if `woa-debug\drivers\` exists — see below. |
| `60-resourcemap.txt` | `HKLM\HARDWARE\RESOURCEMAP` — the arbiter's **output**: every interrupt vector, memory range and port actually granted, by owner. The Enum dumps say what a device asks for; this says what the machine gave out, which is the only way to see why an allocation failed. |
| `61-hw-description.txt` | What Windows built from the ACPI tables before any driver ran. |
| `62-acpi-tables.txt` | Which ACPI tables Windows loaded, by signature — confirms from the OS side which firmware is in use. |
| `63-problem-devices.txt` | Devices with a problem code, on their own. |
| `64-setupact-pnp.txt` | The PCI/resource/arbiter lines of `setupact.log`, so the whole 24 KB does not have to be read over a serial console. |

## Loading this project's drivers

Drop driver packages into `woa-debug\drivers\<name>\` on the stick and
`collect.cmd` runs `drvload` on each `.inf`, then re-runs the device list so
before and after sit side by side.

Two limits worth knowing before reading a failure as a bug in the driver:

* **Signing.** Unsigned kernel drivers need test signing enabled in the
  stick's BCD and Secure Boot off, or `drvload` fails with a signature error.
* **Frameworks.** WinPE does not ship GpioClx, SpbCx or NetAdapterCx, so
  [gpio](../drivers/gpio/rk3576gpio), [i2c](../drivers/i2c/rk3xi2c),
  [spi](../drivers/spi/rk3xspi) and [net](../drivers/net/dwmac) cannot load
  there however they are signed. They need an installed Windows.
  `sdport` **is** present, because WinPE boots from storage, so
  [rkdwmmc](../drivers/storage/rkdwmmc) is the one that can be tried this way.

## Adding a probe

Append a block to `collect.cmd`. Keep each probe self-contained and redirect
its own output: a command that does not exist in a given WinPE build should
not be able to take the rest of the run down with it.


## Reading a resource failure

`CM_PROB_NORMAL_CONFLICT` on a device means the arbiter could not satisfy its
requirements. Three files answer three different questions, and they are easy
to confuse:

* `11-enum-acpi.txt` / `12-enum-pci.txt` → `LogConf\BasicConfigVector` is
  **what the device will accept**, as alternative lists of descriptors. A
  device with an unconstrained alternative for some resource cannot be starved
  of that resource, whatever the tables say.
* `LogConf\BootConfig` in the same dumps is **what firmware left it at** —
  for a PCI device, the BAR addresses UEFI programmed. ARM64 Windows keeps
  these rather than rebalancing, so the address UEFI picks is the address
  Windows is stuck with.
* `60-resourcemap.txt` is **what was actually granted**, to everyone.

Both are `REG_RESOURCE_LIST` / `IO_RESOURCE_REQUIREMENTS_LIST` binaries.
Decoding notes that cost time to work out: the descriptors in these lists are
**20 bytes**, not the 16 the struct definition suggests; `reg query` prints
each value on one line, so grab it by line number rather than by pattern (a
dump of the whole Enum tree has a `BootConfig` under almost every device); and
strip to hex *after* removing the value name, since `BootConfig` and
`REG_RESOURCE_LIST` contain hex letters themselves.
