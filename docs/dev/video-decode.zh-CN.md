# Windows 上的硬件视频解码（和编码）：计划

[English](video-decode.md) | 简体中文

写于 2026-11-22。这份计划里的东西都还没有在硬件上跑过。板子顺序：先立创泰山派 3M，再 CM5-IO，然后 ROCK 4D
（同一颗 SoC，同一个固件选项）。

## 决定：通过显示驱动做 D3D11 视频解码

Windows 应用使用硬件解码器有两条路：

- **(a) D3D11VA**：显示驱动的用户态驱动（UMD）通过 `ID3D11VideoDevice` 提供解码器 profile；
  runtime 把 DXVA 图像参数、slice 控制信息和码流交给它；内核驱动启动解码引擎，再发出一个 fence 信号。
- **(b) 一个 Media Foundation 解码器（MFT）**，它和一个单独的 KMDF 驱动通信，把 NV12 返回到系统内存或 D3D 内存。

我们选 **(a)**。原因：

1. Edge 和所有 Chromium 构建只通过它们用来渲染的那个 D3D11 设备上的 `ID3D11VideoDevice` 解码
   （`D3D11VideoDecoder`），从来不加载第三方 MFT。没有 (a)，不管别的做了什么，YouTube 都只能软件解码。
2. Movies & TV、Media Player 以及其他基于 Media Foundation 的应用，用的是微软自己的解码器 MFT
   （H.264 系统自带；HEVC、VP9 和 AV1 Video Extensions）。这些 MFT 是 DXVA 客户端：只要有一个带解码器 profile 的
   D3D11 设备，它们就通过它做硬件解码。所以 (a) 同样能服务 MF 应用，不需要我们自己写 MFT。
3. 解码出来的 NV12 surface 留在 Mali 能采样、VOP2 能扫描输出的内存里。单独驱动上的 MFT 没有共享的 D3D 设备，
   每一帧都要经过 CPU。
4. 解码器的 MMIO、中断和 IOMMU 只有一个所有者，就在已经管着 VOP2 及其 IOMMU 的那个驱动里（页表代码相同）。

(b) 不做。只有当显示驱动永远做不出视频 DDI 时它才值得做，而且那时也只对 MF 应用有用。

这和 wddm-mali-g52 的 `docs/D3D11-UMD.md`（U4 视频 DDI，U5 解码节点，2026-11-21 决定）一致。
本仓库里的 KMDF 驱动 `drivers/video/vdpu383` 是 bring-up 用的试验台，不是最终产品：它用来验证寄存器编程；
它的寄存器构建器（`drivers/video/vdpu383/lib`，纯 C，不分配内存，不用浮点，输入是 DXVA 结构体）会原样搬进 UMD。

编解码器顺序：**先 H.264**。DXVA 的 H.264 结构体最简单；mainline 的 `rkvdec-vdpu383-h264.c` 是 mainline
里最短的路径；ITU 一致性测试集给每条码流都附带了参考 YUV；浏览器要求 avc1 时，YouTube 也会给 avc1。
接下来是 HEVC（mainline 有 `rkvdec-vdpu383-hevc.c`），然后是 VP9 和 AV1（只有 MPP 有；mainline 里 VDPU383 不支持）。

## 里程碑

| | 目标 | 在哪里 | 状态 | 工作量 |
|---|---|---|---|---|
| V1 | 固件给 PD_VDEC 上电并声明设备；Windows 绑定驱动；寄存器可读 | edk2-rk3576 `vdpu383-acpi-v2`，woa-rk3576 `vdpu383-v2` | 代码完成，CI 已构建，未运行 | 只差板上测试 |
| V2 | 在测试工具里从保存的码流解码 H.264 帧，和参考 YUV 的 MD5 比对 | `tools/vdpu383test`，`drivers/video/vdpu383/lib` | 代码完成：解析器、寄存器构建器、测试框架、replay、MD5；只用 `--null` 检查过 | 板上迭代 1 到 3 周 |
| V3 | 面向应用的路径：`ID3D11VideoDevice` 列出 H.264，`DecoderBeginFrame` 能解码 | wddm-mali-g52 U4 + U5，单独一个分支 | 未开始；需要 U4（视频 DDI）和第二个节点 | V2 之后 5 到 8 周 |
| V4 | Edge 上的 YouTube（avc1）和 Movies & TV 用硬件播放 1080p | 同上 | 未开始 | V3 之后 2 到 4 周（NV12 显示、参考 surface 生命周期、电源） |

