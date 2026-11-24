# 安装 Windows

用本仓库的 WinPE U 盘，把 Windows 11 23H2 装到 ArmSoM CM5-IO 或立创泰山派 3M 的 eMMC 上。
[English](../INSTALL.md)（本页对应 2026-11-24 的英文版）

第一次接触这些板子？请从 [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/blob/main/README.zh-CN.md) 开始。
[Maskrom Studio](https://github.com/gahingwoo/maskrom-studio) 可以在电脑上通过 USB 完成同样的安装，不用 U 盘；截至 2026-11-24 还没有在板子上跑过。

用这个方法装过的：2026-10-01 在 CM5-IO 上装的 Windows 11 23H2 企业版（22631.2428，首次启动后做过原地修复）；2026-11-13 在泰山派 3M 上用当前脚本全新安装。

## 用哪个版本

RK3576 是 ARMv8.0。Windows 11 24H2 及以后的版本需要 ARMv8.1 原子指令，跑不了，固件也绕不过去。

| 目标 | 版本号 | |
|---|---|---|
| Windows 10 ARM64 22H2 | 19045 | 应该能用，没试过 |
| Windows 11 23H2 | 22631 | 可用，这里测试的就是它 |
| Windows 11 24H2 及以后 | 26100+ | 启动不了 |

`dism /Get-ImageInfo` 可以查看映像的版本号。安装脚本会拒绝 25999 以上的版本。

## 为什么不用 Windows 安装程序

23H2 的安装程序没有 eMMC 控制器的驱动，看不到 eMMC。它还会因为没有 TPM 2.0 和安全启动而拒绝这些板子，在
CM5-IO 上还会因为 eMMC 小于 64 GB 而拒绝。装好的系统从第一次启动起就需要测试签名模式，因为系统盘的驱动是测试签名的。
所以改用本仓库的 WinPE，通过 DISM 写入系统映像。

## 需要准备

- [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/releases/latest) 的固件，**以完整映像**写到 eMMC
  （这样才会建立脚本要检查的固件分区）。用浏览器：[flash.gahingwoo.com](https://flash.gahingwoo.com/)，选
  "UEFI firmware, whole image"。方法见 [edk2-rk3576 刷写固件](https://edk2.gahingwoo.com/zh-CN/flash.html)。
- 本仓库[最新正式版](https://github.com/gahingwoo/woa-rk3576/releases/latest)（0.1.1 或更新）的 WinPE 映像，以及一个装得下它和 `install.wim` 的 U 盘。
- Windows 11 23H2 ARM64 ISO 里的 `install.wim` 或 `install.esd`。
- 一个 USB 键盘。泰山派 3M 还需要 HDMI 显示器或立创的 DSI 屏。

不要混用旧版本。泰山派 3M 上 0.3.0 之前、CM5-IO 上 0.4.0 之前的固件，以及 0.1.0 的驱动，把 eMMC 跑在 148.5 MHz
而不是 52 MHz，Windows 下写入的文件可能读回来就变了，却不报任何错。

在泰山派 3M 上，触摸、蓝牙、耳机孔、HDMI 音频和 USB-C 需要 0.4.1 之后的固件：下一个正式版发布前，用 edk2-rk3576
的 [`ci` 构建](https://edk2.gahingwoo.com/zh-CN/flash.html#滚动的-ci-构建)。安装本身用 0.4.1 就可以。

装好后的布局：eMMC 第 1 分区是固件（不动），eMMC 其余部分是 Windows，NVMe（如果有）不动。

## 步骤

1. 把 WinPE 映像写进 U 盘。它只有一个 515 MB 的 FAT32 分区 `WINPE`，里面有 `woa-deploy\`、`woa-debug\` 和放已签名驱动的 `woa-drivers\`；U 盘其余空间未分配。
2. 在未分配的空间里建一个 exFAT 或 NTFS 分区，把 `sources\install.wim`（或 `.esd`）复制进去。FAT32 放不下它。
3. 取出板子上的 SD 卡。脚本看到不止一个 SD/eMMC 磁盘就会停下。
4. 把 U 盘插到 USB-A 口。（泰山派 3M 的 USB-C 口只对上电前插好的设备有效。）
5. 上电，打开固件的启动菜单，从 U 盘启动。
6. 等信息收集程序跑完，它会显示 U 盘的盘符。
7. 用这个盘符运行：

   ```
   E:\woa-deploy\deploy-windows.cmd
   ```

8. 提示时输入 `ERASE`。
9. 完成后拔掉 U 盘，重启。

Windows 会完成初始设置（不联网也能完成），进入桌面，右下角显示"测试模式"。

脚本日志在 U 盘的 `woa-deploy\deploy.log`。

## 脚本做了什么

1. 找到 eMMC（唯一一个 28 到 33 GB 或 57 到 63 GB 的 `SD\` 磁盘），检查它的第一个分区是固件（偏移 32768，长度 67076096）。不符合就停下，什么也不写。
2. 找到映像，列出其中的版本，拒绝非 ARM64 的映像和 25999 以上的版本。
3. 要求你输入 `ERASE`，删除第一个分区之后的所有分区，新建 ESP、MSR 和一个 NTFS 分区。
4. 用 `/Compact` 写入映像，加入驱动，运行 `bcdboot`，打开测试签名，把 `dwcsdhc` 和 `sdstor` 设为启动时加载，禁用 `sdbus`，关闭休眠和快速启动，并设置 `BypassNRO`，让初始设置不联网也能完成。

## 第一次启动

固件会先试 SD 卡槽和 USB，再从 eMMC 的 ESP 启动 `\EFI\Boot\bootaa64.efi`；除非打开了 "Save OS Variable Changes to eMMC"，`bcdboot` 写的启动项不会保存到固件里。

第一次启动后：

- 按[故障排查](troubleshooting.md#每次启动后时间都不对)设置时钟（泰山派 3M）；
- 按需加装驱动：[硬件功能](features.md)，GPU 驱动见 [wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)。

`tools/woa-deploy/add-drivers.cmd` 可以在 WinPE 下给已安装的系统添加或更新驱动，不会清除任何东西。其他方法见[驱动](drivers.md)。

## 各板说明

### ArmSoM CM5-IO

- eMMC 是 29 GB。
- Windows 能看到 NVMe，脚本不会动它。

### 立创泰山派 3M

- eMMC 是 62008590336 字节；0.1.1 及以后的 WinPE 映像能识别它。
- 固件必须在 eMMC 上。固件放在 SD 卡上时，脚本会看到两个 SD/eMMC 磁盘而停下。
- 写入固件完整映像会替换立创的出厂系统，要恢复出厂系统得重新写立创自己的固件。
- 以后在已装好 Windows 的板子上更新固件，选"UEFI firmware, keep the partitions"。写完整映像会清掉分区表。
- 接着立创的 DSI 屏时，固件用屏幕而不是 HDMI，Windows 也在屏幕上以竖屏显示（装 wddm-mali-g52 后可横屏）。2026-11-13 那次是通过 HDMI 安装的；想照做，先断电拔下屏幕排线。
- WinPE 和第一次设置时，手边准备一个 USB 键盘。

启动不了时：见[故障排查](troubleshooting.md)。
