# RK3576 工具

[English](README.md) | 简体中文

RK3576 板子在 Windows on Arm 下给用户用的程序，打成一个包，安装到 `%ProgramFiles%\RK3576`：

| 文件 | 是什么 | 来自 |
|---|---|---|
| `rk-monitor.exe` | 托盘监视器和桌面小部件：CPU、GPU 和 NPU 占用，温度（[tools/rk-monitor](../rk-monitor/README.zh-CN.md)） | woa-rk3576 |
| `npu\rknputest.exe` | NPU 驱动的测试工具（`rknputest info`、`rknputest m6`） | woa-rk3576-npu |
| `npu\charsiu_run.exe` | 在 NPU 上运行语言模型（gguf） | woa-rk3576-npu（charsiu，为 Windows 构建） |
| `npu\rknpu-perfcounters.ps1` | 手动注册 NPU 计数器（安装程序会自动做这一步） | woa-rk3576-npu |
| `setup\` | 安装程序的脚本，保留下来用于首次启动、计数器任务和卸载 | woa-rk3576 |
| `setup\rk3576-browser-gpu.ps1` | WDDM 2.x 下让 Edge、Chrome 和 WebView2 用 GPU（[BROWSER-GPU.zh-CN.md](BROWSER-GPU.zh-CN.md)）；安装到 `browser-gpu\` | woa-rk3576 |
| `VERSION.txt` | 这个包的版本，以及构建它所用的 commit | CI |

woa-rk3576 每次 push，CI 都会构建它，产物名为 `rk3576-tools-arm64`：
rk-monitor 来自本仓库，rknputest 和 charsiu_run 来自 woa-rk3576-npu，用的是 `.github/workflows/ci.yml` 里
`NPU_REF` 指定的 commit。

## 在运行中的 Windows 上安装

把产物解压到任意位置，运行 `install.cmd`（会请求管理员权限）。它会：

- 把 `RK3576\` 复制到 `%ProgramFiles%\RK3576`（再运行一次就是原地升级；正在运行的 rk-monitor 会先被停止，之后自动重新启动）；
- 让 rk-monitor 在每次登录时为所有用户启动，并打开桌面小部件（每个用户都可以在托盘菜单里关掉它），通过任务
  `\RK3576\RK3576 Monitor`（任何用户登录时触发，以该用户身份运行，不提权，没有延迟）。第一个样本之前约 5 秒显示 "..."；
- 添加 `Start menu\Programs\RK3576`：RK3576 Monitor、RK3576 tools folder、Uninstall RK3576 tools；并在 Settings、Apps
  （设置、应用）里添加一项；
- 用 lodctr 根据驱动存储中的清单注册 GPU（`RK3576 GPU`，maliwddm）和 NPU（`RK3576 NPU`，rknpu）性能计数器，并添加任务
  `\RK3576\Performance counters`（SYSTEM，每次开机 30 秒后），在驱动更新移动了它们之后重新注册。没装的驱动会被跳过，
  等以后某次开机再补上。
- 让 Edge、Chrome 和 WebView2 在 GPU 运行 WDDM 2.x 时用 GPU 渲染：`setup\rk3576-browser-gpu.ps1 -Action Install`
  把 `--disable-direct-composition` 加到它们的快捷方式、打开命令和 WebView2 的参数里，任务 `\RK3576\Browser GPU`
  在浏览器更新后把它重新加上（[BROWSER-GPU.zh-CN.md](BROWSER-GPU.zh-CN.md)）。

`install.cmd /quiet /nostart` 安装时不暂停，也不立即启动 rk-monitor。日志：`%ProgramData%\RK3576\setup.log`。

## 卸载

Settings、Apps、RK3576 tools（设置、应用）；或开始菜单里的 Uninstall RK3576 tools；或
`uninstall.cmd`（在 `install.cmd` 旁边，或在 `%ProgramFiles%\RK3576\setup` 里）。
它会删除安装时添加的所有东西：文件夹、快捷方式、登录任务（以及离线安装的 Run 项）、`HKLM\SOFTWARE\RK3576`、
应用列表里的那一项、计数器任务和任务文件夹、它所做的计数器注册、浏览器 GPU 开关（先从所有快捷方式和命令中去掉，策略恢复原状）、`%ProgramData%\RK3576`，以及每个用户的
rk-monitor 设置（`HKCU\Software\RK3576` 和每用户的 Run 项；没有登录的用户通过其配置文件的 `NTUSER.DAT` 处理）。
驱动不会被动到。

## 离线安装（给映像安装程序用）

用于没有在运行的 Windows：从 WinPE 写到磁盘上的映像，或者挂载的 `install.wim`。Maskrom Studio 安装
Windows 时用的就是这个。`offline-install.cmd` 只需要 cmd 和 reg.exe（WinPE 没有 PowerShell）：

```
offline-install.cmd W:\ /hook
```

`W:\` 是新 Windows 的根目录。如果安装程序想自己做同样的事，接口如下：

**1. 文件。** 把包里的 `RK3576\` 文件夹原样复制到 `<root>\Program Files\RK3576\`。

**2. 注册表**，写在离线的 `SOFTWARE` 配置单元里
（`<root>\Windows\System32\config\SOFTWARE`，例如 `reg load HKLM\X ...`）：

| 键（相对于配置单元根） | 值 | 类型 | 数据 |
|---|---|---|---|
| `Microsoft\Windows\CurrentVersion\Run` | `RK3576 Monitor` | REG_EXPAND_SZ | `"%ProgramFiles%\RK3576\rk-monitor.exe" --autostart` |
| `RK3576\Monitor` | `Widget` | REG_DWORD | `1` |
| `RK3576` | `InstallDir` | REG_EXPAND_SZ | `%ProgramFiles%\RK3576` |
| `RK3576` | `Version` | REG_SZ | `VERSION.txt` 的第一行 |
| `RK3576` | `FirstBootPending` | REG_DWORD | `1`（首次启动步骤会删掉它） |
| `Microsoft\Windows\CurrentVersion\Uninstall\RK3576Tools` | `DisplayName` `RK3576 tools`、`DisplayVersion`、`Publisher` `woa-rk3576`、`InstallLocation`、`DisplayIcon`、`UninstallString` `"%ProgramFiles%\RK3576\setup\uninstall.cmd"`、`NoModify` 1、`NoRepair` 1 | | |

数据里的 `%ProgramFiles%` 是原样写入的（REG_EXPAND_SZ），所以不管安装时映像是哪个盘符，值都是对的。
Run 项只负责首次启动步骤运行之前的那段时间：那一步会创建登录任务 `\RK3576\RK3576 Monitor`，设置 `RK3576\Monitor`
`AutostartTask` = 1，并删除 Run 项。

**3. 首次启动。** 在 Windows 安装完成后，以 SYSTEM 身份运行一次：

```
call "%ProgramFiles%\RK3576\setup\firstboot.cmd"
```

最合适的位置是 `<root>\Windows\Setup\Scripts\SetupComplete.cmd`。
`/hook` 会把这一行追加进去（文件不存在就创建）。如果安装程序在调用 `offline-install.cmd` 之后还要写自己的
`SetupComplete.cmd`（Maskrom Studio 会用自己的覆盖它），就不要加 `/hook`，把这一行写进它自己的文件里。
这一步会添加快捷方式、登录任务和计数器任务，并注册计数器；它总是以 0 退出，日志写到 `%ProgramData%\RK3576`，
它没做成的部分，在运行中的系统上执行 `install.cmd` 就能修好。

`offline-install.cmd` 的退出码：成功为 0；用法错误为 1；`<root>` 不是 Windows 或包不完整为 2；复制或写注册表失败为 3
（配置单元会被重新卸载）。

## 验证

| 检查 | 预期 |
|---|---|
| 运行 `install.cmd` 之后 | 托盘图标和小部件（主屏幕右上角）；开始菜单 > RK3576 下有三项 |
| `Get-ScheduledTask -TaskPath \RK3576\` | 上面列出的任务 |
| `Get-Counter -ListSet "RK3576 NPU","RK3576 GPU"` | 两组计数器；`%ProgramData%\RK3576\counters.log` 写明注册了什么 |
| 重启并登录 | 几秒内出现 rk-monitor；`HKLM\...\CurrentVersion\Run` 里没有 "RK3576 Monitor" |
| 驱动更新之后 | `counters.log` 显示新的驱动存储文件夹；rk-monitor 的 NPU 和 GPU 行仍然有数字 |
| 运行 `uninstall.cmd` 之后 | 文件夹、开始菜单文件夹、各任务和计数器组都没了；`reg query HKLM\SOFTWARE\RK3576` 什么也查不到 |
