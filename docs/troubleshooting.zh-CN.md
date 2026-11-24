# Windows 故障排查

[English](troubleshooting.md) | 简体中文

固件方面的问题（MaskROM、刷写、设置）见 edk2-rk3576 的[故障排查](https://edk2.gahingwoo.com/troubleshooting.zh-CN.html)。要从 Windows 收集数据，见[调试](dev/debugging.md)（英文）。

## INACCESSIBLE_BOOT_DEVICE（0x7B）

第二个参数是 `0xC0000034` 时，表示 Windows 找不到系统盘的驱动。安装脚本已经处理了目前遇到的三种原因；如果用别的方法装的系统，逐一检查：

- `dwcsdhc` 必须是启动时加载。DISM 离线时只会安装启动时加载的驱动，其他的只是暂存。
- `sdstor` 必须是启动时加载。它默认是按需启动，DISM 不会像安装程序那样改它。
- `sdbus` 必须禁用。eMMC 的 `_CID` 是 `PNP0D40`，sdbus 会匹配它，而 PnP 一旦把控制器绑到 sdbus 上就会一直保持。如果已经绑上了，删除 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0D40` 以及 `{a0a588a4-...}` 类键下对应的项。

## "计算机意外地重新启动"

第一次启动崩溃，打断了 specialize 阶段。

1. 按 Shift+F10。
2. 打开 `regedit`。
3. 把 `HKLM\SYSTEM\Setup\Status\ChildCompletion\setup.exe` 设为 3。
4. 关闭 regedit，点确定。

## 关机要好几分钟

快速启动开着：它会休眠，而休眠经过 eMMC 驱动时会触发 WHEA 错误。运行 `powercfg /h off`。安装脚本和 `net-debug.cmd` 已经这样做了。

## 每次启动后时间都不对

泰山派 3M 的 RTC 里没有有效时间。

1. 把 `HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation\RealTimeIsUniversal` 设为 1（REG_DWORD），让 Windows 把固件时钟当作 UTC。
2. 把 Windows Time 服务设为自动启动。
3. 插着网线：固件启动时会通过 NTP 校时。

时间没校准之前，`pnputil` 会拒绝签名时间晚于板子当前时间的驱动（"not within its validity period"）；先运行 `w32tm /resync /force`。

## Windows 安装程序看不到 eMMC

这是正常的：安装程序没有 eMMC 控制器的驱动。请用 WinPE U 盘安装（见[安装](INSTALL.zh-CN.md)）。

## 用 Ventoy 做的 WinPE U 盘启动不了

不要用 Ventoy 装这个映像：它的 UDF 驱动出现了同步异常。请直接把映像写进 U 盘。

## 安装脚本在写入前停下了

- "more than one SD/eMMC disk"：取出 SD 卡，并且固件要放在 eMMC 上，不要放在卡上。
- 第一个分区不是固件：先把固件完整映像写到 eMMC。
- 映像被拒绝：不是 ARM64，或者版本号大于 25999（24H2 及以后跑不了）。

## 重装音频驱动后没声音

检查任务栏声音菜单里的默认播放设备（见[驱动](drivers.zh-CN.md#替换已经绑定并在运行的驱动)）。
