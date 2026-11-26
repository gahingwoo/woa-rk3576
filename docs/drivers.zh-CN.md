# 添加和替换驱动

[English](drivers.md) | 简体中文

怎样把驱动装进这些板子上已安装的 Windows，以及怎样替换一个正在运行的驱动。本仓库、
[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) 和
[woa-rk3576-npu](https://github.com/gahingwoo/woa-rk3576-npu) 的驱动都按这些规则来。

## 一次装全部

[驱动包](../tools/rk3576-drivers/README.zh-CN.md)（发布页上的 `rk3576-drivers-arm64.zip`）里有三块板子的全部驱动，
它的 `install.cmd` 会装好这块板子要的那些。

## 先校准时钟

先同步时间（`w32tm /resync /force`）。泰山派 3M 的 RTC 里没有有效时间，pnputil 会拒绝"签名时间在未来"的驱动目录，
提示"a required certificate is not within its validity period"。见[故障排查](troubleshooting.zh-CN.md#每次启动后时间都不对)。

## 在 WinPE 下离线安装

用 WinPE U 盘启动，运行 `woa-deploy\add-drivers.cmd`。它会把 U 盘上的驱动添加或更新到已安装的 Windows 里，不清除任何东西。

## 在运行中的 Windows 里安装

驱动要取自 WinPE 产物的 `woa-drivers\`（或驱动项目自己的已签名包），不要用 CI 构建产物：只有 WinPE 工作流会给驱动目录签名。

每次 WinPE 构建都用新的测试证书签名，所以要先信任这个证书：

1. 从驱动目录里取出证书。在 Linux 上：

   ```bash
   openssl pkcs7 -inform DER -in gt9271.cat -print_certs -out signer.pem
   openssl x509 -in signer.pem -outform DER -out signer.cer
   ```

2. 在板子上，用管理员命令提示符：

   ```
   certutil -addstore -f Root signer.cer
   certutil -addstore -f TrustedPublisher signer.cer
   ```

3. 暂存驱动包，然后重启：

   ```
   pnputil /add-driver gt9271.inf
   shutdown /r /t 0
   ```

   还没有驱动的设备会在下次启动时用上暂存的驱动包。在运行中的板子上不要用 `/install`。

不要在运行中的 Windows 里重装 SD/eMMC 主机驱动（`dwcsdhc`、`dwcmshc`）：下面的方法也不行：eMMC 板上的 Windows 就运行在它们上面。请从 WinPE 离线更新。

## 替换已经绑定并在运行的驱动

对已经有驱动的设备，`pnputil /add-driver` 之后重启，Windows 仍会保留旧驱动；而对正在运行的音频、DMA、显示或蓝牙驱动用 `/install`，曾让泰山派 3M 蓝屏。正确做法：

1. 只暂存驱动包：`pnputil /add-driver <名字>.inf`。
2. 以 SYSTEM 身份，在设备的 `HKLM\SYSTEM\CurrentControlSet\Enum\<实例>` 键下，把 `ConfigFlags` 的 0x20 位（`CONFIGFLAG_REINSTALL`）置 1。
3. 重启。设备会用最新暂存的驱动包启动。

在板子上总结出的规则：

- `DriverVer` 没变时，pnputil 会保留旧的二进制文件。请安装 `DriverVer` 更新的版本。
- 不要用 `pnputil /delete-driver /uninstall` 强行替换：对 es8323 这样做，服务被设成了禁用，`.sys` 也被删了。
- 重装音频驱动后，检查任务栏声音菜单里的默认播放设备。Windows 可能还指向上一次安装留下的、已经不存在的端点，结果什么也播不出来。
- 删掉不再使用的暂存驱动包（`pnputil /enum-drivers`，然后 `pnputil /delete-driver oemNN.inf`）：在泰山派 3M 的 eMMC 上它们很快就会占满空间。
