# vdpu383：RK3576 视频解码器，初始调试

[English](README.md) | 简体中文

RK3576 H.264/HEVC/VP9/AV1 解码器（RKVDEC，Rockchip 的叫法是 VDPU383）的 KMDF 驱动，外加一个测试程序：
它解析码流、生成解码器的寄存器文件，并把输出和参考 MD5 对比。应用程序不会直接用到这个驱动。Edge 将来要用的解码器
计划作为显示驱动（[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)）里的 VIDEO_DECODE 节点；
[lib](lib) 里的寄存器生成代码是纯 C（不调用 KMDF、WDM 或 C 库，不分配内存，不用浮点），可以原样搬过去。

只有在固件设置里打开 *Video Decoder (VDPU383)*（0.4.1 之后的 edk2-rk3576 默认打开，0.4.1 默认关闭，见[硬件功能](../../../docs/features.zh-CN.md#视频解码器)）后，
`ACPI\RKCP0383` 这个设备才存在。

| 编码格式 | 状态 |
|---|---|
| H.264 | 已上板（泰山派 3M）：六个一致性码流逐位一致，硬件错误为 0 |
| HEVC Main | 已上板：用连续缓冲区（`Iommu` 0）60/75 MATCH，14 个大码流分配失败；还没在 IOMMU 路径上重跑 |
| VP9 profile 0/2 | 已上板：215/216；失败的那个（vp90-2-trac3849）已在生成代码里修正，尚未重跑 |
| AV1 Main | 仅 CI（解析器和生成代码在 Linux 上用 `--null` 跑） |
| IOMMU 缓冲区（`Iommu` 1，默认） | 已上板，跑过 H.264 和 VP9 |

历史记录：`git log -- drivers/video/vdpu383 tools/vdpu383test`。

## 文件位置

| 路径 | 内容 |
|---|---|
| [lib/vdpu383_h264.c](lib/vdpu383_h264.c), [lib/v383_dxva.h](lib/v383_dxva.h) | H.264：把 DXVA_PicParams_H264 + DXVA_Qmatrix_H264 + 码流转换成寄存器 0..383 和辅助缓冲区（来自 Rockchip MPP，按 MIT 使用）；DXVA 结构体在编译时对照 dxva.h 检查 |
| [lib/vdpu383_hevc.c](lib/vdpu383_hevc.c), [lib/v383_dxva_hevc.h](lib/v383_dxva_hevc.h) | HEVC Main，来自 MPP，对照主线 rkvdec-vdpu383-hevc.c 核对；重建 DXVA 不携带的 SPS 参考图像集 |
| [lib/vdpu383_vp9.c](lib/vdpu383_vp9.c), [lib/vdpu383_vp9_probs.c](lib/vdpu383_vp9_probs.c), [lib/v383_dxva_vp9.h](lib/v383_dxva_vp9.h) | VP9 profile 0 和 2：寄存器来自 MPP hal_vp9d_vdpu383.c（MIT），打包代码按 MPP 的布局重新编写（输出逐字节相同），默认概率表来自主线 v4l2-vp9.c（GPL-2.0）。在帧之间保留帧上下文、分段图和运动矢量（V383_Vp9State） |
| [lib/vdpu383_av1.c](lib/vdpu383_av1.c), [lib/vdpu383_av1_cdf.c](lib/vdpu383_av1_cdf.c), [lib/v383_dxva_av1.h](lib/v383_dxva_av1.h) | AV1 Main 8/10 bit：DXVA_PicParams_AV1 转换成寄存器和 672 字节的全局记录，来自 MPP（MIT）；按 surface 保存参考槽状态（V383_Av1State），可以把 DXVA 的 tile 数据包进 OBU |
| [driver.c](driver.c), [warmup.c](warmup.c), [vdpu383_ioctl.h](vdpu383_ioctl.h) | KMDF 驱动和它的接口 `\\.\VDPU383` |
| [../../../tools/vdpu383test](../../../tools/vdpu383test) | 测试程序、解析器、一致性测试列表和脚本 |
| [../../../tools/vdpu383-capture](../../../tools/vdpu383-capture) | Linux 参考抓取和寄存器对比 |

## 硬件事实和来源

| 项目 | 来源 | 状态 |
|---|---|---|
| 寄存器在 0x27B00000（link +0，function +0x100，cache +0x600，IOMMU +0x800/+0x900），GSIV 340/341，SRAM 0x3FF88000 480 KiB | 主线 rk3576.dtsi、pm-domains.c、clk-rk3576.c、rst-rk3576.c | 从源码读出 |
| 启动：link 0x54 = 超时值，0x58 = 0，0x40 = 1；中断：读 0x48/0x4C，向 0x4C 写 0x03FF0000，向 0x48 写 0x00030000 | 主线 rkvdec | 从源码读出 |
| 读缓存：进入 D0 时和每次 IP 复位后设 CACHE0_CFG = 0x13、MAX_READS = 0x1C；每次解码前 CLR_CACHE0 = 1 | 主线 | 已移植（主线实测每帧 3.6 ms，用上电默认值时 22.5 到 65.4 ms） |
| 解码超时：IP 复位（link 0x48 关中断，0x44 = 1，等待状态位 11，清状态），然后排空 DPC | 主线 | 已移植；出错状态只在 `VDPU383_RUN_RESET_ON_ERROR` 下才复位 |
| 进入 D0 时和 IP 复位后的预热解码 | 厂商内核 mpp_hack_rk3576.c | 必需：没有它，上电后 H.264 解码是错的 |
| SPS/PPS 记录同时写在偏移 0（MPP）和 pps_id * 176（主线） | 两者说法不一致 | 两处都写 |
| profile_idc、seq_parameter_set_id、constraint_set3_flag | DXVA 里没有 | `--dxva-only` 传入猜测值，测试硬件是否在意 |
| IOMMU v2 页表（4 KiB 页，IO 地址从 16 MiB 开始，SRAM 和预热缓冲区按物理地址映射），每次复位后恢复 | 主线 rockchip-iommu.c、rkvdec.c | 已移植 |
| 缓冲区用 write-combined | | 猜的 |
| PAFF、FMO、4:2:2、H.264 10 bit、MVC | | 不支持；MBAFF 未测试 |

## 设置和痕迹记录

`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters` 下的 DWORD，设备启动时读取
（修改后重启设备或重启系统）：

| 设置 | 默认值 | 含义 |
|---|---|---|
| `WarmupAtD0` | 1 | 每次进入 D0 和 IP 复位时运行预热解码。0：从不运行（H.264 会出错，直到运行 `vdpu383test warmup`） |
| `Iommu` | 1 | 缓冲区经解码器 IOMMU 映射。0：4 GiB 以下的物理连续缓冲区，关闭分页（大 surface 会以错误 1450 失败） |
| `UserAccess` | 0 | 谁能从用户态打开 `\\.\VDPU383`。0：SYSTEM 和管理员，一个句柄。1：所有用户，多个句柄。2：低完整性进程也可以。为 1 或 2 时忽略 `VDPU383_RUN_NO_ADDR_CHECK`，每个句柄最多持有 384 MiB。播放器都不需要它：它们经显示驱动解码（见下面的会话） |

`UserAccess` 1 和 2 只用于测试板，作为显示驱动路径的退路。这个接口允许调用者写解码器寄存器；驱动检查每个地址寄存器都指向该句柄自己的缓冲区，
但不检查解码器是否停留在缓冲区内。用户态句柄共用一个 IOMMU 页目录，所以一个进程能碰到另一个进程的缓冲区；
`Iommu` 为 0 时它能访问任意内存。

会话（接口版本 4，[vdpu383_check.h](vdpu383_check.h)）：从内核态打开的文件对象。wddm-mali-g52 的 maliwddm
为每个要解码的 D3D 设备开一个，在里面替那个进程发调用（wddm-mali-g52 docs/CHROMIUM-VIDEO.md，C2）。会话只能
取信息、分配、释放、导入和解码；每次解码的每个地址寄存器都按它自己的缓冲区和导入检查（不含 SRAM，不看标志）；
不读回寄存器，最多等 2 秒，只能导入发给自己进程的令牌；解码时用它自己的 IOMMU 页目录，里面只有它的缓冲区，
所以解码器碰不到别的会话或用户句柄的内存；需要 `Iommu` 1；拿不到物理地址和寄存器内容。最多 64 个会话。
`vdpu383test info` 打印 `sessions` 一行，任何用户都可以用 wddm-mali-g52 的 `vdectest kmd` 看同样的计数。

驱动还会把痕迹记录写到同一个键下并立即刷盘，卡死或蓝屏后仍能读到（如果是解码器导致 Windows 起不来，启动时关掉固件选项）：

| 值 | 含义 |
|---|---|
| `LastStep`, `PrevStep`, `StepSeq` | 当前步骤、上一步、计数器。蓝屏后为 `D0Entry: first register read` = 固件没给解码器上电 |
| `RegsPa`, `SramPa` | 分配到的资源（应为 0x27B00000、0x3FF88000） |
| `Version`, `Mmu0Status`, `Mmu1Status`, `LinkIrqAtD0`, `LinkStatusAtD0` | 进入 D0 时的第一批读取结果 |
| `CacheCfgPowerOn`, `CacheMaxReadsPowerOn` | 驱动设置之前的读缓存值 |
| `WarmupIrq`, `WarmupStatus`, `WarmupTimedOut` | 最近一次预热 |
| `FirstRunResult`, `FirstRunStatus` | 本次会话的第一次解码（0 = 正常） |
| `IommuSetting`, `IommuOn`, `MmuDte0`, `MmuEnableStatus0/1` | 读到的 `Iommu` 值、分页是否打开、目录寄存器、打开后的状态（位 0 = 分页） |
| `LastMmuFaultAddr` | 最近一次 IOMMU 故障的 IO 地址 |
| `LastImportStage`, `LastImportStatus`, `LastImportPages`, `LastImportExporterStatus`, `LastImportExporterPages`, `LastImportHandle`, `LastImportPid`, `ImportFailures`, `Imports` | 最近一次失败的 `IOCTL_VDPU383_IMPORT`（前 16 次，之后每 64 次一次）。Stage：1 参数或 IOMMU 关，2 内存不足，3 刷新未知导入，4 无空闲 IO 地址，5 无导出方（maliwddm 未启动），6 导出方拒绝（见其状态与 maliwddm 的 `VdecLast*`），7 页数不符，8 IOMMU 表项 |

## 安装

固件：打开 *ACPI / Device Tree* > *Video Decoder (VDPU383, experimental)*（config table mode 必须包含 ACPI）。
之后串口日志会显示 `RK3576Dxe: VDEC powered, version=0x...`，UEFI shell 里 `acpiview -s SSDT` 会列出 OEM table ID 为 `RKVDEC` 的表。

驱动：取自 WinPE workflow 的构建产物 `woa-drivers-signed-arm64`（`woa-drivers\vdpu383\`、`woa-drivers\signer.cer`、
`woa-tools\vdpu383test.exe`）。测试证书每次运行都会变，所以要导入同一次运行的证书。以管理员身份，先运行 `w32tm /resync /force`：

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
pnputil /add-driver vdpu383\vdpu383.inf
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0" /v ConfigFlags /t REG_DWORD /d 0x20 /f
shutdown /r /t 0
```

分阶段安装（标记为需要重装，然后重启）比在运行中的设备上用 `/install` 更稳妥。之后设备管理器的系统设备下会出现
"Rockchip RK3576 Video Decoder (VDPU383, bring-up)"，没有问题代码。

失败时发回：`pnputil /enum-devices /instanceid "ACPI\RKCP0383\0" /drivers` 和 `C:\Windows\INF\setupapi.dev.log` 里 `RKCP0383` 相关的行。

## 测试

按顺序运行；第一个失败的步骤就指出了出问题的功能。

| 步骤 | 命令 | 通过标准 |
|---|---|---|
| 上电 | `vdpu383test info` | `version register` 不为零；两个 IOMMU 实例都是 `paging ON` 且 `dte` 相同；`warm-up verdict: PASS`；`buffers IO addresses, IOMMU paging on` |
| 预热 | `vdpu383test warmup`，然后 `vdpu383test warmup-direct` 运行三次 | 状态位 0 置位，位 1-9 清零（10-15 不是错误），`verdict: PASS`；三次 MD5 相同 |
| 重放 | `vdpu383test decode BA1_Sony_D.jsv -n 3 --dump C:\v383 --readback`，然后 `vdpu383test replay C:\v383\pic_0000\replay.txt` | `replay: ok ... MATCH`（状态正常但不匹配 = 解码不确定；状态不同 = 寄存器文件以外的状态有影响） |
| H.264 | `vdpu383test decode STREAM --ref REFERENCE.yuv -o out.yuv`，再加 `--dxva-only` 跑一次 | `reference: N frames; MATCH`，MD5 与下表一致 |
| HEVC | `powershell -ExecutionPolicy Bypass -File hevc-conformance.ps1 -Dir D:\hevc\streams`（先 `-Only RPS_A`，最后 `-DxvaOnly -Out .\hevc-dxva`） | 每行都是 `MATCH` |
| VP9 | `vp9-conformance.ps1 -Dir D:\vp9\streams`（先 `-Only vp90-2-00`，最后 `-DxvaOnly -Out .\vp9-dxva`） | 每行都是 `MATCH` |
| AV1 | `av1-conformance.ps1 -Dir D:\av1\streams`，然后 `-DxvaOnly -Out .\av1-dxva` 和 `-DxvaOnly -Synth 1 -Out .\av1-dxva-frame` | `MATCH`；`GRAIN-OFF` = 输出等于 dav1d 不加胶片颗粒的结果 |
| 回退 | 设 `Iommu` 0，重启 | `vdpu383test info` 显示 `physical, contiguous`，BA1_Sony_D 仍然匹配；之后删掉这个值 |

全部跑完后，`vdpu383test info` 应显示 `0 IOMMU faults`、`0 failed allocations`，`pages mapped` 回到 122。
出故障的解码会打印 `MMU-FAULT` 和 `IOMMU fault at IO address 0x...`；下一次解码正常进行。

### 码流

- H.264：ITU-T H.264.1 测试套件，https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/ （AVCv1/、FRExt/）；每个 zip 里有码流和参考 .yuv。先跑 BA1_Sony_D、CABA1_SVA_B、CABA3_SVA_B、MR2_TANDBERG_E、FRExt1_Panasonic_D、HPCAQ2LQ_BRCM_B。
- HEVC：`https://fate-suite.ffmpeg.org/hevc-conformance/NAME.bit`，即 [hevc-conformance.txt](../../../tools/vdpu383test/hevc-conformance.txt) 里列出的 75 个码流及 FFmpeg 6.1.1 的 MD5。AMP_A/B 需要大约 170 MiB 缓冲区。NUT_A_ericsson_5 和 RPS_D_ericsson_6 可能不一致（见列表）。
- VP9：`https://fate-suite.ffmpeg.org/vp9-test-vectors/`，转封装为 IVF（`ffmpeg -i NAME.webm -c copy -f ivf NAME.ivf`，vp90-2-16-intra-only 要加 `-copyinkf`），列在 [vp9-conformance.txt](../../../tools/vdpu383test/vp9-conformance.txt)。参考输出用 `-autoscale 0 -fps_mode passthrough` 生成；不加的话 FFmpeg 会重复帧，正确的码流也会显示 MISMATCH。`-Extra '--vp9-mpp'` 按 MPP 处理帧上下文的方式重跑（MPP 与规范不一致之处）。
- AV1：fate-suite `av1-test-vectors/` 和 `av1/` 里的 17 个 IVF 文件，加上 7 个用 libaom 和 SVT-AV1 生成的，列在 [av1-conformance.txt](../../../tools/vdpu383test/av1-conformance.txt)，附 dav1d 的 MD5。顺序：allintra、cdfupdate、mv、mfmv，然后其余。

`--frame-md5` 用的逐帧参考 MD5 在测试包的 `framemd5\NAME.framemd5` 里。

| H.264 码流（zip） | 文件 | 覆盖内容 | 帧数 | 参考 MD5 |
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
| CAPM3_Sony_D | CAPM3_Sony_D.jsv | CABAC，混合 slice 类型 | 300 | 9ecc3bf5ff7cac9a068a5ba57bc87cb7 |
| CAWP1_TOSHIBA_E | CAWP1_TOSHIBA_E.264 | 加权预测，CIF | 90 | 87946607d1d774c2da8ec8632710c84a |
| CVWP2_TOSHIBA_E | CVWP2_TOSHIBA_E.264 | 加权预测，CAVLC | 90 | 7616ccc1e4cc89805cf5ae11e4437533 |
| FRExt1_Panasonic_D | FRExt1_Panasonic.avc | High，8x8 变换 | 8 | f7023a3a93cc98c0c8d83c1deaf11048 |
| FRExt3_Panasonic_E | FRExt3_Panasonic.avc | High | 11 | 231e4f3105f75a3602161ec484bbd8e3 |
| HPCV_BRCM_A | HPCV_BRCM_A.264 | High，CAVLC，CIF | 300 | 5eafc104844211893e1d8af0b8b66ed6 |
| HPCAQ2LQ_BRCM_B | HPCAQ2LQ_BRCM_B.264 | High，CABAC，缩放矩阵 | 100 | cf81785c89930d5b1c191a1ef4cb9e83 |

### 失败时发回什么

针对第一个失败的码流：结果文件（`*-results\*-results.txt`）和它的 `NAME.txt`，
`vdpu383test decode NAME --frame-md5 --readback --regs -n 4 > win.txt`，
`vdpu383test decode NAME -n 4 --dump C:\v383` 及打包好的 `C:\v383`，`vdpu383test info`，Device Parameters 键，
H.264 还要附上下面的 Linux 抓取结果和 `compare-regs.py` 输出。

### Linux 参考抓取

在板子上运行 Armbian edge 7.3.x，在 `woa-tools\` 或 [tools/vdpu383-capture](../../../tools/vdpu383-capture) 里：

```
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-bad gstreamer1.0-plugins-base v4l-utils python3
sudo ./capture-linux.sh                       # BA1_Sony_D and CABA3_SVA_B, fetched from the ITU site
sudo ./capture-linux.sh STREAM.264 -n 8       # or any stream
```

它会生成 `vdpu383-capture-<host>-<time>.tar.gz`，对每个码流包含：前 N 帧各自解码后的寄存器文件、解码输出及其 MD5、
ftrace 调用图，以及时钟、电源域、CRU 和 PMU 状态。如果 `/dev/mem` 拒绝访问解码器地址范围，用 `iomem=relaxed` 启动。

对比某一帧（Linux 从 1 开始计数，Windows 工具从 0 开始）：

```
vdpu383test decode CABA3_SVA_B.264 -n 8 --regs > win.txt          (Windows)
python3 compare-regs.py capture/CABA3_SVA_B/regs-pic-0003.txt win.txt --picture 2
```

`vdpu383test --null decode ...` 在任何 PC 上都能得到同样的寄存器文件，只是缓冲区地址是编造的。
