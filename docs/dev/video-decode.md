# Hardware video decode (and encode) on Windows: plan

Written 2026-11-22. Nothing in this plan has run on hardware. Board order:
LCKFB TaishanPi 3M first, then CM5-IO, then ROCK 4D (same SoC, same
firmware option).

## Decision: D3D11 video decode through the display driver

How a Windows app reaches a hardware decoder:

- **(a) D3D11VA**: the display driver's user-mode driver (UMD) exposes
  decoder profiles through `ID3D11VideoDevice`; the runtime hands it DXVA
  picture parameters, slice control and the bitstream; the kernel driver
  starts the engine and signals a fence.
- **(b) A Media Foundation decoder (MFT)** that talks to a separate KMDF
  driver and returns NV12 in system or D3D memory.

We take **(a)**. Reasons:

1. Edge and every Chromium build decode only through `ID3D11VideoDevice` on
   the D3D11 device they render with (`D3D11VideoDecoder`). They never load
   a third-party MFT. Without (a), YouTube stays on software decode whatever
   else exists.
2. Movies & TV, Media Player and anything else built on Media Foundation
   use Microsoft's own decoder MFTs (H.264 inbox; HEVC, VP9 and AV1 Video
   Extensions). Those MFTs are DXVA clients: given a D3D11 device with
   decoder profiles, they decode in hardware through it. So (a) also serves
   the MF apps, with no MFT of ours.
3. The decoded NV12 surface stays in memory the Mali samples and the VOP2
   scans out. An MFT on a separate driver has no shared D3D device, so every
   frame crosses the CPU.
4. One owner for the decoder's MMIO, interrupt and IOMMU, inside the
   driver that already owns the VOP2 and its IOMMU (same table code).

(b) is not pursued. It would only be worth doing if the display driver
never gains the video DDI, and then only for MF apps.

This matches wddm-mali-g52 `docs/D3D11-UMD.md` (U4 video DDI, U5 decode
node, decided 2026-11-21). The KMDF driver `drivers/video/vdpu383` on this
repository is the bring-up rig, not the product: it proves the register
programming, and its register builder (`drivers/video/vdpu383/lib`, plain
C, no allocation, no floats, DXVA structures in) moves to the UMD
unchanged.

Codec order: **H.264 first**. DXVA's H.264 structures are the simplest,
mainline's `rkvdec-vdpu383-h264.c` is the shortest path in mainline, the
ITU conformance suite ships reference YUV for every stream, and YouTube
serves avc1 to a browser that asks for it. HEVC next (mainline has
`rkvdec-vdpu383-hevc.c`), then VP9 and AV1 (MPP only; not in mainline for
VDPU383).

## 2026-11-24: the first player milestone (V3a)

The board passed V1 and V2 on 2026-11-24 (conformance MATCH, also
`--dxva-only`). The next step is written, not run: wddm-mali-g52 branch
`video-decode` (its `docs/VIDEO-DECODE.md` has the design and the board
test) gives the D3D11.1 interface and `ID3D11VideoDevice` with H.264 to
the player processes its `UmdVideo` names, and decodes in the player's
process through this driver's bring-up interface, with the register
builder copied unchanged. For that, vdpu383.sys on branch
`vdpu383-d3d11va` gains `UserAccess` (driver README): off by default,
1 lets users open the device. Decoding through maliwddm with the
decoder's IOMMU on (V3b) replaces it later.

## Milestones

| | Goal | Where | State | Effort |
|---|---|---|---|---|
| V1 | firmware powers PD_VDEC and declares the device; Windows binds a driver; registers readable | edk2-rk3576 `vdpu383-acpi-v2`, woa-rk3576 `vdpu383-v2` | code done, CI built, not run | board test only |
| V2 | decode H.264 frames from a stored bitstream in a test tool, compare with the reference YUV MD5 | `tools/vdpu383test`, `drivers/video/vdpu383/lib` | code done: parser, register builder, harness, replay, MD5; checked only with `--null` | 1 to 3 weeks of board iterations |
| V3 | app-facing path: `ID3D11VideoDevice` lists H.264 and `DecoderBeginFrame` decodes | wddm-mali-g52 U4 + U5, on a branch of its own | not started; needs U4 (video DDI) and a second node | 5 to 8 weeks after V2 |
| V4 | Edge YouTube (avc1) and Movies & TV play 1080p in hardware | same | not started | 2 to 4 weeks after V3 (NV12 presentation, reference surface lifetime, power) |

V2 is "as much as is sound" before a board run: every value that can be
read from mainline, MPP or the vendor kernel is in; the ones they disagree
on are written both ways (see the driver README, "Verified, derived,
guessed"). The next code changes have to come from what the board says.

### What V3 needs from the display driver (proposed interface, not made)

Nothing on the display driver's branches is changed by this work. Proposed
for whoever does U4/U5 (or for a `video-decode` branch from `hpd-edge`):

1. **Ownership.** The firmware option gains a value: `AcpiVdec` 1 publishes
   `ACPI\RKCP0383` for the bring-up driver (today); 2 would leave
   `RKCP0383` absent (`_STA` 0) and append the `\_SB.VDRS` resource
   template (MMIO 0x27B00000, GSIV 340/341, SRAM) to GPU0's `_CRS`, after
   the resources maliwddm already parses, so only one driver ever maps the
   block. Vdec.asl keeps the template in one named object for this.
2. **Node.** `MW_UMD_ADAPTER_INFO.DecodeNode` (already reserved in the
   adapter-info query) set to the decode node's ordinal; `UmdVideo` 2 turns
   the profiles on.
3. **Submission.** One private command per picture: the register image
   from `vdpu383_h264_build()` plus the allocation list (bitstream, output
   and reference NV12, RCB and aux buffers). The KMD patches IOVAs, checks
   every address register against the list (as `V383Run` does today),
   writes the registers, starts the core the way `V383Run` does, completes
   the fence from the interrupt, and on timeout performs `V383IpReset`.
4. **IOMMU.** The decoder's IOMMU (`rockchip,rk3568-iommu`, at +0x800 and
   +0x900) is the same kind as the VOP2's: the scanout.c table code gains a
   third client. The bring-up driver runs with paging off, so V2 results do
   not depend on it.

