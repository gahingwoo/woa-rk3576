# pin40: GPIO and I2C from PowerShell

English | [简体中文](README.zh-CN.md)

Two Windows PowerShell 5.1 scripts for the 40-pin header through
`Windows.Devices.Gpio` and `Windows.Devices.I2c` (RhProxy). They need the
firmware's "40-pin GPIO/I2C/SPI for apps" option and the pin numbers and
bus names in [docs/pin40.md](../../docs/pin40.md).

```
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -List
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 0 -Count 10 -PeriodMs 500
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 3 -Read
powershell -ExecutionPolicy Bypass -File i2c-scan.ps1
powershell -ExecutionPolicy Bypass -File i2c-scan.ps1 -Bus I2C7
```

`gpio-blink.ps1` leaves the pin as an input when it ends. `i2c-scan.ps1`
lists the I2C and SPI buses, then reads one byte from every 7-bit address
of one bus. Use `powershell.exe`, not PowerShell 7: PowerShell 7 cannot
load WinRT types.