V2 是在上板之前"能做到的稳妥程度"：凡是能从 mainline、MPP 或 vendor 内核读出来的值都已经写进去了；
它们之间有分歧的值，两种写法都写了（见驱动 README 的 "Verified, derived, guessed"）。下一步的代码改动要根据板子的反馈来做。

### V3 需要显示驱动提供什么（建议的接口，尚未实现）

这项工作不改动显示驱动任何分支上的东西。以下是给做 U4/U5 的人（或者给从 `hpd-edge` 拉出的 `video-decode` 分支）的建议：

1. **所有权。** 固件选项增加一个值：`AcpiVdec` 为 1 时，为 bring-up 驱动发布 `ACPI\RKCP0383`（现在的做法）；
   为 2 时不出现 `RKCP0383`（`_STA` 0），并把 `\_SB.VDRS` 资源模板（MMIO 0x27B00000，GSIV 340/341，SRAM）
   追加到 GPU0 的 `_CRS` 里、放在 maliwddm 已经解析的那些资源之后，这样这个模块始终只有一个驱动去映射。
   为此 Vdec.asl 把这个模板放在一个具名对象里。
2. **节点。** `MW_UMD_ADAPTER_INFO.DecodeNode`（adapter-info 查询里已经预留）设为解码节点的序号；
   `UmdVideo` 为 2 时打开这些 profile。
3. **提交。** 每张图像一条私有命令：`vdpu383_h264_build()` 生成的寄存器映像，加上分配列表（码流、输出和参考 NV12、
   RCB 和辅助缓冲区）。KMD 填入 IOVA，按列表检查每个地址寄存器（和现在的 `V383Run` 一样），写寄存器，
   用 `V383Run` 的方式启动核心，在中断里完成 fence，超时就执行 `V383IpReset`。
4. **IOMMU。** 解码器的 IOMMU（`rockchip,rk3568-iommu`，在 +0x800 和 +0x900）和 VOP2 的是同一种：
   scanout.c 的页表代码多一个客户端。bring-up 驱动是在关闭分页的情况下运行的，所以 V2 的结果不依赖它。

## 编码：只定范围

- **硬件模块。** RK3576 有两个 `rkv-encoder-rk3576-core` 实例（MPP 把这一代叫 VEPU510；vendor dtsi：
  0x27A00000 和 0x27A10000，各有自己的 IOMMU，共用一个 CCU），用于 H.264 和 HEVC；还有一个 JPEG 编码器
  （`rkv-jpeg-encoder-v1`，0x27910800，和 JPEG 解码器共用 IOMMU）。RK3576 上没有 Hantro VEPU。
- **Mainline。** linux-next（2026-11-22）里这两个模块都没有驱动，也没有设备树节点。能读的源码只有 vendor 内核
  （`drivers/video/rockchip/mpp/mpp_rkvenc2.c`）和 MPP 的 `hal_h264e_vepu510.c` / `hal_h265e_vepu510.c`
  （Apache-2.0 OR MIT）。码率控制在 MPP 的用户态里做。
- **Windows 上的路径。** D3D11 没有视频编码 DDI（D3D12 有，而这套驱动是 D3D11）。编码器是以硬件 MFT
  （`MFT_ENUM_FLAG_HARDWARE`）的形式提供给应用（相机、Teams、Game Bar）的，所以编码终究还是走方案 (b)：
  一个基于 KMDF 驱动或第三个节点的编码器 MFT。
- **工作量。** 固件选项和电源：1 周。按 V1/V2 的模式做 bring-up 试验台（移植 MPP 的 VEPU510 H.264 HAL，
  输入原始 NV12，输出 Annex B，和 Linux 上的 MPP 比对）：4 到 6 周。带码率控制的硬件 MFT：再 4 到 6 周。
  V4 之前不值得开始。

## 板上测试，每个里程碑一项

固件由主会话负责刷写。每项测试都从一块现在能启动 Windows 的板子开始。每次固件或驱动改动只跑一项测试。

