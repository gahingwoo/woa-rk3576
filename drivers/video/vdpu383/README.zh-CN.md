# vdpu383：RK3576 视频解码器，初始调试

[English](README.md) | 简体中文

RK3576 的 H.264/HEVC/VP9/AV1 解码器（RKVDEC，Rockchip 的叫法是 VDPU383），由 Windows 驱动。这是原型：一个 KMDF 驱动把这个硬件模块
开放给一个测试程序；测试程序解析 H.264，生成解码器的寄存器文件，并把输出和一致性测试码流的参考图像对比。应用程序看不到它。

Edge 将来要用的解码器计划放在显示驱动里（[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)，docs/D3D11-UMD.md，
里程碑 U5），作为一个 VIDEO_DECODE 节点，因为 D3D11 视频必须和 ANGLE 跑在同一个设备上。[lib](lib) 里的寄存器生成代码写成了可以原样搬过去的形式：
纯 C，不调用 KMDF、WDM 或 C 库，不分配内存，不用浮点，输入是 DXVA 结构体。

**这里的东西都还没在硬件上跑过。** 它在 CI 里能编译；解析器和寄存器生成代码在没有解码器的 Linux 上跑过（`--null`）。

## 文件位置

| 路径 | 内容 |
|---|---|
| [lib/vdpu383_h264.c](lib/vdpu383_h264.c) | 把 DXVA_PicParams_H264 + DXVA_Qmatrix_H264 + 码流转换成寄存器 0..383 和辅助缓冲区。来自 Rockchip MPP（Apache-2.0 OR MIT，按 MIT 使用） |
| [lib/v383_dxva.h](lib/v383_dxva.h) | DXVA H.264 结构体，逐字节与 dxva.h 的布局一致（编译时对照 dxva.h 检查） |
| [driver.c](driver.c), [warmup.c](warmup.c) | `ACPI\RKCP0383` 的 KMDF 驱动 |
| [vdpu383_ioctl.h](vdpu383_ioctl.h) | 驱动的接口（`\\.\VDPU383`） |
| [../../../tools/vdpu383test](../../../tools/vdpu383test) | 测试程序和它的 H.264 解析器 |
| [../../../tools/vdpu383-capture](../../../tools/vdpu383-capture) | 主线 Linux 对同样的片段做了什么，以及寄存器对比 |

固件那边是 edk2-rk3576 的 `vdpu383-acpi-v2` 分支（旧的 `vdpu383-acpi` 变基到 main 上，保留了 NPU 选项）：一个默认关闭的设置项，
打开后给 PD_VDEC 上电、设置解码器时钟，并安装一张带这个设备的 SSDT。

## 已核实、推导出的、猜的

| 项目 | 来源 | 状态 |
|---|---|---|
| 寄存器在 0x27B00000（link +0，function +0x100，cache +0x600，IOMMU +0x800/+0x900），GSIV 340/341，PD_VDEC 位，CRU 位，SRAM 0x3FF88000 480 KiB | 主线 rk3576.dtsi、pm-domains.c、clk-rk3576.c、rst-rk3576.c | 从源码读出，没有实测 |
| 启动：link 0x54 = 超时值，0x58 = 0，0x40 = 1；中断：读 0x48/0x4C，向 0x4C 写 0x03FF0000，向 0x48 写 0x00030000 | 主线 rkvdec-vdpu383-h264.c、rkvdec.c | 从源码读出 |
| +0x600 处的参考帧读缓存：进入 D0 时和每次 IP 复位后设 CACHE0_CFG = 0x13、MAX_READS = 0x1C；每次解码前 CLR_CACHE0 = 1 | 主线 "configure the reference read cache"（v3，已合并） | 已移植；主线实测一个 2688x1520 码流，用上电默认值时每帧 22.5 或 65.4 ms，用这些值时 3.6 ms。复位后重新设置是我们加的：主线没有确定复位会不会清掉它们 |
| 超时的解码一定会做 IP 复位（link 0x48 关中断，0x44 = 1，等待状态位 11，清状态），然后排空排队中的 DPC | 主线 "Stop the VDPU383 core on timeout"（已发出，那边只做过编译测试）和 "Handle interrupts without a current context" | 已移植；出错状态仍然只在 `VDPU383_RUN_RESET_ON_ERROR` 下才复位 |
| 预热解码 | 厂商内核 mpp_hack_rk3576.c（GPL-2.0+ OR MIT） | 原样移植；主线不做这一步，所以可能不需要。只有 Device Parameters 里 `WarmupAtD0` = 1（默认 0）时才在进入 D0 时运行；`vdpu383test warmup` 可按需运行 |
| 寄存器值、SPS/PPS/RPS 位布局、RCB 大小、CABAC 表 | MPP hal_h264d_vdpu383.c 等文件 | 已移植；RCB 计算从浮点改成整数（值相同，在 `selftest` 里检查） |
| SPS/PPS 记录同时写在偏移 0（MPP）和 pps_id * 176（主线） | 两者说法不一致 | 不管硬件读哪个都能覆盖到 |
| 由 DXVA_PicParams_H264 得到的默认参考列表 P/B0/B1 | H.264 8.2.4.2，按主线交给硬件的方式 | 新写的 |
| profile_idc、seq_parameter_set_id、constraint_set3_flag | DXVA 里没有 | 工具传入真实值；`--dxva-only` 传入猜测值，用来弄清硬件是否在意 |
| zig-zag 顺序的 DXVA_Qmatrix_H264 | DXVA 规范，按 FFmpeg 的 dxva2_h264.c 的理解 | 没有对照 Windows 解码器核对 |
| IOMMU 保持分页关闭，所以解码器地址是物理地址 | rockchip-iommu.c 的语义 | 猜测是安全的；主线是开着分页跑的 |
| 对非一致性设备，缓冲区用 write-combined（Normal non-cacheable） | | 猜的 |
| 场图像（PAFF）、FMO、4:2:2、10 bit、MVC | | 不支持；MBAFF 帧会直接通过，没有测试 |

