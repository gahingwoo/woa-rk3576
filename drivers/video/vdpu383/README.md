# vdpu383: RK3576 video decoder, bring-up

English | [简体中文](README.zh-CN.md)

The RK3576's H.264/HEVC/VP9/AV1 decoder (RKVDEC, VDPU383 in Rockchip's
naming), driven from Windows. This is the prototype: a KMDF driver that
exposes the block to a test program, and the program, which parses H.264,
builds the decoder's register file and checks the output against the
conformance streams' reference pictures. Applications do not see it.

The decoder that Edge will use is planned inside the display driver
([wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52), docs/D3D11-UMD.md,
milestone U5) as a VIDEO_DECODE node, because D3D11 video has to run on the
same device as ANGLE. The register builder in [lib](lib) is written to move
there unchanged: plain C, no KMDF, WDM or C library calls, no allocation, no
floating point, DXVA structures in.

**Run on the TaishanPi 3M on 2026-11-24:** with *Video Decoder (VDPU383)*
enabled in the firmware setup (off by default), Windows binds `vdpu383.sys`
to `ACPI\RKCP0383`, and the six H.264 conformance streams tried decode bit
for bit like FFmpeg's references, with 0 hardware errors (edk2-rk3576 merge
`a23adb9`, [features](../../../docs/features.md#video-decoder)). It compiles
in CI; the parser and the register builder also run on Linux with no decoder
(`--null`).

## What is where

| Path | What |
|---|---|
| [lib/vdpu383_h264.c](lib/vdpu383_h264.c) | DXVA_PicParams_H264 + DXVA_Qmatrix_H264 + bitstream to registers 0..383 and the auxiliary buffer. From Rockchip MPP (Apache-2.0 OR MIT, used as MIT) |
| [lib/v383_dxva.h](lib/v383_dxva.h) | the DXVA H.264 structures, byte for byte dxva.h's layout (checked against dxva.h at compile time) |
| [lib/vdpu383_hevc.c](lib/vdpu383_hevc.c) | the same for HEVC Main: DXVA_PicParams_HEVC + DXVA_Qmatrix_HEVC + bitstream, from MPP, checked against mainline's rkvdec-vdpu383-hevc.c; rebuilds the SPS's reference picture sets, which DXVA does not carry (see the top of the file). Not run on the board |
| [lib/v383_dxva_hevc.h](lib/v383_dxva_hevc.h) | the DXVA HEVC structures, dxva.h's layout |
| [lib/vdpu383_vp9.c](lib/vdpu383_vp9.c) | VP9 profile 0 and 2 (8 and 10 bit 4:2:0): DXVA_PicParams_VP9 + the frame to registers, the global record and the probability tables. Registers and buffer handling from MPP's hal_vp9d_vdpu383.c (MIT); the packers written fresh to the layout of MPP's hal_vp9d_com.c (Apache-2.0 only, nothing copied), byte for byte equal to MPP's output. The decoder reads the compressed header and adapts the probabilities itself; the builder keeps the four frame contexts, the segment maps and the motion vectors between frames (V383_Vp9State). Not run on the board |
| [lib/vdpu383_vp9_probs.c](lib/vdpu383_vp9_probs.c), [lib/v383_dxva_vp9.h](lib/v383_dxva_vp9.h) | the VP9 default and key-frame probabilities (mainline v4l2-vp9.c, GPL-2.0); the DXVA VP9 structures, dxva.h's layout |
| [driver.c](driver.c), [warmup.c](warmup.c) | the KMDF driver for `ACPI\RKCP0383` |
| [vdpu383_ioctl.h](vdpu383_ioctl.h) | the driver's interface (`\\.\VDPU383`) |
| [../../../tools/vdpu383test](../../../tools/vdpu383test) | the test program and its H.264, HEVC and VP9 (IVF) parsers |
| [../../../tools/vdpu383-capture](../../../tools/vdpu383-capture) | what mainline Linux does with the same clips, and a register diff |

The firmware side is edk2-rk3576 branch `vdpu383-acpi-v2` (the old
`vdpu383-acpi` rebased onto main with the NPU option kept): a setup option, off
by default, that powers PD_VDEC, sets the decoder clocks and installs an
SSDT with the device.

## Verified, derived, guessed

| Item | Source | Status |
|---|---|---|
| Registers at 0x27B00000 (link +0, function +0x100, cache +0x600, IOMMU +0x800/+0x900), GSIV 340/341, PD_VDEC bits, CRU bits, SRAM 0x3FF88000 480 KiB | mainline rk3576.dtsi, pm-domains.c, clk-rk3576.c, rst-rk3576.c | read from source, not measured |
| Start: link 0x54 = timeout, 0x58 = 0, 0x40 = 1; interrupt: read 0x48/0x4C, write 0x03FF0000 to 0x4C and 0x00030000 to 0x48 | mainline rkvdec-vdpu383-h264.c, rkvdec.c | read from source |
| Reference read cache at +0x600: CACHE0_CFG = 0x13, MAX_READS = 0x1C at D0 entry and after every IP reset; CLR_CACHE0 = 1 before every decode | mainline "configure the reference read cache" (v3, merged) | ported; mainline measured a 2688x1520 stream at 22.5 or 65.4 ms per frame with the power-on values and 3.6 ms with these. Reprogramming after a reset is ours: mainline left open whether the reset clears them |
| A timed-out decode always gets an IP reset (link 0x48 irq off, 0x44 = 1, wait for status bit 11, clear status), then queued DPCs are drained | mainline "Stop the VDPU383 core on timeout" (posted, compile-tested there) and "Handle interrupts without a current context" | ported; an error status still resets only with `VDPU383_RUN_RESET_ON_ERROR` |
| Warm-up decode | vendor kernel mpp_hack_rk3576.c (GPL-2.0+ OR MIT) | ported unchanged; mainline does not do it, so it may be unnecessary. At D0 entry only with Device Parameters `WarmupAtD0` = 1 (default 0); `vdpu383test warmup` runs it on demand |
| Register values, SPS/PPS/RPS bit layouts, RCB sizes, CABAC table | MPP hal_h264d_vdpu383.c and friends | ported; RCB arithmetic changed from float to integer (same values, checked in `selftest`) |
| SPS/PPS record written both at offset 0 (MPP) and at pps_id * 176 (mainline) | the two disagree | whichever the hardware reads is covered |
| Default reference lists P/B0/B1 from DXVA_PicParams_H264 | H.264 8.2.4.2, as mainline hands them to the hardware | written fresh |
| profile_idc, seq_parameter_set_id, constraint_set3_flag | not in DXVA | the tool passes the real ones; `--dxva-only` passes guesses, to find out whether the hardware cares |
| DXVA_Qmatrix_H264 in zig-zag order | the DXVA spec as FFmpeg's dxva2_h264.c reads it | not checked against a Windows decoder |
| IOMMU paging on with the driver's own v2 tables (one directory for both instances +0x800/+0x900, 4 KiB pages, 32-bit IO addresses from 16 MiB up, SRAM and warm-up buffer at their physical addresses); enable/disable/fault sequences of rk_iommu_enable/disable/irq, no FORCE_RESET (rockchip,disable-mmu-reset); paging restored after every IP reset and decode error | mainline rockchip-iommu.c (rk3576.dtsi: "rockchip,rk3576-iommu", "rockchip,rk3568-iommu" = v2), rkvdec.c rkvdec_iommu_restore(), rkvdec-rcb.c; the VOP2 scan-out of wddm-mali-g52 runs the same IOMMU type with the same entry format on the board | ported, not run on the board yet. Device Parameters `Iommu` 0 gives the old path (paging off, physical addresses, contiguous buffers below 4 GiB), which failed for 4 MiB surfaces with error 1450 on 2026-11-25 |
| Buffers write-combined (Normal non-cacheable) for a non-coherent device | | guessed |
| Field pictures (PAFF), FMO, 4:2:2, 10 bit, MVC | | not supported; MBAFF frames pass through untested |

## Breadcrumbs and settings

The driver keeps its last step and what it read in the device's hardware
key, flushed to disk at once, so a hang or a bugcheck leaves them readable
after the next boot (boot with the setup option off if the decoder is what
stops Windows):

```
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters"
```

| Value | Meaning |
|---|---|
| `LastStep`, `PrevStep`, `StepSeq` | the step the driver was in, the one before, and a counter. `D0Entry: first register read` as LastStep after a bugcheck means the decoder's power domain or clocks were off: the firmware did not power it |
| `RegsPa`, `SramPa` | the resources Windows gave it (expect 0x27B00000, 0x3FF88000) |
| `Version`, `Mmu0Status`, `Mmu1Status`, `LinkIrqAtD0`, `LinkStatusAtD0` | the first reads at D0 entry |
| `CacheCfgPowerOn`, `CacheMaxReadsPowerOn` | the read cache before the driver set it |
| `WarmupIrq`, `WarmupStatus`, `WarmupTimedOut` | the last warm-up, at D0 entry or from the tool |
| `FirstRunResult`, `FirstRunStatus` | the first decode of the session (0 = ok) |
| `IommuSetting`, `IommuOn`, `MmuDte0`, `MmuEnableStatus0/1` | the `Iommu` setting read, whether paging came on at D0 entry (1) or the driver fell back to physical addresses (0), the directory register and both statuses after enabling (bit 0 = paging) |
| `LastMmuFaultAddr` | the IO address of the last decode failed with an IOMMU fault |

Settings, DWORDs in the same key, read at each start of the device:

| Setting | Default | Meaning |
|---|---|---|
| `WarmupAtD0` | 0 | 1: run the vendor kernel's warm-up decode at every D0 entry, as the vendor kernel does at probe and resume |
| `Iommu` | 1 | 1: buffers are any pages, mapped through the decoder's IOMMU, and `dma` is an IO address. 0: the old path, physically contiguous buffers below 4 GiB, paging off. Takes effect at the next start of the device (disable/enable or reboot) |
| `UserAccess` | 0 | 0: SYSTEM and administrators only, one handle at a time (vdpu383test). 1: every user may open `\\.\VDPU383`, many handles at once, so that a video player's process reaches it through the display driver's user-mode part (wddm-mali-g52 `video-decode`, D3D11 video decode milestone V3a); 2: low-integrity processes too (a browser's GPU process). With 1 or 2 `VDPU383_RUN_NO_ADDR_CHECK` is ignored, and each handle may hold at most 384 MiB of decoder buffers. Read when the device starts: set it, then restart the device or the board |

Security note on `UserAccess`: the bring-up interface lets the caller write
the decoder's registers. The driver checks that every address register of
a run points into a buffer that handle allocated (or the SRAM), but not
that the decoder stays inside it. With `Iommu` 0 the decoder's IOMMU is
off, so a process that can open the device can make the decoder read or
write memory past the end of its own buffers. With `Iommu` 1 (the default)
the decoder reaches only what is mapped in its IOMMU, but that is every open
handle's buffers, the SRAM and the warm-up buffer, so one process can still
reach another's buffers. That is why `UserAccess` is 0 by default and meant
for a test board only; the product path is the decode node inside maliwddm
(docs/dev/video-decode.md, V3b).

## Test plan

Run in this order. Each step says what to send back if it fails.

### 0. Firmware

Flash the `vdpu383-acpi-v2` firmware (CI artifact `cm5io-firmware` or
`tspi3m-firmware` of the edk2-rk3576 run on that branch). With the option
still off, boot once to Windows and Linux as usual: nothing must have
changed.

Then in setup, *ACPI / Device Tree*: the config table mode must include ACPI;
set *Video Decoder (VDPU383, experimental)* to Enabled, save, reset. The
serial log should then show:

```
RK3576Dxe: VDEC: gpll 1188000000 Hz, cpll 1000000000 Hz -> aclk 594000000, aclk_bak 500000000, hevc_ca 1000000000 Hz
RK3576Dxe: VDEC powered, version=0x........ mmu status=0x........./0x........
```

(or `VDEC power domain was already on` before the second line). In the UEFI
shell, `acpiview -s SSDT` lists a table with OEM table ID `RKVDEC`.

Send back on failure: the serial log from power-on to the boot manager.

### 1. Install

From the WinPE workflow run of branch `vdpu383-v2`, artifact
`woa-drivers-signed-arm64`: `woa-drivers\vdpu383\` (the signed package),
`woa-drivers\signer.cer` and `woa-tools\` (vdpu383test.exe and the Linux
capture scripts). The WinPE image of that run already contains the driver.

On an installed Windows, as administrator, after `w32tm /resync /force`:

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
pnputil /add-driver vdpu383\vdpu383.inf /install
```

Device Manager, *System devices*: "Rockchip RK3576 Video Decoder (VDPU383,
bring-up)", no problem code. With the firmware option off the device does
not exist and the driver binds nothing.

Send back on failure: `pnputil /enum-devices /instanceid "ACPI\RKCP0383\0" /drivers`
(or the problem code shown), and `C:\Windows\INF\setupapi.dev.log` lines
around `RKCP0383`.

### 2. Power and warm-up

```
vdpu383test info
```

Expected (from the vendor code, not measured):

- `version register` non-zero, the same as the firmware's `version=` line
- both IOMMU instances `paging ON` with the same `dte` (`paging off` with Device Parameters `Iommu` 0, and with drivers before branch `vdpu383-iommu`)
- `warm-up at D0 entry: not run` (the default, `WarmupAtD0` 0)

Then `vdpu383test warmup` (through the link registers, with interrupts
live): `irq 0x0000000?`, status bit 0 (`frame_rdy`) set and bits 1-9
clear, `verdict: PASS`. Bits 10-15 may be set; they are not errors. Then
`vdpu383test warmup-direct` (the same 32x32 picture
through the ordinary start path). Run `warmup-direct` three times: the
output MD5 must be the same each time.

Send back: the output of all three commands, whatever they print.

### 3. Replay

```
vdpu383test decode BA1_Sony_D.jsv -n 3 --dump C:\v383 --readback
vdpu383test replay C:\v383\pic_0000\replay.txt
vdpu383test replay C:\v383\pic_0002\replay.txt
```

Each replay rewrites one picture's register file and buffers from the dump
and checks the output against the MD5 recorded when it was decoded:
`replay: ok ... MATCH`. A mismatch with a good status means the decode is
not deterministic (memory ordering, cache attributes); a different status
means state outside the register file matters.

Send back on failure: the console output and `C:\v383` zipped.

### 4. H.264 conformance

Streams from the ITU-T H.264.1 conformance suite,
https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/ (AVCv1/ and
FRExt/), each a zip with the stream and the reference output. For each:

```
vdpu383test decode STREAM --ref REFERENCE.yuv -o out.yuv
```

Pass: `reference: N frames; MATCH`, and `output md5` equal to the MD5 of the
reference file below. Then the same with `--dxva-only` added: the same MD5
means the hardware needs nothing beyond DXVA plus the slice header's pps id.

| Stream (zip) | File | What it covers | Frames | Reference MD5 |
|---|---|---|---|---|
| BA1_Sony_D | BA1_Sony_D.jsv | Baseline, CAVLC, I and P, QCIF | 17 | 114d1cf94a2fcaffda0cf1b49964bf3d |
| SVA_BA1_B | SVA_BA1_B.264 | Baseline | 17 | dab92aa2145ab44abab2beb2868dd326 |
| BA_MW_D | BA_MW_D.264 | Baseline, several slices | 100 | 7d5d351ad061640294bf43a43150fbca |
| BANM_MW_D | BANM_MW_D.264 | Baseline, nal_ref_idc variety | 100 | e637d38ed004df3540218e3d84b43e42 |
| MIDR_MW_D | MIDR_MW_D.264 | several IDRs | 100 | d87bff88b2c5b96ccb291ef68a45bbc2 |
| NRF_MW_E | NRF_MW_E.264 | non-reference frames | 100 | a8635615b50c5a16decc555a3c6c81c8 |
| MR1_BT_A | MR1_BT_A.h264 | multiple reference frames | 62 | 6ea31a214aadd8bdc8e7d37195d91c81 |
| MR2_TANDBERG_E | MR2_TANDBERG_E.264 | multiple references, reordering | 300 | d154bf9264960fecc6d2cf72be4cf8cc |
| CABA1_SVA_B | CABA1_SVA_B.264 | Main, CABAC, I | 17 | 2f5cabd5bb4954c0386cafd8a9aa782a |
| CABA2_SVA_B | CABA2_SVA_B.264 | CABAC, P | 17 | 6d4277a7fc70ed1fbe3c5f10b0a70671 |
| CABA3_SVA_B | CABA3_SVA_B.264 | CABAC, B | 33 | 63a0f8fdcbb87b0dff330acdd10905c0 |
| CABA3_TOSHIBA_E | CABA3_TOSHIBA_E.264 | CABAC, B, 300 frames | 300 | 13651d01c5b533e6b7aa132bae7669ed |
| CANL1_TOSHIBA_G | CANL1_TOSHIBA_G.264 | CABAC, deblocking off | 300 | afa072746b16bd96f3152b45e2f2881e |
| CABAST3_Sony_E | CABAST3_Sony_E.jsv | CABAC, slice types, CIF | 25 | e95f27c3779af519c1a317adb2f699a5 |
| CAPM3_Sony_D | CAPM3_Sony_D.jsv | CABAC, pictures with mixed slice types | 300 | 9ecc3bf5ff7cac9a068a5ba57bc87cb7 |
| CAWP1_TOSHIBA_E | CAWP1_TOSHIBA_E.264 | weighted prediction, CIF | 90 | 87946607d1d774c2da8ec8632710c84a |
| CVWP2_TOSHIBA_E | CVWP2_TOSHIBA_E.264 | weighted prediction, CAVLC | 90 | 7616ccc1e4cc89805cf5ae11e4437533 |
| FRExt1_Panasonic_D | FRExt1_Panasonic.avc | High, 8x8 transform | 8 | f7023a3a93cc98c0c8d83c1deaf11048 |
| FRExt3_Panasonic_E | FRExt3_Panasonic.avc | High | 11 | 231e4f3105f75a3602161ec484bbd8e3 |
| HPCV_BRCM_A | HPCV_BRCM_A.264 | High, CAVLC, CIF | 300 | 5eafc104844211893e1d8af0b8b66ed6 |
| HPCAQ2LQ_BRCM_B | HPCAQ2LQ_BRCM_B.264 | High, CABAC, scaling matrices | 100 | cf81785c89930d5b1c191a1ef4cb9e83 |

The MD5s are of the reference .yuv in each zip; FFmpeg's software decode of
each stream gives the same MD5 (checked 2026-11-21). Start with BA1_Sony_D,
then CABA1_SVA_B, CABA3_SVA_B, MR2_TANDBERG_E, FRExt1_Panasonic_D and
HPCAQ2LQ_BRCM_B: each adds one feature, so the first failure names it.

Send back on failure, for the first stream that fails:

1. the full console output of
   `vdpu383test decode STREAM --ref REF.yuv --frame-md5 --readback --regs -n 8 > win.txt`
2. `vdpu383test decode STREAM -n 4 --dump C:\v383` with `C:\v383` zipped
3. the Linux capture of the same stream (below) and `compare-regs.py`
   output for the first picture that differs
4. `vdpu383test info` after the failure

### 5. HEVC Main conformance

Streams: FFmpeg's fate-suite copies of the JCT-VC conformance streams,
`https://fate-suite.ffmpeg.org/hevc-conformance/NAME.bit`, the 75 8-bit 4:2:0
ones listed in [hevc-conformance.txt](../../../tools/vdpu383test/hevc-conformance.txt)
with the frame count and the MD5 of FFmpeg 6.1.1's I420 output for each
(made 2026-11-25, `-flags unaligned` as in fate: CONFWIN_A_Sony_1's
reference was first made without it, 414 wide, and is corrected). Board
2026-11-25 with the contiguous buffers: 60/75 MATCH, 0 hardware errors;
the 14 large or 4 MiB-stream ones failed to allocate (error 1450) before
picture 0, and CONFWIN_A_Sony_1's output is the corrected reference's. On Linux every one of them goes through the parser and
the register builder with `--null`: FFmpeg's frame count, and every
picture's reference sets derived from its slice headers (as the decoder
derives them) equal to DXVA's, with the SPS's sets and with the table
rebuilt from DXVA alone. CI does that for nine of them.

