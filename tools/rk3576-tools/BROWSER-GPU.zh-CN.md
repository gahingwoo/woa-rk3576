# 让 Edge、Chrome 和 WebView2 用上 GPU

[English](BROWSER-GPU.md) | 简体中文

修好的 maliwddm（wddm-mali-g52 2df1808 或更新）上，Edge、Chrome 和 WebView2 不需要任何
开关。它们通过 DirectComposition 渲染，1440p 下 60 fps（泰山派 3M 和 CM5-IO，2026-11-27）。

`setup\rk3576-browser-gpu.ps1` 的第 1 版把 `--disable-direct-composition` 加到了它们的
启动入口上。这个开关从来没有起作用。加上它，Chromium 通过 ANGLE 的 blt 模型交换链呈现：
在旧的 maliwddm 上这些画面根本到不了屏幕（Edge 窗口一片白），在修好的驱动上只有 32 fps。
旧的 maliwddm 不加开关时会退回软件渲染，可以正常使用。测量结果见 wddm-mali-g52 的
`docs/EDGE-PRESENT.md`。

现在这个脚本只去掉第 1 版加上的东西，从不添加任何东西。

## 去掉什么

只处理程序是 `msedge.exe`、`chrome.exe`、`msedge_proxy.exe` 或 `chrome_proxy.exe` 的条目：

- 快捷方式中的开关：所有用户的开始菜单和桌面，每个用户的开始菜单、桌面和 Quick Launch
  （固定在任务栏上的图标）；
- 打开命令（`shell\<verb>\command`）中的开关：`HKLM\SOFTWARE\Classes` 和每个用户的
  classes 中的 `MSEdge*`、`microsoft-edge*`、`Chrome*`、`Chromium*` 和 `Applications\` 键，
  以及 `Clients\StartMenuInternet\*`；
- 本机和每个用户的 `Run` 值中的开关；
- 第 1 版设置的策略（Edge 的 `StartupBoostEnabled` 和 `BackgroundModeEnabled`，Chrome 的
  `BackgroundModeEnabled`）。每个值恢复成第 1 版保存的原值；如果之后管理员改过，就不动它；
- `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`（本机）和
  `HKLM\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments` 中的开关。

`Install` 和 `Uninstall` 还会删除第 1 版的计划任务 `\RK3576\Browser GPU`、它在
`%ProgramFiles%\RK3576\browser-gpu` 中的副本和它的设置 `HKLM\SOFTWARE\RK3576\BrowserGpu`。

## 命令

在管理员命令提示符中：

```
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Status
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Install
```

| 操作 | 作用 |
|---|---|
| `Status` | 列出第 1 版留下的东西 |
| `Install`、`Uninstall` | 去掉所有东西，包括没有登录的用户，并删除计划任务、副本和设置 |
| `Apply`、`Revert`、`Enable`、`Disable` | 去掉开关、策略和 WebView2 参数；为旧的调用方保留（第 1 版的计划任务运行 `Apply`） |

`-DryRun` 什么也不改。日志在 `%ProgramData%\RK3576\browser-gpu.log`。之后请关闭并重新打开浏览器。

## 由谁运行

- RK3576 工具包：`install.cmd` 和离线安装的首次启动运行 `-Action Install`；卸载时运行
  `-Action Uninstall`。
- Maskrom Studio 的 Windows 安装在 `SetupComplete.cmd` 中、工具包之后运行 `-Action Install`。

## 测试

CI（`.github/workflows/ci.yml` 中的 `browser-gpu` 作业）对按第 1 版留下的样子准备好的虚构
机器运行 [tests/browser-gpu.tests.ps1](tests/browser-gpu.tests.ps1)。在运行器上，它先把第 1 版的
开关加到真实的 Edge 和 Chrome 上，再检查第 1 版的计划任务运行本脚本、`Install`、`Revert` 和
`Uninstall` 都能把原来的状态还回去。