## 痕迹记录和设置

驱动把它的最后一步和读到的内容保存在设备的硬件键下，并立即刷到磁盘，所以卡死或蓝屏之后，下次启动仍然能读到
（如果是解码器导致 Windows 起不来，启动时把设置项关掉）：

```
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"
```

| 值 | 含义 |
|---|---|
| `LastStep`, `PrevStep`, `StepSeq` | 驱动所在的步骤、上一步，以及一个计数器。蓝屏后 LastStep 是 `D0Entry: first register read`，说明解码器的电源域或时钟没开：固件没给它上电 |
| `RegsPa`, `SramPa` | Windows 分给它的资源（应为 0x27B00000、0x3FF88000） |
| `Version`, `Mmu0Status`, `Mmu1Status`, `LinkIrqAtD0`, `LinkStatusAtD0` | 进入 D0 时的第一批读取结果 |
| `CacheCfgPowerOn`, `CacheMaxReadsPowerOn` | 驱动设置之前的读缓存值 |
| `WarmupIrq`, `WarmupStatus`, `WarmupTimedOut` | 最近一次预热，来自进入 D0 时或工具 |
| `FirstRunResult`, `FirstRunStatus` | 本次会话的第一次解码（0 = 正常） |

一个设置项，同一个键下的 DWORD，每次设备启动时读取：

| 设置 | 默认值 | 含义 |
|---|---|---|
| `WarmupAtD0` | 0 | 1：每次进入 D0 时运行厂商内核的预热解码，和厂商内核在 probe 和 resume 时的做法一样 |

## 测试计划

按顺序运行。每一步都写明了失败时要发回什么。

### 0. 固件

刷写 `vdpu383-acpi-v2` 固件（该分支上 edk2-rk3576 运行的 CI 构建产物 `cm5io-firmware` 或 `tspi3m-firmware`）。
设置项保持关闭，照常启动一次 Windows 和 Linux：不应有任何变化。

然后在设置界面的 *ACPI / Device Tree* 里：config table mode 必须包含 ACPI；把 *Video Decoder (VDPU383, experimental)*
（视频解码器，实验性）设为 Enabled，保存，重启。串口日志应显示：

```
RK3576Dxe: VDEC: gpll 1188000000 Hz, cpll 1000000000 Hz -> aclk 594000000, aclk_bak 500000000, hevc_ca 1000000000 Hz
RK3576Dxe: VDEC powered, version=0x........ mmu status=0x........./0x........
```

（或者在第二行之前出现 `VDEC power domain was already on`）。在 UEFI shell 里，`acpiview -s SSDT` 会列出一张 OEM table ID 为 `RKVDEC` 的表。

失败时发回：从上电到启动管理器的串口日志。

### 1. 安装

