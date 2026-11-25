# Windows 上的硬件视频解码（和编码）

[English](video-decode.md) | 简体中文

RK3576 VDPU383 在 Windows 下的计划。板子顺序：泰山派 3M，然后 CM5-IO，然后 ROCK 4D（同一颗 SoC，同一个固件选项）。调试驱动的安装和测试见 [vdpu383 README](../../drivers/video/vdpu383/README.zh-CN.md)。

## 决定：通过显示驱动做 D3D11 视频解码

应用使用硬件解码器有两条路：

- (a) D3D11VA：显示驱动的用户态驱动（UMD）通过 `ID3D11VideoDevice` 提供解码器 profile，接收 DXVA 图像参数、slice 和码流，由内核驱动启动解码引擎并发出 fence 信号。
- (b) 一个 Media Foundation 解码器（MFT），通过单独的 KMDF 驱动工作，把 NV12 返回到系统内存或 D3D 内存。

我们选 (a)：

1. Edge 和 Chromium 只通过渲染用的 D3D11 设备上的 `ID3D11VideoDevice` 解码，从不加载第三方 MFT。
2. Movies & TV、Media Player 等 MF 应用用的是微软自己的解码器 MFT（H.264 系统自带；HEVC、VP9、AV1 扩展），它们是 DXVA 客户端。所以 (a) 也能服务它们。
3. 解码出的 NV12 留在 Mali 能采样、VOP2 能扫描输出的内存里；单独驱动上的 MFT 每一帧都要经过 CPU。
4. 解码器的 MMIO、中断和 IOMMU 只有一个所有者：已经管着 VOP2 及其 IOMMU 的那个驱动。

KMDF 驱动 [vdpu383](../../drivers/video/vdpu383) 是调试用的试验台，不是最终产品。它的寄存器构建器（`drivers/video/vdpu383/lib`，纯 C，不分配内存，不用浮点，输入是 DXVA 结构体）会原样搬进 UMD。这和 wddm-mali-g52 的 `docs/D3D11-UMD.md`（U4 视频 DDI，U5 解码节点）一致。

编解码器顺序：H.264（DXVA 结构体最简单，有 mainline 的 `rkvdec-vdpu383-h264.c`，ITU 一致性测试集带参考 YUV），然后 HEVC（mainline 的 `rkvdec-vdpu383-hevc.c`），然后 VP9 和 AV1（只有 MPP）。

## 里程碑

| | 目标 | 在哪里 | 状态 |
|---|---|---|---|
| V1 | 固件给 PD_VDEC 上电并声明 `ACPI\RKCP0383`；Windows 绑定 `vdpu383.sys`；寄存器可读 | edk2-rk3576、`drivers/video/vdpu383` | 板测（泰山派 3M） |
| V2 | `vdpu383test` 解码存好的 H.264，与参考 MD5 一致 | `tools/vdpu383test`、`drivers/video/vdpu383/lib` | 板测：六条一致性码流与 FFmpeg 一致，0 个硬件错误 |
| V3a | 给 `UmdVideo` 列出的进程提供带 H.264 的 `ID3D11VideoDevice`，在播放器进程里通过 vdpu383 的 `UserAccess` 接口解码 | wddm-mali-g52 `video-decode`、woa-rk3576 `vdpu383-d3d11va` | 分支 |
| V3b | 打开解码器 IOMMU，通过 maliwddm 解码；取代 V3a | wddm-mali-g52 | 未开始 |
| V4 | Edge 上的 YouTube（avc1）和 Movies & TV 硬件播放 1080p | wddm-mali-g52 | 未开始 |

调试驱动里的其他编解码器（细节和数字见它的 README）：HEVC Main 和 VP9 已用 `vdpu383test` 板测，AV1 仅 CI。解码器从一张表里读 HEVC SPS 的短期参考图像集，而 DXVA 不带这些信息，所以 HEVC 构建器（`lib/vdpu383_hevc.c`，来自 MPP 的 VDPU383 HEVC HAL）从 DXVA 重建这张表并加以检查。

mainline、MPP 和厂商内核说法不一致的值，两种写法都保留了；见驱动 README 的"硬件事实和来源"。

### V3b 需要显示驱动提供什么（提议，尚未实现）

