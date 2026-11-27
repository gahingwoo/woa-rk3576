# 回归测试套件

[English](README.md) | 简体中文

每块板子一条命令，每项检查一行 PASS / FAIL / SKIP / N-A，覆盖三块 RK3576 板子：立创泰山派 3M、ArmSoM CM5-IO 和
Radxa ROCK 4D。`regress.ps1` 在 Windows 11 23H2 下运行，`regress.sh` 在 Linux（Armbian、Fedora，设备树启动）下运行，
`run-remote.sh` 在电脑上通过 SSH 运行其中之一，并把结果拷回来。

套件只读。它不安装也不卸载任何东西，不改驱动、固件和启动设置，也从不重启或关闭板子，所以随时都可以跑。它会写入的只有：
结果文件夹、存储检查用的一个临时文件（用完即删），以及在 Windows 上为需要桌面的检查给已登录用户建的一个一次性计划任务
（用完即删）。唯一的重负载检查 FurMark 只在加了 `-Stress` 时运行。

## 运行

在电脑上（板子运行 OpenSSH；Windows 上以 PowerShell 作为 shell，使用管理员账户并开启自动登录）：

```sh
tools/regress/run-remote.sh woo@tspi3m.lan             # Windows，除 -Stress 外的全部检查
tools/regress/run-remote.sh woo@cm5io.lan -Quick       # 只跑短的检查
tools/regress/run-remote.sh woo@tspi3m.lan -Stress -StressSeconds 300
tools/regress/run-remote.sh root@rock4d.lan --quick    # Linux（自动识别）
```

结果保存在 `tools/regress/results/<主机>-<时间>/`（`results.md`、`results.json` 以及各工具的输出）。退出码就是套件的
退出码：0 没有 FAIL，1 至少一个 FAIL，2 套件无法运行（未提权、未知板子），3 SSH 或拷贝失败。Windows 运行前会先获取
解码测试码流（`fetch-streams.sh`）并一起拷过去；放在 `tools/regress/bin/` 里的工具（例如 `vdpu383test.exe`）也会一起拷。

