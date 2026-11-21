# vdpu383: RK3576 video decoder, bring-up

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

**Nothing here has run on hardware.** It compiles in CI; the parser and the
register builder have run on Linux with no decoder (`--null`).

## What is where

| Path | What |
|---|---|
| [lib/vdpu383_h264.c](lib/vdpu383_h264.c) | DXVA_PicParams_H264 + DXVA_Qmatrix_H264 + bitstream to registers 0..383 and the auxiliary buffer. From Rockchip MPP (Apache-2.0 OR MIT, used as MIT) |
| [lib/v383_dxva.h](lib/v383_dxva.h) | the DXVA H.264 structures, byte for byte dxva.h's layout (checked against dxva.h at compile time) |
| [driver.c](driver.c), [warmup.c](warmup.c) | the KMDF driver for `ACPI\RKCP0383` |
| [vdpu383_ioctl.h](vdpu383_ioctl.h) | the driver's interface (`\\.\VDPU383`) |
| [../../../tools/vdpu383test](../../../tools/vdpu383test) | the test program and its H.264 parser |
| [../../../tools/vdpu383-capture](../../../tools/vdpu383-capture) | what mainline Linux does with the same clips, and a register diff |

The firmware side is edk2-rk3576 branch `vdpu383-acpi`: a setup option, off
by default, that powers PD_VDEC, sets the decoder clocks and installs an
SSDT with the device.

## Verified, derived, guessed

| Item | Source | Status |
|---|---|---|
| Registers at 0x27B00000 (link +0, function +0x100, cache +0x600, IOMMU +0x800/+0x900), GSIV 340/341, PD_VDEC bits, CRU bits, SRAM 0x3FF88000 480 KiB | mainline rk3576.dtsi, pm-domains.c, clk-rk3576.c, rst-rk3576.c | read from source, not measured |
| Start: link 0x54 = timeout, 0x58 = 0, 0x40 = 1; interrupt: read 0x48/0x4C, write 0x03FF0000 to 0x4C and 0x00030000 to 0x48 | mainline rkvdec-vdpu383-h264.c, rkvdec.c | read from source |
| Warm-up decode at D0 entry | vendor kernel mpp_hack_rk3576.c (GPL-2.0+ OR MIT) | ported unchanged; mainline does not do it, so it may be unnecessary |
| Register values, SPS/PPS/RPS bit layouts, RCB sizes, CABAC table | MPP hal_h264d_vdpu383.c and friends | ported; RCB arithmetic changed from float to integer (same values, checked in `selftest`) |
| SPS/PPS record written both at offset 0 (MPP) and at pps_id * 176 (mainline) | the two disagree | whichever the hardware reads is covered |
| Default reference lists P/B0/B1 from DXVA_PicParams_H264 | H.264 8.2.4.2, as mainline hands them to the hardware | written fresh |
| profile_idc, seq_parameter_set_id, constraint_set3_flag | not in DXVA | the tool passes the real ones; `--dxva-only` passes guesses, to find out whether the hardware cares |
| DXVA_Qmatrix_H264 in zig-zag order | the DXVA spec as FFmpeg's dxva2_h264.c reads it | not checked against a Windows decoder |
| IOMMU left with paging off, so decoder addresses are physical | rockchip-iommu.c semantics | guessed safe; mainline runs with paging on |
| Buffers write-combined (Normal non-cacheable) for a non-coherent device | | guessed |
| Field pictures (PAFF), FMO, 4:2:2, 10 bit, MVC | | not supported; MBAFF frames pass through untested |

## Test plan

Run in this order. Each step says what to send back if it fails.

### 0. Firmware

Flash the `vdpu383-acpi` firmware (CI artifact `cm5io-firmware` or
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

From the WinPE workflow run of branch `vdpu383`, artifact
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
- both IOMMU instances `paging off`
- `warm-up at D0 entry: ran 1 timed_out 0 irq 0x0000000? status 0x....` with
  bit 0 of status (`frame_rdy`) set and bits 1-9 clear, and
  `warm-up verdict: PASS`. Bits 10-15 may be set; they are not errors.

Then `vdpu383test warmup` (again, through the link registers, with
interrupts live) and `vdpu383test warmup-direct` (the same 32x32 picture
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
