# Hardware video decode (and encode) on Windows

English | [简体中文](video-decode.zh-CN.md)

The plan for the RK3576 VDPU383 under Windows. Board order: TaishanPi 3M,
then CM5-IO, then ROCK 4D (same SoC, same firmware option). How to install
and test the bring-up driver: [vdpu383 README](../../drivers/video/vdpu383/README.md).

## Decision: D3D11 video decode through the display driver

Two ways for an app to reach a hardware decoder:

- (a) D3D11VA: the display driver's user-mode driver (UMD) exposes decoder
  profiles through `ID3D11VideoDevice`, takes DXVA picture parameters,
  slices and bitstream, and the kernel driver runs the engine and signals a
  fence.
- (b) A Media Foundation decoder (MFT) over a separate KMDF driver,
  returning NV12 in system or D3D memory.

We take (a):

1. Edge and Chromium decode only through `ID3D11VideoDevice` on their render
   device and never load a third-party MFT.
2. Movies & TV, Media Player and other MF apps use Microsoft's decoder MFTs
   (H.264 inbox; HEVC, VP9, AV1 extensions), which are DXVA clients. So (a)
   serves them too.
3. Decoded NV12 stays in memory the Mali samples and VOP2 scans out; an MFT
   on a separate driver copies every frame through the CPU.
4. One owner for the decoder's MMIO, interrupt and IOMMU: the driver that
   already owns VOP2 and its IOMMU.

The KMDF driver [vdpu383](../../drivers/video/vdpu383) is the bring-up rig,
not the product. Its register builder (`drivers/video/vdpu383/lib`, plain C,
no allocation, no floats, DXVA structures in) moves to the UMD unchanged.
This matches wddm-mali-g52 `docs/D3D11-UMD.md` (U4 video DDI, U5 decode
node).

Codec order: H.264 (simplest DXVA structures, mainline
`rkvdec-vdpu383-h264.c`, ITU conformance suite with reference YUV), then
HEVC (mainline `rkvdec-vdpu383-hevc.c`), then VP9 and AV1 (MPP only).

## Milestones

| | Goal | Where | Status |
|---|---|---|---|
| V1 | firmware powers PD_VDEC and declares `ACPI\RKCP0383`; Windows binds `vdpu383.sys`; registers readable | edk2-rk3576, `drivers/video/vdpu383` | board-tested (TaishanPi 3M) |
| V2 | `vdpu383test` decodes stored H.264 and matches the reference MD5 | `tools/vdpu383test`, `drivers/video/vdpu383/lib` | board-tested: six conformance streams match FFmpeg, 0 hardware errors |
| V3a | `ID3D11VideoDevice` with H.264 for the processes named in `UmdVideo`, decoding in the player's process through vdpu383's `UserAccess` interface | wddm-mali-g52 `video-decode`, woa-rk3576 `vdpu383-d3d11va` | branch |
| V3b | decode through maliwddm with the decoder's IOMMU on; replaces V3a | wddm-mali-g52 | not started |
| V4 | Edge YouTube (avc1) and Movies & TV play 1080p in hardware | wddm-mali-g52 | not started |

Other codecs in the bring-up driver (details and counts in its README):
HEVC Main and VP9 board-tested with `vdpu383test`, AV1 CI only. DXVA does
not carry the HEVC SPS short-term reference picture sets the decoder reads
from a table, so the HEVC builder (`lib/vdpu383_hevc.c`, from MPP's VDPU383
HEVC HAL) rebuilds that table from DXVA and checks it.

Values that mainline, MPP and the vendor kernel disagree on are written both
ways; see the driver README, "Hardware facts and their sources".

### What V3b needs from the display driver (proposed, not made)

1. Ownership. `AcpiVdec` 1 publishes `ACPI\RKCP0383` for the bring-up
   driver; 2 would leave it absent (`_STA` 0) and append the `\_SB.VDRS`
   resource template (MMIO 0x27B00000, GSIV 340/341, SRAM) to GPU0's `_CRS`,
   so only one driver maps the block. Vdec.asl keeps the template in one
   named object for this.
2. Node. `MW_UMD_ADAPTER_INFO.DecodeNode` (reserved) set to the decode
   node's ordinal; `UmdVideo` 2 turns the profiles on.
3. Submission. One private command per picture: the register image from
   `vdpu383_h264_build()` plus the allocation list (bitstream, output and
   reference NV12, RCB and aux buffers). The KMD patches IOVAs, checks every
   address register against the list (as `V383Run` does), starts the core,
   completes the fence from the interrupt, and runs `V383IpReset` on
   timeout.
4. IOMMU. The decoder's IOMMU (`rockchip,rk3568-iommu`, at +0x800 and +0x900)
   is the same kind as VOP2's, so the scanout.c table code gains a third
   client. vdpu383 runs it with its own tables (driver.c, IOMMU section),
   because physically contiguous multi-MiB buffers could not be had.

## Encode: scope only

- Blocks: two `rkv-encoder-rk3576-core` (VEPU510; vendor dtsi 0x27A00000
  and 0x27A10000, own IOMMUs, shared CCU) for H.264 and HEVC, and a JPEG
  encoder (`rkv-jpeg-encoder-v1`, 0x27910800, IOMMU shared with the JPEG
  decoder). No Hantro VEPU.
- Sources: no mainline driver or DT node. Vendor kernel
  `drivers/video/rockchip/mpp/mpp_rkvenc2.c` and MPP's
  `hal_h264e_vepu510.c` / `hal_h265e_vepu510.c` (Apache-2.0 OR MIT). Rate
  control is in MPP user space.
- Windows path: D3D11 has no video-encode DDI, so encoders reach apps
  (Camera, Teams, Game Bar) as hardware MFTs (`MFT_ENUM_FLAG_HARDWARE`):
  option (b) after all.
- Effort: firmware 1 week; bring-up rig (MPP's VEPU510 H.264 HAL, NV12 in,
  Annex B out, compared with MPP on Linux) 4 to 6 weeks; hardware MFT with
  rate control 4 to 6 weeks more. Not before V4.

## Board tests

V1 and V2 steps (install, `vdpu383test info`, `BA1_Sony_D.jsv` with
expected MD5 114d1cf94a2fcaffda0cf1b49964bf3d, the full stream list) are in
the [driver README](../../drivers/video/vdpu383/README.md). V3 and V4 pass
when `d3dprobe` lists the H.264 profile with `UmdVideo` 2, `edge://gpu` says
"Video Decode: Hardware accelerated", YouTube forced to avc1 plays 1080p
with no dropped frames, and Movies & TV plays a local 1080p H.264 file with
the CPU idle (wddm-mali-g52 `docs/D3D11-UMD.md` U4 and U5).

## Back-out

- Firmware setup, *Video Decoder* Disabled: no SSDT, no `ACPI\RKCP0383`,
  the driver never loads, PD_VDEC stays off. Needs no Windows access, so it
  also covers a boot that bugchecks.
- The breadcrumbs (`LastStep` and others) from a failed boot stay in the
  device's `Device Parameters` key.
- Remove the driver with the option off: `pnputil /enum-drivers`, find
  `vdpu383.inf`'s `oemNN.inf`, `pnputil /delete-driver oemNN.inf`.
- Warm-up runs at every D0 entry and after every IP reset (`WarmupAtD0` 1,
  the default; H.264 needs it). `WarmupAtD0` 0 turns it off;
  `vdpu383test warmup` runs it on demand.
- Firmware: flash the previous image; older firmware ignores `AcpiVdec`.