vdpu383test with HEVC: CI artifact `rk3576-woa-vdpu383test-arm64` of
branch `vdpu383-hevc` (run 45383528190 or later); vdpu383.sys as for H.264.
On the board, as administrator, the streams in one folder:

```
powershell -ExecutionPolicy Bypass -File hevc-conformance.ps1 -Dir D:\hevc\streams -Only RPS_A
powershell -ExecutionPolicy Bypass -File hevc-conformance.ps1 -Dir D:\hevc\streams
powershell -ExecutionPolicy Bypass -File hevc-conformance.ps1 -Dir D:\hevc\streams -DxvaOnly -Out .\hevc-dxva
```

Pass: `MATCH` on every line (the output MD5 is FFmpeg's, the frame count
too, no hardware errors), first with the parser's SPS sets, then with
`-DxvaOnly`: the same MD5 there means a D3D11 driver, which has only DXVA,
decodes HEVC with this builder. In that order: RPS_A_docomo_4 (one slice,
P and B), then the whole list; it names the first feature that fails
(SLIST_* scaling lists, TILES_* tiles, WPP_*/ENTP_* entry points, DSLICE_*
dependent slices, LTRPSPS_A long-term pictures from the SPS, RAP_* leading
pictures). AMP_A/B (2560x1600) need about 170 MiB of decoder buffers.