### V0：固件，选项关闭（预期没有变化）

固件：edk2-rk3576 `vdpu383-acpi-v2`。保持 *Video Decoder (VDPU383, experimental)*（视频解码器，实验性）为
Disabled，分别启动一次 Windows 和 Linux。

通过标准：两者都和以前一样启动；设备管理器里没有新设备。

### V1：电源、设备、驱动、寄存器

1. 在 Setup 的 *ACPI / Device Tree* 里：config table mode 要包含 ACPI；把 *Video
   Decoder (VDPU383, experimental)* 设为 Enabled；保存，重启。串口日志里应有：
   `RK3576Dxe: VDEC powered, version=0x...`。
2. Windows：设备管理器里出现一个未知设备 `ACPI\RKCP0383\0`。
3. 从 `vdpu383-v2` 的 WinPE workflow artifact `woa-drivers-signed-arm64` 里取文件，以管理员身份运行
   （先同步时钟：`w32tm /resync /force`）：

   ```
   certutil -addstore -f Root signer.cer
   certutil -addstore -f TrustedPublisher signer.cer
   pnputil /add-driver vdpu383\vdpu383.inf
   ```

   然后以 SYSTEM 身份，在 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0` 下的 `ConfigFlags`
   里置上 bit 0x20（CONFIGFLAG_REINSTALL），再重启。
4. 重启之后：

   ```
   reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"
   vdpu383test info
   ```

通过标准：`LastStep` 为 `D0Entry: done`，`Version` 非零且等于固件日志里的 `version=`，`RegsPa` 为 0x27b00000；
`vdpu383test info` 打印出相同的版本号，两个 IOMMU 都是分页关闭状态。

需要发回：上面两条命令的输出；如果串口日志里没有那行固件输出，也把串口日志发回。

### V2：解出一张图像，然后跑一致性测试

从同一个 ITU-T H.264.1 一致性测试 zip 里，把 `BA1_Sony_D.jsv` 和对应的参考 `.yuv` 复制到 `vdpu383test.exe` 旁边，然后：

```
vdpu383test warmup
vdpu383test decode BA1_Sony_D.jsv --ref REFERENCE.yuv -o out.yuv
```

通过标准：`reference: 17 frames; MATCH`，输出的 MD5 为
114d1cf94a2fcaffda0cf1b49964bf3d。如果失败，后续步骤（replay、寄存器转储、用来比对的 Linux 抓取）见
[驱动 README](../../drivers/video/vdpu383/README.zh-CN.md) 的 "Test plan" 第 2 到 4 步。
那里的完整码流列表就是 V2 的完成标准。

### V3 和 V4

定义在 wddm-mali-g52 `docs/D3D11-UMD.md` 的 U4 和 U5 里：`UmdVideo` 为 2 时 `d3dprobe` 列出 H.264 profile；
`edge://gpu` 显示 "Video Decode: Hardware accelerated"；强制 avc1 的 YouTube 播放 1080p 不丢帧；
Movies & TV 播放本地 1080p H.264 文件时 CPU 空闲。

## 回退

- **Windows 里出了任何问题：** 在固件设置里把 *Video Decoder* 设为 Disabled。这样 SSDT 不会安装，
  `ACPI\RKCP0383` 不存在，驱动也就不会加载。PD_VDEC 保持断电。这一步不需要进 Windows，所以启动时蓝屏也能用它恢复。
- **事后想看失败原因：** 选项关闭后，失败那次启动留下的痕迹（`LastStep` 等）仍然在该设备的
  `Device Parameters` 键里。
- **删除驱动：** 运行 `pnputil /enum-drivers`，找到 `vdpu383.inf` 对应的 `oemNN.inf`，然后在选项关闭的状态下运行
  `pnputil /delete-driver oemNN.inf`（没有绑定的设备，所以不会从正在运行的驱动底下把它卸掉）。
- **怀疑是预热（warm-up）的问题：** 它在 D0 时默认关闭（`WarmupAtD0` 0）。只有 `vdpu383test warmup` 会运行它。
- **固件：** 刷回之前的映像；没有这个选项的固件会忽略该选项的变量（`AcpiVdec`）。