从 `vdpu383-v2` 分支的 WinPE workflow 运行里取构建产物 `woa-drivers-signed-arm64`：`woa-drivers\vdpu383\`（签名好的驱动包）、
`woa-drivers\signer.cer` 和 `woa-tools\`（vdpu383test.exe 和 Linux 抓取脚本）。那次运行生成的 WinPE 映像里已经带了这个驱动。

在已安装的 Windows 上，以管理员身份，先运行 `w32tm /resync /force`，然后：

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
pnputil /add-driver vdpu383\vdpu383.inf /install
```

设备管理器的 *System devices*（系统设备）下应出现 "Rockchip RK3576 Video Decoder (VDPU383, bring-up)"，没有问题代码。
固件设置项关闭时，这个设备不存在，驱动不会绑定任何东西。

失败时发回：`pnputil /enum-devices /instanceid "ACPI\RKCP0383\0" /drivers`（或显示的问题代码），以及
`C:\Windows\INF\setupapi.dev.log` 里 `RKCP0383` 附近的几行。

### 2. 上电和预热

```
vdpu383test info
```

预期结果（根据厂商代码，没有实测）：

- `version register` 不为零，和固件 `version=` 那一行一致
- 两个 IOMMU 实例都是 `paging off`
- `warm-up at D0 entry: not run`（默认值，`WarmupAtD0` 为 0）

然后运行 `vdpu383test warmup`（通过 link 寄存器，中断开启）：`irq 0x0000000?`，状态位 0（`frame_rdy`）置位，位 1-9 清零，
`verdict: PASS`。位 10-15 可能置位，它们不是错误。再运行 `vdpu383test warmup-direct`（同样的 32x32 图像，走普通的启动路径）。
`warmup-direct` 运行三次：每次输出的 MD5 必须相同。

发回：这三条命令的全部输出，不管打印了什么。

### 3. 重放

```
vdpu383test decode BA1_Sony_D.jsv -n 3 --dump C:\v383 --readback
vdpu383test replay C:\v383\pic_0000\replay.txt
vdpu383test replay C:\v383\pic_0002\replay.txt
```

每次重放都从转储里重新写入一帧图像的寄存器文件和缓冲区，并把输出和解码时记录的 MD5 对比：`replay: ok ... MATCH`。
状态正常但 MD5 不匹配，说明解码不是确定性的（内存顺序、缓存属性）；状态不同，说明寄存器文件以外的状态也有影响。

失败时发回：控制台输出和打包好的 `C:\v383`。

### 4. H.264 一致性测试

码流来自 ITU-T H.264.1 一致性测试套件，https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/ （AVCv1/ 和 FRExt/），
每个都是一个 zip，里面有码流和参考输出。对每一个：

```
vdpu383test decode STREAM --ref REFERENCE.yuv -o out.yuv
```

通过的标准：`reference: N frames; MATCH`，并且 `output md5` 和下表中参考文件的 MD5 相同。然后加上 `--dxva-only` 再跑一次：
MD5 相同，说明硬件除了 DXVA 和 slice header 里的 pps id 之外不需要别的东西。