Send back on failure: `hevc-results\hevc-results.txt` and the first failing
stream's `NAME.txt`, then `vdpu383test decode NAME.bit --frame-md5 --regs -n 4 > win.txt`
and `vdpu383test info`. FFmpeg's per-frame MD5s for comparing `--frame-md5`
are in the streams folder of the bundle (`framemd5\NAME.framemd5`).

### 6. IOMMU buffers (branch `vdpu383-iommu`)

The driver of that branch maps every buffer through the decoder's IOMMU.
Install it staged, not hot: add it to the driver store, mark the device for
reinstall, reboot. As administrator, from the WinPE workflow run's
`woa-drivers-signed-arm64` (`woa-drivers\vdpu383\`, `woa-drivers\signer.cer`,
`woa-tools\vdpu383test.exe`; the CI workflow's `rk3576-woa-vdpu383-arm64` is
unsigned). The test certificate changes with every run, so import this
run's:

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
pnputil /add-driver vdpu383\vdpu383.inf
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0" /v ConfigFlags /t REG_DWORD /d 0x20 /f
shutdown /r /t 0
```

(Without a `signer.cer`, export the certificate from the `.cat`:
Properties, *Digital Signatures*, *Details*, *View Certificate*, *Copy to
File*.) After the reboot `pnputil /enum-devices /instanceid "ACPI\RKCP0383\0" /drivers`
shows the new driver version, and `vdpu383test info` ends with
`buffers IO addresses, IOMMU paging on`, both IOMMU lines say `paging ON`
with the same `dte`.

Then, in this order:

1. `vdpu383test warmup` and `vdpu383test warmup-direct`: as before (the
   warm-up buffer is mapped at its physical address).
2. The six H.264 streams of section 4: all MATCH.
3. `hevc-conformance.ps1` with all 75 streams, then `-DxvaOnly`: 75 MATCH
   expected (NUT_A_ericsson_5 and RPS_D_ericsson_6 may differ, see the
   list). `vdpu383test info` afterwards: `0 IOMMU faults`, `0 failed
   allocations`, and `pages mapped` back to the SRAM's and warm-up
   buffer's 122.
4. Fallback: `reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters" /v Iommu /t REG_DWORD /d 0 /f`,
   reboot, `vdpu383test info` says `physical, contiguous`, and BA1_Sony_D
   still MATCHes. Delete the value afterwards.

A decode that faults prints `MMU-FAULT` and `IOMMU fault at IO address
0x...`, the next decode runs normally. Send back on failure: `vdpu383test
info`, the failing stream's `NAME.txt`, and the Device Parameters key.

### 7. VP9 conformance (branch `vdpu383-vp9`)

Streams: the libvpx test vectors in FFmpeg's fate-suite
(`https://fate-suite.ffmpeg.org/vp9-test-vectors/`), all 215 profile 0
ones and the profile 2 10-bit 4:2:0 one, remuxed to IVF (`ffmpeg -i
NAME.webm -c copy -f ivf NAME.ivf`, with `-copyinkf` for
vp90-2-16-intra-only), listed in
[vp9-conformance.txt](../../../tools/vdpu383test/vp9-conformance.txt) with
the frame count and the MD5 of FFmpeg 6.1.1's output, each frame at its own
size (`-autoscale 0`), yuv420p or yuv420p10le. Made 2026-11-25. Off the
board, on Linux with `--null`, for every stream: the frame count is FFmpeg's,
the uncompressed header parser agrees with FFmpeg's trace_headers on every
frame (base_q_idx, refresh_frame_flags and both header sizes), every
address register points into the tool's buffers; the global record and the
probability tables are byte for byte what MPP's packers write for the same
frames (checked off-board against MPP's code; `selftest` keeps the MD5s).
CI runs 22 of them. Nothing of it has run on the decoder.

vdpu383test with VP9: CI artifact `rk3576-woa-vdpu383test-arm64` of branch
`vdpu383-vp9`; vdpu383.sys of section 6 (`vdpu383-iommu`, now main) or the
one installed, unchanged by this branch. As administrator, the streams in
one folder:

```
vdpu383test decode D:\vp9\streams\vp90-2-00-quantizer-00.ivf --frame-md5
powershell -ExecutionPolicy Bypass -File vp9-conformance.ps1 -Dir D:\vp9\streams -Only vp90-2-00
powershell -ExecutionPolicy Bypass -File vp9-conformance.ps1 -Dir D:\vp9\streams
powershell -ExecutionPolicy Bypass -File vp9-conformance.ps1 -Dir D:\vp9\streams -DxvaOnly -Out .\vp9-dxva
```

In that order: one key frame and one inter frame (quantizer-00); the
quantizer, sharpness and size groups (vp90-2-00 to -03: intra and inter
prediction, loop filter, odd sizes); then the features: deltaq, 05-resize
(scaled references), 06-bilinear, 09-lf_deltas, 10-show-existing-frame
(hidden frames, superframes), 15-segkey_adpq and segmentation-* (segment
maps between frames), 16-intra-only (frame contexts: see below),
parallelmode (forward-only probabilities), 2pass-akiyo and trac* (alt-ref,
compound prediction), tiling-pedestrian (4 tile columns at 1080p), and
vp92-2-20-10bit-yuv420 (10 bit; the output is unpacked from the decoder's
4-in-5-bytes layout). A third-party mainline V4L2 driver for this decoder
reports compound-prediction frames wrong with registers equal to MPP's
(github.com/SympleNZ/rkvdec-vdpu383-vp9); the 2pass and trac streams show
whether that applies here.

Where MPP and the VP9 specification differ the builder follows the
specification; `-Extra '--vp9-mpp'` repeats a run the way MPP does it
(every intra frame resets the four contexts; the adapted context of a
frame that does not keep it is written over its context). A stream that
MATCHes only one way says which the hardware wants: vp90-2-16-intra-only
is the one expected to tell them apart.

Send back on failure: `vp9-results\vp9-results.txt`, the first failing
stream's `NAME.txt`, then `vdpu383test decode NAME.ivf --frame-md5 --regs -n 4 > win.txt`
and `vdpu383test info`. FFmpeg's per-frame MD5s are in the kit's
`framemd5\NAME.framemd5`.

### Linux reference capture

On the board under Armbian edge 7.3.x, from `woa-tools\` (or
tools/vdpu383-capture in this repository):

```
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-bad gstreamer1.0-plugins-base v4l-utils python3
sudo ./capture-linux.sh                       # BA1_Sony_D and CABA3_SVA_B, fetched from the ITU site
sudo ./capture-linux.sh STREAM.264 -n 8       # or any stream
```

It writes `vdpu383-capture-<host>-<time>.tar.gz`; send that file. It holds,
per stream, the register file after each of the first N pictures, the
hardware-decoded output with its MD5 (which should be the reference MD5
above), an ftrace call graph, and the decoder clocks, power domain, CRU and
PMU state. It only reads registers while the decoder is held awake; if
`/dev/mem` refuses the decoder's range it says so, and a boot with
`iomem=relaxed` on the kernel command line fixes that.

To diff a picture (Linux's `regs-pic-0003.txt` is the third picture; the
Windows tool counts from 0):

```
vdpu383test decode CABA3_SVA_B.264 -n 8 --regs > win.txt          (Windows)
python3 compare-regs.py capture/CABA3_SVA_B/regs-pic-0003.txt win.txt --picture 2
```

`vdpu383test --null decode ...` gives the same register file on any PC,
with made-up buffer addresses.