直接在板子上以管理员身份运行：

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File regress.ps1 [-Board auto|tspi3m|cm5io|rock4d] [-Quick] [-Out DIR]
```

```sh
sudo ./regress.sh [--board auto|tspi3m|cm5io|rock4d] [--quick] [--out DIR]
```

### regress.ps1 的选项

| 选项 | 含义 |
|---|---|
| `-Board` | `auto`（默认）读取 SMBIOS：type 1 的制造商和产品名（`LCKFB TaishanPi 3M`、`ArmSoM CM5-IO`、`Radxa ROCK 4D`，来自各板的 `PcdPlatformVendorName` 和 `PcdPlatformName`），或 BIOS 版本 `rk3576-<PLATFORM_NAME>-...`。三块板子的 ACPI OEM ID（`RKCP` / `RK3576`）都一样，所以只做检查，不用来区分板子 |
| `-Quick` | 存储检查用 16 MiB 而不是 64 MiB；不跑一致性码流 |
| `-Stress`、`-StressSeconds N` | 在用户会话里用 GpuTest 跑 FurMark N 秒（默认 300），1024x640。任一热区或 GPU 达到 90 C 立即停止；开始前若有任何温度达到 80 C 或以上则拒绝运行 |
| `-GpuTest PATH` | GpuTest.exe 的路径；默认是板子上的路径（`C:\图吧工具箱\...\GpuTest_Windows x64\GpuTest.exe`） |
| `-Out DIR` | 结果文件夹（默认 `C:\RK3576-regress\<板子>-<时间>`） |
| `-ToolsDir DIR` | `rknputest.exe` 和 `vdpu383test.exe` 所在目录；还会查找脚本旁的 `bin\`、`%ProgramFiles%\RK3576`、`%ProgramFiles%\RK3576\npu`（RK3576 工具包）和 `PATH` |
| `-User NAME` | 会话类检查使用的桌面用户（默认：当前已登录用户） |
| `-ExpectFirmware REGEX` | 额外要求固件版本字符串匹配，例如某个发布标签 |
| `-Require nvme,sd,...` | 本次运行把可选部件当作必需：`nvme`、`sd`、`emmc`、`wifi`、`bluetooth`、`npu`、`vdec`、`analog`、`hdmiaudio`，以及 `jack`（耳机端点必须处于活动状态：耳机孔要插着东西） |
| `-FetchStreams` | 在板子上直接下载缺少的解码码流 |
| `-DryRun -Mock FILE` | 用记录好的数据评估各项检查（见“测试”） |

## 检查项

每项检查都受 [`boards/`](boards) 里板子清单的约束。清单根据固件的各板功能清单编写。清单中的值为 `true`（必须存在：缺失即
FAIL）、`"optional"`（缺失为 SKIP，用于不一定装配或不一定开启的部件）或 `false`（N-A）。板子有变化时请修改清单。

### Windows（regress.ps1）

| id | 检查内容 | 规则来源 |
|---|---|---|
| `fw.banner` | SMBIOS BIOS 版本是本板的 `rk3576-<PLATFORM_NAME>-<git describe>`（带 `-dirty` 时会注明）。它说明镜像是为哪块板子编译的；其中的版本号在存放它的模块没有重新编译时可能是旧的，所以不能证明刷进去的是哪一次编译 | `scripts/build.sh` 的 `FIRMWARE_VER` |
| `fw.acpi` | DSDT 存在，OEM ID / 表 ID 为 `RKCP` / `RK3576` | `AcpiTables.h` |
| `board.smbios` | SMBIOS 制造商和产品名是本板的 | 板子的 `.dsc` |
| `cpu.count`、`cpu.freq` | 8 个逻辑 CPU；最高频率（`~MHz` 和 `\Processor Information(*)\Processor Frequency`）至少 1600 MHz | CPU Maximum Frequency 默认 1608 MHz |
| `devmgr` | 除板子白名单外没有带问题代码的设备（22 即已禁用，不算问题）；ID 截到硬件 ID 为止 | Maskrom Studio 的首次启动自检 |
| `gpu.driver` | `ACPI\RKCP7402` 绑定到 `maliwddm`，状态 OK | |
| `gpu.wddm` | `\RK3576 GPU(*)\WDDM version` = 20；FAIL 时显示回退原因计数器 | maliwddm 计数器（`maliwddm-counters.cmd`） |
| `gpu.clock` | `\RK3576 GPU(*)\Clock (MHz)`，或 Device Parameters 中的 `GpuClkHz`：泰山派 3M 为 702 MHz（775 mV 电压），其他为 594；GPU 降频时会写明原因 | |
| `gpu.temp` | `\RK3576 GPU(*)\Temperature (C)` 在 10 到 90 C 之间 | |
| `display.mode` | 在用户会话中（`EnumDisplaySettings`）：当前模式是 59 或 60 Hz 下可选的最大模式，且运行在 59 或 60 Hz | |
| `display.tdr` | 运行开始后系统日志中没有 Display 事件 4101（驱动重置）；最后评估 | |
| `audio.hdmi`、`audio.analog` | 通过 MMDevice API 读取所有状态的端点（失败时退回 PnP 的在位端点）。HDMI：清单 `hdmiSai`（`ACPI\RKCP3004\6`）上 csaudiork3x 设备的活动播放端点，按 KS 筛选器（`topologyhdmi`）或名称（`HDMI` 或 `Digital Output`）识别；需要接带扬声器的显示器。模拟音频，在有 `analogCodec` 的板子上：编解码器（`ACPI\RKCP8388`）和 `analogSai` 上的 csaudiork3x 设备已启动；耳机孔空着时耳机端点不出现，所以只作提示，除非 `-Require jack`。没有 `analogCodec` 时：要有活动的耳机端点 | 清单的 `audio.*Endpoint`、`hdmiSai`、`analogSai`、`analogCodec` |
| `net.ethernet`、`net.gateway` | `ACPI\RKCP6543` 上的网卡为 Up 且有 IPv4 地址；默认网关能 ping 通 | |
| `net.wifi`、`net.bluetooth` | 一个 802.11 网卡（仅泰山派 3M，可选：驱动还在分支上）；蓝牙芯片本身的设备正常且有一个蓝牙设备正常。芯片设备默认是 `ACPI\RKCP4345`，清单里 `network.bluetoothDevice` 可以指定别的（ROCK 4D：`USB\VID_A69C&PID_8D81&MI_00`，AIC8800D80 的 HCI 接口，由系统自带的 BthUsb 驱动） | |
| `usb.controllers`、`usb.devices` | 至少两个 `ACPI\PNP0D10` xHCI 控制器正常；列出 USB 设备，CM5-IO 上必须有板载 Hub | |
| `storage.emmc`、`storage.sd`、`storage.nvme` | `ACPI\RKCP0D40`（eMMC）、`ACPI\RKCPFE2C`（SD）下的磁盘，PCIe 上的磁盘（NVMe）；状态 Healthy | |
| `storage.rw` | 以直写方式向系统盘写入 64 MiB（`-Quick` 时 16 MiB），读回，比较 SHA-256，删除 | Maskrom Studio 的自检 |
| `npu.info`、`npu.m3` | 当 `ACPI\RKCP3576` 存在时（设置里开启了 NPU）：绑定到 `rknpu`，`rknputest info` 退出码 0 且报告清单 `npuCores` 个核心（2），`rknputest m3 --core both --iters 1` 退出码 0（逐位一致） | woa-rk3576-npu 的 `tools/rknputest` |
| `vdec.warmup`、`vdec.<编码>.<码流>` | 当 `ACPI\RKCP0383` 存在时：绑定到 `vdpu383`，`vdpu383test warmup` 退出码 0，然后用 `vdpu383test decode` 解 [`streams.txt`](streams.txt) 里的每个码流，比较帧数、硬件错误为 0 以及输出的 MD5 | `tools/vdpu383test/hevc-conformance.ps1` |
| `thermal.zones` | 至少三个 ACPI 热区，每个在 0 到 90 C 之间 | rk-monitor 用的计数器 |
| `time.clock`、`time.rtc` | 年份在 2026..2040 之间，且与 `time.windows.com` 相差不超过 120 秒（`w32tm /stripchart`，只读）；`HYMB0001` 正常 | |
| `stress.furmark`、`stress.gpu-after` | 仅 `-Stress`：FurMark 跑完设定时间且未达到 90 C；之后 GPU 仍然正常；记录 GpuTest 的分数 | |

需要桌面的程序（显示模式探测、GpuTest）会在已登录用户的会话里启动，因为 SSH 会话是会话 0，没有桌面：用
`schtasks /create /ru <用户> /it` 配合 `conhost.exe --headless`，运行一次后删除。没有用户登录（未开启自动登录）时这些检查为 SKIP。

### 各板的检查项

| | 泰山派 3M | CM5-IO | ROCK 4D |
|---|---|---|---|
| 设备管理器白名单 | EC20 的接口 4（`USB\VID_2C7C&PID_0125&MI_04`） | 无 | AIC8800D80 USB Wi-Fi（`USB\VID_A69C`） |
| GPU 频率 | 702 MHz | 594 MHz | 594 MHz |
| 模拟音频 | 必需 | 可选（ES8388 在分支 `cm5io-parity` 上） | 必需 |
| Wi-Fi / 蓝牙 | 可选 / 必需 | N-A | N-A / 必需（AIC8800D80 HCI，要先由 aic8800usb 加载固件） |
| USB Hub | | 必需 | |
| eMMC / SD / NVMe | 必需 / 可选 / 可选 | 必需 / 可选 / 必需 | N-A / 可选 / 可选 |
| NPU | 设置中开启时 | 可选（设置选项只在 `cm5io-parity` 上） | 设置中开启时 |
| 视频解码器 | 设置中开启时 | 设置中开启时 | 设置中开启时 |
| RTC | 没有装电池：时间来自网络 | 有电池 | 有电池 |

### Linux（regress.sh）

`board.dt`（设备树 compatible）、`fw.banner`（通过 UEFI 启动时读 SMBIOS，通过 U-Boot 启动时为 SKIP）、`cpu.count`、
`cpu.freq`（cpufreq）、`dmesg`（清单 `linux.dmesgIgnore` 之外没有 `err`、`crit`、`alert` 或 `emerg` 级别的行；这些行写入
`dmesg-errors.txt`）、`net.ethernet` 和 `net.gateway`（绑定到 GMAC 驱动的网口已 up、有 IPv4、网关有响应）、
`usb.controllers`、`usb.devices`（CM5-IO 的 Hub）、`storage.emmc`、`storage.sd`、`storage.nvme`、`storage.rw`（在
`/var/tmp` 中，用 `iflag=direct` 读回）、`display.hdmi`（DRM 连接器为 `connected`）、`gpu.driver`（panfrost）、
`npu.driver` 和 `vdec.driver`（内核有相应驱动时才报告）、`thermal.zones`（`rk3576.dtsi` 的六个热区）、`fan.pwm`
（`pwmfan` hwmon；主线设备树里没有，所以是可选）、`time.rtc`（HYM8563）、`time.clock`，以及 `power.off`，始终为
N-A：套件从不关闭板子。

## 解码码流

[`streams.txt`](streams.txt) 列出九个小码流（四个 H.264、五个 HEVC Main，总共约 380 KB），取自 FFmpeg fate-suite 中
ITU-T 一致性码流的副本，每个都带 SHA-256、帧数和解码后 I420 输出的 MD5。码流不存进 git：`fetch-streams.sh` 把它们下载到 `streams/` 并校验哈希，`run-remote.sh` 把它们拷到板子上。

## 测试

不需要板子：

* `tests/run-dryrun.ps1` 解析这里的每个 `.ps1`，检查含非 ASCII 文本的文件是否带 UTF-8 BOM（否则 Windows PowerShell 5.1
  会按 ANSI 读取），并对每个 `tests/mock-*.json` 运行 `regress.ps1 -DryRun`。mock 文件里是板子会报告的数据和预期结果
  （正常的板子、埋好的回归问题、缺少计数器、在 90 C 被停止的压力测试）。它还检查套件不会在 `-Out` 之外写任何东西。
* `tests/test-regress-sh.sh` 为虚构的板子构造 `/sys` 和 `/proc` 目录树（`REGRESS_ROOT`），把桩程序 `dmesg`、`ip`、`ping`
  和 `timedatectl` 放在 `PATH` 最前面，检查五种情况；然后用桩程序 `ssh` 和 `scp` 运行 `run-remote.sh`。

CI 在每次推送时运行这两项（[`ci.yml`](../../.github/workflows/ci.yml) 中的 `regress-linux`、`regress-windows`），
分别在 PowerShell 7 和 Windows PowerShell 5.1 下运行，另外在 x64 runner 上真实运行一次 `regress.ps1`（那里大部分行是 FAIL；
它必须跑完、通过存储检查并且不留下任何东西）。

## 添加检查

1. 在 `Fact 'name' { ... }` 块里收集数据，这样 mock 可以提供这些数据。
2. 用 `Add-Result id name status measured expected reason` 判定结果，并用 `Cap` 按清单值决定是否适用。
3. 把这项数据加到 mock 里，把预期结果加到它们的 `expect` 里。
