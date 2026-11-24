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
| `VERSION.txt` | 这个包的版本，以及构建它所用的 commit | CI |

woa-rk3576 每次 push，CI 都会构建它，产物名为 `rk3576-tools-arm64`：
rk-monitor 来自本仓库，rknputest 和 charsiu_run 来自 woa-rk3576-npu，用的是 `.github/workflows/ci.yml` 里
`NPU_REF` 指定的 commit。

## 在运行中的 Windows 上安装

把产物解压到任意位置，运行 `install.cmd`（会请求管理员权限）。它会：

- 把 `RK3576\` 复制到 `%ProgramFiles%\RK3576`（再运行一次就是原地升级；正在运行的 rk-monitor 会先被停止，之后自动重新启动）；
- 让 rk-monitor 在每次登录时为所有用户启动，并打开桌面小部件（每个用户都可以在托盘菜单里关掉它）。这就是任务
  `\RK3576\RK3576 Monitor`：任何用户登录时触发，以该用户身份运行（Users 组，不提权，只在已登录时运行），没有延迟。
  如果用 Run 项，要等 Explorer 的启动延迟，大约 30 秒。
  用这种方式启动的 rk-monitor 会等 5 秒再取第一个样本，并在第一分钟以后台模式运行，所以不会给会话刚开始时的繁忙再添负担；
  托盘图标和小部件会立刻出现，在第一个样本之前显示 "..."；
- 添加 `Start menu\Programs\RK3576`：RK3576 Monitor、RK3576 tools folder、Uninstall RK3576 tools；并在 Settings、Apps
  （设置、应用）里添加一项；
- 用 lodctr 根据驱动存储中的清单注册 GPU（`RK3576 GPU`，maliwddm）和 NPU（`RK3576 NPU`，rknpu）性能计数器，并添加任务
  `\RK3576\Performance counters`（SYSTEM，每次开机 30 秒后），在驱动更新移动了它们之后重新注册。没装的驱动会被跳过，
  等以后某次开机再补上。

`install.cmd /quiet /nostart` 安装时不暂停，也不立即启动 rk-monitor。日志：`%ProgramData%\RK3576\setup.log`。

## 卸载

Settings、Apps、RK3576 tools（设置、应用）；或开始菜单里的 Uninstall RK3576 tools；或
`uninstall.cmd`（在 `install.cmd` 旁边，或在 `%ProgramFiles%\RK3576\setup` 里）。
它会删除安装时添加的所有东西：文件夹、快捷方式、登录任务（以及离线安装的 Run 项）、`HKLM\SOFTWARE\RK3576`、
应用列表里的那一项、计数器任务和任务文件夹、它所做的计数器注册、`%ProgramData%\RK3576`，以及每个用户的
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

## 在板子上测试

1. 运行 `install.cmd`。托盘图标和小部件（主屏幕右上角）出现。开始菜单的 RK3576 下有三项。
2. `Get-ScheduledTask -TaskPath \RK3576\` 列出两个任务；
   `Get-Counter -ListSet "RK3576 NPU","RK3576 GPU"` 列出两组计数器；
   `%ProgramData%\RK3576\counters.log` 写明注册了什么。
3. 按 Win+D：小部件还在。拖动它，注销再登录：它回到原来的位置。托盘菜单里打开 Widget click-through（小部件点击穿透）：点击会落到桌面上。
4. 重启并登录：桌面出现后几秒内 rk-monitor 就在了（托盘图标和小部件，约 5 秒内显示 "..."），小部件是打开的；
   `reg query HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` 里没有 "RK3576 Monitor"。`counters.log` 里每个驱动多了一行
   "already registered"。
5. 驱动更新（暂存一个新的 rknpu 或 maliwddm 驱动包，设置重装标志，重启）：`counters.log` 显示注册了新的驱动存储文件夹，
   rk-monitor 的 NPU 和 GPU 行仍然有数字。
6. 运行 `uninstall.cmd`：文件夹、开始菜单文件夹、任务和计数器组都没了；`reg query HKLM\SOFTWARE\RK3576` 什么也查不到。