## Encode: scope only

- **Block.** RK3576 has two `rkv-encoder-rk3576-core` instances (MPP calls
  the generation VEPU510; vendor dtsi: 0x27A00000 and 0x27A10000, each with
  its own IOMMU, sharing a CCU) for H.264 and HEVC, and a JPEG encoder
  (`rkv-jpeg-encoder-v1`, 0x27910800, IOMMU shared with the JPEG decoder).
  There is no Hantro VEPU on RK3576.
- **Mainline.** No driver and no device-tree node for either block in
  linux-next (2026-11-22). The only readable sources are the vendor kernel
  (`drivers/video/rockchip/mpp/mpp_rkvenc2.c`) and MPP's
  `hal_h264e_vepu510.c` / `hal_h265e_vepu510.c` (Apache-2.0 OR MIT). Rate
  control runs in MPP user space.
- **Windows path.** D3D11 has no video-encode DDI (D3D12 has one, and this
  driver stack is D3D11). Encoders reach apps (Camera, Teams, Game Bar) as
  hardware MFTs (`MFT_ENUM_FLAG_HARDWARE`), so encode is option (b) after
  all: an encoder MFT over a KMDF driver or a third node.
- **Effort.** Firmware option and power: 1 week. Bring-up rig on the V1/V2
  pattern (port MPP's VEPU510 H.264 HAL, raw NV12 in, Annex B out, compare
  against MPP on Linux): 4 to 6 weeks. Hardware MFT with rate control:
  4 to 6 weeks more. Not worth starting before V4.

## Board tests, one per milestone

Firmware flashing is done by the main session. Every test starts from a
board that boots Windows today. Run one test per firmware or driver change.

### V0: firmware, option off (no change expected)

Firmware: edk2-rk3576 `vdpu383-acpi-v2`. Boot Windows and Linux once with
*Video Decoder (VDPU383, experimental)* left Disabled.

Pass: both boot as before; Device Manager has no new device.

### V1: power, device, driver, registers

1. Setup, *ACPI / Device Tree*: config table mode includes ACPI; *Video
   Decoder (VDPU383, experimental)* Enabled; save, reset. Serial log:
   `RK3576Dxe: VDEC powered, version=0x...`.
2. Windows: Device Manager shows an unknown device `ACPI\RKCP0383\0`.
3. From the WinPE workflow artifact `woa-drivers-signed-arm64` of
   `vdpu383-v2`, as administrator (clock synced first,
   `w32tm /resync /force`):

   ```
   certutil -addstore -f Root signer.cer
   certutil -addstore -f TrustedPublisher signer.cer
   pnputil /add-driver vdpu383\vdpu383.inf
   ```

   then, as SYSTEM, set bit 0x20 (CONFIGFLAG_REINSTALL) in `ConfigFlags`
   under `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0`, and restart.
4. After the restart:

   ```
   reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"
   vdpu383test info
   ```

Pass: `LastStep` is `D0Entry: done`, `Version` non-zero and equal to the
firmware's `version=`, `RegsPa` 0x27b00000; `vdpu383test info` prints the
same version and both IOMMUs with paging off.

Send back: both outputs, and the serial log if the firmware line is
missing.

### V2: one decoded picture, then conformance

Copy `BA1_Sony_D.jsv` and the reference `.yuv` from the same ITU-T
H.264.1 conformance zip next to `vdpu383test.exe`, then:

```
vdpu383test warmup
vdpu383test decode BA1_Sony_D.jsv --ref REFERENCE.yuv -o out.yuv
```

Pass: `reference: 17 frames; MATCH`, output MD5
114d1cf94a2fcaffda0cf1b49964bf3d. If it fails, the next steps (replay,
register dumps, the Linux capture to diff against) are in
[the driver README](../../drivers/video/vdpu383/README.md), "Test plan" 2 to 4.
The full stream list there is V2's exit criterion.

### V3 and V4

Defined in wddm-mali-g52 `docs/D3D11-UMD.md` U4 and U5: `d3dprobe` lists
the H.264 profile with `UmdVideo` 2; `edge://gpu` says "Video Decode:
Hardware accelerated"; YouTube forced to avc1 plays 1080p with no dropped
frames; Movies & TV plays a local 1080p H.264 file with the CPU idle.

## Back-out

- **Anything goes wrong in Windows:** firmware setup, *Video Decoder*
  Disabled. The SSDT is then not installed, `ACPI\RKCP0383` does not exist,
  and the driver never loads. PD_VDEC stays off. This needs no Windows
  access, so it also covers a boot that bugchecks.
- **To read why it failed afterwards:** with the option off, the
  breadcrumbs (`LastStep` and friends) are still in the device's
  `Device Parameters` key from the failed boot.
- **Remove the driver:** `pnputil /enum-drivers`, find `vdpu383.inf`'s
  `oemNN.inf`, then `pnputil /delete-driver oemNN.inf` with the option off
  (no device bound, so nothing is uninstalled from under a running driver).
- **Warm-up suspected:** it is off at D0 by default (`WarmupAtD0` 0). Only
  `vdpu383test warmup` runs it.
- **Firmware:** flash the previous image back; the option's variable
  (`AcpiVdec`) is ignored by firmware without the option.
