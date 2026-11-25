# 让 Edge、Chrome 和 WebView2 用上 GPU

[English](BROWSER-GPU.md) | 简体中文

在 WDDM 2.x 下，Chromium 的 DirectComposition 呈现在 maliwddm 上会失败。GPU
进程崩溃三次后，浏览器退回软件渲染。加上 `--disable-direct-composition`
启动时，Edge 158 和 Chrome 158（ARM64）通过 ANGLE D3D11（功能级别 10_1）在
Mali-G52 上渲染，GPU 进程不崩溃（已在泰山派 3M 上板测试）。

浏览器进程也读这个开关，所以 maliwddm 自己关不掉 DirectComposition（见
wddm-mali-g52 的 `docs/EDGE-PRESENT.md`），两个浏览器也都没有对应的策略。
`setup\rk3576-browser-gpu.ps1` 把开关加到每一个启动入口上，并一直保持。

## 改了什么

只改程序是 `msedge.exe`、`chrome.exe`、`msedge_proxy.exe` 或
`chrome_proxy.exe` 的条目，而且只在开关还没有的时候改。

| 位置 | 内容 |
|---|---|
| 快捷方式：所有用户的开始菜单和桌面；每个用户的开始菜单、桌面（包括被重定向的桌面）和 Quick Launch，其中有固定在任务栏上的图标（`User Pinned\TaskBar`）和 `ImplicitAppShortcuts` | 开关加在快捷方式参数的最前面 |
| `HKLM\SOFTWARE\Classes` 和每个用户的 classes 中的打开命令：键 `MSEdge*`、`microsoft-edge*`、`Chrome*`、`Chromium*`、`Applications\msedge.exe`、`Applications\chrome.exe`，以及 `Clients\StartMenuInternet\*`（本机和用户） | 每个 `shell\<verb>\command` 在程序路径之后、`--single-argument` 之前加上开关。Chromium 把 `--single-argument` 之后的所有内容当成一个 URL。值的类型（REG_SZ 或 REG_EXPAND_SZ）保持不变 |
| 本机和每个用户的 `Run` 值 | 同上，只改启动浏览器的值。Edge 的启动增强就是从这里启动 Edge 的 |
| 策略 `HKLM\SOFTWARE\Policies\Microsoft\Edge` 的 `StartupBoostEnabled` 0 和 `BackgroundModeEnabled` 0，`HKLM\SOFTWARE\Policies\Google\Chrome` 的 `BackgroundModeEnabled` 0 | 这样就不会有不带开关启动的浏览器实例还在运行。新窗口会加入正在运行的实例，用的是那个实例的开关。每个值原来的状态会先保存，还原时放回去；如果之后管理员改过这个值，就不动它 |
| 本机环境变量 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`，以及它的注册表后备 `HKLM\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments` 中的值 `*` | 开关追加在原有内容后面。微软的 WebView2 参考（`CreateCoreWebView2EnvironmentWithOptions`）说明，来自环境变量或注册表的参数会追加到应用自己传入的参数之后，所以应用自己的开关不受影响。环境变量没有设置时才读注册表，所以用旧环境块启动的 WebView2 也能拿到开关。如果用户自己设了同名变量，对该用户来说本机变量就被盖住了 |

只要设置了任何一条策略，Edge 和 Chrome 就会显示"由你的组织管理"。Maskrom
Studio 的 Windows 精简本来就会设置那两条 Edge 策略。`Policies` = 0（见下文）会让
这三条策略都不被改动。

其他东西一概不动：不改浏览器文件、用户配置文件或偏好设置。

## 什么时候运行

计划任务 `\RK3576\Browser GPU` 以 SYSTEM 身份运行 `-Action Apply`：

- 每次开机后 1 分钟；
- 任何用户登录后 30 秒（这时该用户的注册表和快捷方式已经加载；Windows 在第一次
  登录时创建默认的任务栏图标）；
- 每小时一次；
- MSI 安装、修复或卸载（Chrome 的 MSI）之后 2 分钟，或者应用程序日志中出现
  `edgeupdate`、`gupdate` 或 `GoogleUpdater` 提供者的事件之后 2 分钟。这些更新
  程序是否真的写这类事件，还没在板子上确认过；如果不写，每小时一次的运行也能补上。

浏览器更新会重写它的快捷方式和打开命令。下一次运行会把开关加回去；如果一切都已就位，
运行时什么也不改，也不写日志。在更新之后、下一次运行之前打开的浏览器会用软件渲染，
直到重新启动。

默认模式下的判断条件：`ACPI\RKCP7402` 由 `maliwddm` 驱动，设备管理器里没有报错，
并且它的 `Device Parameters\maliwddm\WddmVersion` 不小于 0x2000。如果明确不满足
（没有这个设备、换了驱动、有错误代码，或者回退到了 WDDM 1.3），开关会被重新去掉。
WDDM 1.3 下 Edge 不需要这个开关。如果状态读不出来（WMI 没有响应，或者还没有
`WddmVersion`），什么也不改。

## 设置

`HKLM\SOFTWARE\RK3576\BrowserGpu`（都是可选的）：

| 值 | 含义 |
|---|---|
| `Mode` DWORD | 0 关闭（下次运行时全部去掉），1 自动（默认），2 总是（不判断） |
| `Policies` DWORD | 0：不改 Edge 和 Chrome 的策略 |
| `WebView2` DWORD | 0：不改 WebView2 |
| `LastRun`、`LastGate`、`Applied`、`Version`、`Saved\` | 由脚本写入 |

每个用户可以设置 `HKCU\Software\RK3576\BrowserGpu` 的 `Mode` = 0，这样该用户自己的
快捷方式、classes 和 Run 值都不加开关。

## 命令

在管理员命令提示符中，进入 `%ProgramFiles%\RK3576\browser-gpu`：

```
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Status
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Apply -DryRun
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Disable
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Enable
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Uninstall
```

`Status` 显示模式、判断结果，以及每个带开关和不带开关的条目。`-DryRun` 可以和任何
操作一起用，什么也不改。`Disable` 把 `Mode` 设为 0 并去掉所有开关，之后计划任务也
保持不加。`Uninstall` 去掉所有开关，包括没有登录的用户（会临时加载他们的配置单元），
并删除计划任务、脚本副本和设置。日志在 `%ProgramData%\RK3576\browser-gpu.log`。

## 由谁安装

- RK3576 工具包：`install.cmd` 和离线安装的首次启动步骤会运行 `-Action Install`；
  卸载时先运行 `-Action Uninstall`。所以在已有的 Windows 上运行 `install.cmd`
  也会启用它。
- Maskrom Studio 的 Windows 安装自带一份脚本，不论是否选择了工具包，都会在
  `SetupComplete.cmd` 中运行 `-Action Install`。

两者都把脚本放到 `%ProgramFiles%\RK3576\browser-gpu\`，注册同一个计划任务，所以
只有一份在运行。安装时保留两份中 `$BgVersion` 较高的那一份。工具包安装会替换
`%ProgramFiles%\RK3576`，然后重新装上工具包自带的那一份。

## 不覆盖的情况

- 直接运行 `.exe` 启动的浏览器（资源管理器、运行框、没有快捷方式的便携版 Chrome），
  或者由其他程序用自己的命令行启动的浏览器。它们用软件渲染。桌面或开始菜单上的
  快捷方式都覆盖，不论目标在哪里。
- 上面列出的文件夹以外的快捷方式，比如另一个驱动器上的快捷方式。
- 加开关时已经在运行的浏览器。它在下次启动时才带上开关。

## 测试

CI（`.github/workflows/ci.yml` 中的 `browser-gpu` 作业）在 Windows PowerShell 5.1 和
PowerShell 7 下，对 `HKCU\Software\RK3576BgTest` 下虚构的本机和用户运行
[tests/browser-gpu.tests.ps1](tests/browser-gpu.tests.ps1)，然后在运行器自带的 Edge 和
Chrome 上安装、应用（`Mode` 2）、Disable、Uninstall，并检查没有留下任何东西。

在板子上（泰山派 3M，WDDM 2.0）：

1. `Status`：判断为 Yes，`WddmVersion` 0x2000。
2. 从任务栏启动 Edge，打开 `edge://gpu`："Compositing: Hardware accelerated"，
   命令行中有 `--disable-direct-composition`。Chrome 也一样（`chrome://gpu`），
   从开始菜单启动，以及从其他程序打开一个链接（打开命令）。
3. 重新把 Chrome 固定到任务栏，等每小时一次的运行或者手动运行计划任务
   （`schtasks /run /tn "\RK3576\Browser GPU"`），然后从新的图标启动 Chrome。
4. `Disable`：所有地方的开关都去掉了（`Status`）；`Enable` 再加回来。
