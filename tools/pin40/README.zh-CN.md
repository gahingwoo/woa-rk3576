# pin40：用 PowerShell 操作 GPIO 和 I2C

[English](README.md) | 简体中文

两个 Windows PowerShell 5.1 脚本，通过 `Windows.Devices.Gpio` 和 `Windows.Devices.I2c`（RhProxy）
使用 40 针排针。需要打开固件的 "40-pin GPIO/I2C/SPI for apps" 选项；引脚号和总线名见
[docs/pin40.zh-CN.md](../../docs/pin40.zh-CN.md)。

```
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -List
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 0 -Count 10 -PeriodMs 500
powershell -ExecutionPolicy Bypass -File gpio-blink.ps1 -Pin 3 -Read
powershell -ExecutionPolicy Bypass -File i2c-scan.ps1
powershell -ExecutionPolicy Bypass -File i2c-scan.ps1 -Bus I2C7
```

`gpio-blink.ps1` 结束时把引脚改回输入。`i2c-scan.ps1` 先列出 I2C 和 SPI 总线，再从一条总线的每个
7 位地址读一个字节。请用 `powershell.exe`，不要用 PowerShell 7：PowerShell 7 加载不了 WinRT 类型。
