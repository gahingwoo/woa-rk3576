# null: names for the devices Windows needs no driver for

English | [简体中文](README.zh-CN.md)

`rk3576-null.inf` installs four ACPI devices with no driver, so Device
Manager lists them under *System devices* by name instead of as unknown
devices with a yellow mark (problem code 28). No binary, no service: the
install is the documented null-driver form (`AddService = ,2` with no
service name), and Windows only records the devices' resources.

| Hardware ID | ACPI device | Name in Device Manager | Why no driver |
|---|---|---|---|
| `ACPI\HYMB0001` | `\_SB.I2C2.RTC0` | HYM8563 real-time clock (not used by Windows) | Windows reads the time through UEFI runtime services, which the firmware serves from this chip |
| `ACPI\RKCP0806` | `\_SB.I2C1.PMC0` | RK806 power management IC (managed by the firmware) | rails and power-off are the firmware's |
| `ACPI\RKCP3005` | `\_SB.PINC` | RK3576 pin controller (managed by the firmware) | the firmware sets the pins before Windows starts |
| `ACPI\HISI0031` | `\_SB.UAR2` | RK3576 UART2 (no Windows driver) | inbox serial.sys handles byte-spaced 16550 registers only |

`RKCP0806` and `RKCP3005` need edk2-rk3576 with commits "give the RK806 PMIC
its own _HID" and "give the pin controller its own _HID" (2026-11-24).
Older firmware names both `PRP0001`, a generic ID this INF does not match,
so they keep their yellow mark there. The TaishanPi 3M firmware no longer
declares UART2 at all; the CM5-IO still does.

## Install (stage, mark, restart)

Sync the clock first (`w32tm /resync /force`): pnputil refuses a catalog
signed "in the future". Then, in an elevated PowerShell, in the unpacked CI
artifact `rk3576-null-arm64` (its own test certificate is in it):

```powershell
certutil -addstore Root rk3576-null-test.cer
certutil -addstore TrustedPublisher rk3576-null-test.cer
pnputil /add-driver rk3576-null.inf          # stage only, no /install
schtasks /create /tn null-reinstall /ru SYSTEM /sc once /st 00:00 /f `
  /tr "powershell -NoProfile -ExecutionPolicy Bypass -File $PWD\null-devices-reinstall.ps1"
schtasks /run /tn null-reinstall
Start-Sleep 5; Get-Content $env:SystemRoot\Temp\null-devices-reinstall.log
schtasks /delete /tn null-reinstall /f
shutdown /r /t 0
```

After the restart:

```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'HYMB0001|RKCP0806|RKCP3005|HISI0031' |
  Format-Table Status, Class, FriendlyName, InstanceId -AutoSize
```

Expected: each one `OK`, class `System`, with the name above.

Remove: `pnputil /delete-driver oemNN.inf /uninstall` (the oem name from
`pnputil /enum-drivers`), then restart; the devices go back to code 28.
