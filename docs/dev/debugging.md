# Getting data out of Windows on this board

Windows has no serial console here: SAC needs `sacdrv.sys`, which WinPE does
not have, and SPCR only marks a debug port. Everything below either writes its
results to the USB stick, to be read from Linux afterwards, or goes over the
kernel debugger.

## In WinPE: `collect.cmd`

The WinPE image from this repo runs `tools/woa-debug/collect.cmd` at startup
and writes to `woa-debug\out\` on the stick. On Setup media,
`autounattend.xml` in the root of the stick runs the same script before
Setup's first screen.

`autounattend.xml` must never get a `<DiskConfiguration>` or `<ImageInstall>`
section. Either one makes Setup unattended, and unattended Setup repartitions
disks by itself, which here means the NVMe and the firmware on the eMMC.

| File | Answers |
|---|---|
| `10-devices.txt` | every device Windows enumerated, and whether a driver bound |
| `11-enum-acpi.txt`, `12-enum-pci.txt` | the ACPI and PCI branches of the device tree |
| `20-diskpart.txt` | disks and volumes |
| `30-setupact.log`, `31-setuperr.log` | Setup's logs, when run from Setup media |
| `40-drivers.txt` | driver packages staged in this WinPE |
| `60-resourcemap.txt` | every interrupt and memory range the arbiter granted |
| `61-hw-description.txt` | what Windows built from the ACPI tables, including the firmware build stamp |
| `62-acpi-tables.txt` | which ACPI tables were loaded |
| `63-problem-devices.txt` | devices with a problem code |

Driver packages dropped into `woa-debug\drivers\<name>\` are loaded with
`drvload` and the device list is taken again afterwards.

## In the installed system: `net-debug` and `usb-debug`

Run from an administrator prompt with the stick inserted. Each writes a
timestamped folder under `woa-debug\` on the stick.

- `net-debug.cmd`: the `dwc_eqos` ETW trace, a pktmon capture with per-layer
  counters, a DHCP attempt and a static-IP ARP test, adapter properties and
  event logs. It also turns off Fast Startup. `dwc_eqos` reports zero in
  `Get-NetAdapterStatistics` whatever happens, so read pktmon instead.
- `net-update.cmd`: replaces the installed `dwc_eqos` with the one on the
  stick. `pnputil /add-driver` alone keeps the copy DISM installed; this
  removes every other version, then you reboot.
- `usb-debug.cmd`: USBXHCI, UCX, USBHUB3 and PnP traces across a restart of
  `XHC0`, and each xHCI controller's problem code and status.

`dwc_eqos` logs with TraceLogging. Windows' `tracerpt` does not show those
event names; on Linux, the `etl-parser` Python package does. The USB providers
are manifest-based and `tracerpt` decodes them.

## Kernel debugger over serial

The debug port is UART0 at `0x2AD40000`, the firmware console. Windows runs
KDCOM at 1500000 baud whatever the BCD says. `scripts/kd-listen.py` in
edk2-rk3576 speaks enough of the protocol to keep a boot moving and logs every
module load and any `*** Fatal System Error`.

With `debug on` in the BCD and nothing listening, Windows boots very slowly.
Turn it off with `bcdedit /debug off` when you are done.

## Reading a resource failure

`CM_PROB_NORMAL_CONFLICT` means the arbiter could not satisfy a device. Three
sources answer three different questions:

- `LogConf\BasicConfigVector` in the Enum dumps: what the device will accept.
- `LogConf\BootConfig`: what the firmware left it at. For a PCI device these
  are the BARs UEFI programmed, and ARM64 Windows keeps them.
- `60-resourcemap.txt`: what was granted, to everyone.

The descriptors in these binary lists are 20 bytes, not the 16 the struct
suggests. `reg query` prints each value on one line, so pick it by line number,
and strip to hex only after removing the value name: `BootConfig` and
`REG_RESOURCE_LIST` contain hex letters too.