| 码流（zip） | 文件 | 覆盖内容 | 帧数 | 参考 MD5 |
|---|---|---|---|---|
| BA1_Sony_D | BA1_Sony_D.jsv | Baseline，CAVLC，I 和 P，QCIF | 17 | 114d1cf94a2fcaffda0cf1b49964bf3d |
| SVA_BA1_B | SVA_BA1_B.264 | Baseline | 17 | dab92aa2145ab44abab2beb2868dd326 |
| BA_MW_D | BA_MW_D.264 | Baseline，多个 slice | 100 | 7d5d351ad061640294bf43a43150fbca |
| BANM_MW_D | BANM_MW_D.264 | Baseline，多种 nal_ref_idc | 100 | e637d38ed004df3540218e3d84b43e42 |
| MIDR_MW_D | MIDR_MW_D.264 | 多个 IDR | 100 | d87bff88b2c5b96ccb291ef68a45bbc2 |
| NRF_MW_E | NRF_MW_E.264 | 非参考帧 | 100 | a8635615b50c5a16decc555a3c6c81c8 |
| MR1_BT_A | MR1_BT_A.h264 | 多参考帧 | 62 | 6ea31a214aadd8bdc8e7d37195d91c81 |
| MR2_TANDBERG_E | MR2_TANDBERG_E.264 | 多参考帧，重排序 | 300 | d154bf9264960fecc6d2cf72be4cf8cc |
| CABA1_SVA_B | CABA1_SVA_B.264 | Main，CABAC，I | 17 | 2f5cabd5bb4954c0386cafd8a9aa782a |
| CABA2_SVA_B | CABA2_SVA_B.264 | CABAC，P | 17 | 6d4277a7fc70ed1fbe3c5f10b0a70671 |
| CABA3_SVA_B | CABA3_SVA_B.264 | CABAC，B | 33 | 63a0f8fdcbb87b0dff330acdd10905c0 |
| CABA3_TOSHIBA_E | CABA3_TOSHIBA_E.264 | CABAC，B，300 帧 | 300 | 13651d01c5b533e6b7aa132bae7669ed |
| CANL1_TOSHIBA_G | CANL1_TOSHIBA_G.264 | CABAC，关闭去块滤波 | 300 | afa072746b16bd96f3152b45e2f2881e |
| CABAST3_Sony_E | CABAST3_Sony_E.jsv | CABAC，多种 slice 类型，CIF | 25 | e95f27c3779af519c1a317adb2f699a5 |
| CAPM3_Sony_D | CAPM3_Sony_D.jsv | CABAC，混合 slice 类型的图像 | 300 | 9ecc3bf5ff7cac9a068a5ba57bc87cb7 |
| CAWP1_TOSHIBA_E | CAWP1_TOSHIBA_E.264 | 加权预测，CIF | 90 | 87946607d1d774c2da8ec8632710c84a |
| CVWP2_TOSHIBA_E | CVWP2_TOSHIBA_E.264 | 加权预测，CAVLC | 90 | 7616ccc1e4cc89805cf5ae11e4437533 |
| FRExt1_Panasonic_D | FRExt1_Panasonic.avc | High，8x8 变换 | 8 | f7023a3a93cc98c0c8d83c1deaf11048 |
| FRExt3_Panasonic_E | FRExt3_Panasonic.avc | High | 11 | 231e4f3105f75a3602161ec484bbd8e3 |
| HPCV_BRCM_A | HPCV_BRCM_A.264 | High，CAVLC，CIF | 300 | 5eafc104844211893e1d8af0b8b66ed6 |
| HPCAQ2LQ_BRCM_B | HPCAQ2LQ_BRCM_B.264 | High，CABAC，缩放矩阵 | 100 | cf81785c89930d5b1c191a1ef4cb9e83 |

这些 MD5 是每个 zip 里参考 .yuv 文件的 MD5；FFmpeg 软件解码每个码流得到的 MD5 也相同（2026-11-21 核对）。
先跑 BA1_Sony_D，然后是 CABA1_SVA_B、CABA3_SVA_B、MR2_TANDBERG_E、FRExt1_Panasonic_D 和 HPCAQ2LQ_BRCM_B：
每个都多一项功能，所以第一个失败的码流就指出了是哪项功能出问题。

失败时，针对第一个失败的码流发回：

1. 下面这条命令的完整控制台输出：
   `vdpu383test decode STREAM --ref REF.yuv --frame-md5 --readback --regs -n 8 > win.txt`
2. `vdpu383test decode STREAM -n 4 --dump C:\v383`，以及打包好的 `C:\v383`
3. 同一码流在 Linux 下的抓取结果（见下文），以及第一帧出现差异的图像的 `compare-regs.py` 输出
4. 失败之后的 `vdpu383test info`

### Linux 参考抓取

在板子上运行 Armbian edge 7.3.x，在 `woa-tools\`（或本仓库的 tools/vdpu383-capture）里：

```
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-bad gstreamer1.0-plugins-base v4l-utils python3
sudo ./capture-linux.sh                       # BA1_Sony_D and CABA3_SVA_B, fetched from the ITU site
sudo ./capture-linux.sh STREAM.264 -n 8       # or any stream
```

它会生成 `vdpu383-capture-<host>-<time>.tar.gz`；把这个文件发回来。里面对每个码流包含：前 N 帧图像各自解码后的寄存器文件、
硬件解码输出及其 MD5（应与上面的参考 MD5 相同）、一份 ftrace 调用图，以及解码器时钟、电源域、CRU 和 PMU 的状态。
它只在解码器保持唤醒时读寄存器；如果 `/dev/mem` 拒绝访问解码器的地址范围，它会说明，在内核命令行加上 `iomem=relaxed` 启动即可解决。

对比某一帧图像（Linux 的 `regs-pic-0003.txt` 是第三帧；Windows 工具从 0 开始计数）：

```
vdpu383test decode CABA3_SVA_B.264 -n 8 --regs > win.txt          (Windows)
python3 compare-regs.py capture/CABA3_SVA_B/regs-pic-0003.txt win.txt --picture 2
```

`vdpu383test --null decode ...` 在任何 PC 上都能得到同样的寄存器文件，只是缓冲区地址是编造的。