1. 归属。`AcpiVdec` 为 1 时给调试驱动发布 `ACPI\RKCP0383`；为 2 时不发布它（`_STA` 0），并把 `\_SB.VDRS` 资源模板（MMIO 0x27B00000、GSIV 340/341、SRAM）附加到 GPU0 的 `_CRS` 后面，这样只有一个驱动映射这个模块。Vdec.asl 为此把模板放在一个具名对象里。
2. 节点。`MW_UMD_ADAPTER_INFO.DecodeNode`（已预留）设为解码节点的序号；`UmdVideo` 为 2 时打开 profile。
3. 提交。每张图一个私有命令：`vdpu383_h264_build()` 生成的寄存器映像加分配列表（码流、输出和参考 NV12、RCB 和辅助缓冲区）。KMD 填入 IOVA，按列表检查每个地址寄存器（和 `V383Run` 一样），启动内核，在中断里完成 fence，超时则执行 `V383IpReset`。
4. IOMMU。解码器的 IOMMU（`rockchip,rk3568-iommu`，在 +0x800 和 +0x900）和 VOP2 的是同一种，所以 scanout.c 的页表代码多一个客户。vdpu383 用自己的页表驱动它（driver.c 的 IOMMU 部分），因为在板上拿不到物理连续的数 MiB 缓冲区。

## 编码：只定范围

- 模块：两个 `rkv-encoder-rk3576-core`（VEPU510；厂商 dtsi 里在 0x27A00000 和 0x27A10000，各有 IOMMU，共用一个 CCU），负责 H.264 和 HEVC；一个 JPEG 编码器（`rkv-jpeg-encoder-v1`，0x27910800，与 JPEG 解码器共用 IOMMU）。没有 Hantro VEPU。
- 资料：mainline 没有驱动也没有设备树节点。厂商内核的 `drivers/video/rockchip/mpp/mpp_rkvenc2.c`，以及 MPP 的 `hal_h264e_vepu510.c` / `hal_h265e_vepu510.c`（Apache-2.0 OR MIT）。码率控制在 MPP 用户态。
- Windows 路径：D3D11 没有视频编码 DDI，编码器以硬件 MFT（`MFT_ENUM_FLAG_HARDWARE`）的形式提供给应用（相机、Teams、Game Bar），也就是方案 (b)。
- 工作量：固件 1 周；试验台（移植 MPP 的 VEPU510 H.264 HAL，NV12 进，Annex B 出，和 Linux 上的 MPP 对比）4 到 6 周；带码率控制的硬件 MFT 再 4 到 6 周。V4 之前不做。

## 板测

V1 和 V2 的步骤（安装、`vdpu383test info`、`BA1_Sony_D.jsv` 及其预期 MD5 114d1cf94a2fcaffda0cf1b49964bf3d、完整码流列表）见[驱动 README](../../drivers/video/vdpu383/README.zh-CN.md)。V3 和 V4 的通过条件：`UmdVideo` 为 2 时 `d3dprobe` 列出 H.264 profile；`edge://gpu` 显示 "Video Decode: Hardware accelerated"；强制 avc1 的 YouTube 播放 1080p 不丢帧；Movies & TV 播放本地 1080p H.264 文件时 CPU 空闲（wddm-mali-g52 `docs/D3D11-UMD.md` 的 U4 和 U5）。

## 回退

- 固件设置里把 *Video Decoder* 设为 Disabled：不装 SSDT，没有 `ACPI\RKCP0383`，驱动不会加载，PD_VDEC 保持关闭。不需要进 Windows，所以启动蓝屏时也能用。
- 失败那次启动留下的痕迹（`LastStep` 等）仍在设备的 `Device Parameters` 键里。
- 在选项关闭时删除驱动：`pnputil /enum-drivers` 找到 `vdpu383.inf` 对应的 `oemNN.inf`，然后 `pnputil /delete-driver oemNN.inf`。
- 预热在每次进入 D0 和每次 IP 复位后运行（`WarmupAtD0` 为 1，默认；H.264 需要它）。`WarmupAtD0` 设为 0 关闭；`vdpu383test warmup` 可手动运行。
- 固件：刷回上一个映像；旧固件会忽略 `AcpiVdec`。
