# null：给 Windows 不需要驱动的设备起名字

[English](README.md) | 简体中文

`rk3576-null.inf` 给四个 ACPI 设备安装“空驱动”，这样设备管理器会在 *System devices*（系统设备）下按名字列出它们，
而不是显示成带黄色感叹号的未知设备（问题代码 28）。没有二进制文件，也没有服务：安装方式是文档里写明的空驱动写法
（`AddService = ,2`，不写服务名），Windows 只记录这些设备的资源。

| 硬件 ID | ACPI 设备 | 设备管理器里的名字 | 为什么不需要驱动 |
|---|---|---|---|
| `ACPI\HYMB0001` | `\_SB.I2C2.RTC0` | HYM8563 real-time clock (not used by Windows) | Windows 通过 UEFI 运行时服务读取时间，固件用这颗芯片提供这项服务 |
| `ACPI\RKCP0806` | `\_SB.I2C1.PMC0` | RK806 power management IC (managed by the firmware) | 电源轨和关机都由固件负责 |
| `ACPI\RKCP3005` | `\_SB.PINC` | RK3576 pin controller (managed by the firmware) | 固件在 Windows 启动前就设置好了引脚 |
| `ACPI\HISI0031` | `\_SB.UAR2` | RK3576 UART2 (no Windows driver) | 系统自带的 serial.sys 只能处理按字节排列的 16550 寄存器 |

`RKCP0806` 和 `RKCP3005` 需要带有 "give the RK806 PMIC its own _HID" 和 "give the pin controller its own _HID"
这两个提交（2026-11-24）的 edk2-rk3576。旧固件把它们都叫做 `PRP0001`，这是一个通用 ID，本 INF 不匹配它，
所以在旧固件上它们仍然带黄色感叹号。泰山派 3M 固件已经完全不再声明 UART2；CM5-IO 还在声明。

## 安装（暂存、标记、重启）

先同步时钟（`w32tm /resync /force`）：pnputil 会拒绝签名时间“在未来”的 catalog。然后在管理员 PowerShell 里，
进入解压后的 CI 构建产物 `rk3576-null-arm64`（它自己的测试证书就在里面）：

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

重启之后：

```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'HYMB0001|RKCP0806|RKCP3005|HISI0031' |
  Format-Table Status, Class, FriendlyName, InstanceId -AutoSize
```

预期结果：每个都是 `OK`，类别 `System`，名字和上表一致。

卸载：`pnputil /delete-driver oemNN.inf /uninstall`（oem 名字从 `pnputil /enum-drivers` 查），然后重启；这些设备会变回代码 28。
