# vdpu383: RK3576 video decoder, bring-up

English | [简体中文](README.zh-CN.md)

A KMDF driver for the RK3576 H.264/HEVC/VP9/AV1 decoder (RKVDEC, VDPU383 in
Rockchip's naming), plus a test program that parses streams, builds the
decoder's register file and checks the output against reference MD5s.
Applications do not see this driver directly. The decoder Edge will use is
planned as a VIDEO_DECODE node inside the display driver
([wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52)); the register
builder in [lib](lib) is plain C (no KMDF, WDM or C library calls, no
allocation, no floating point) so it can move there unchanged.

The device `ACPI\RKCP0383` exists only when *Video Decoder (VDPU383)* is
enabled in the firmware setup (on by default since edk2-rk3576 0.5.0; see
[features](../../../docs/features.md#video-decoder)).

| Codec | Status |
|---|---|
| H.264 | board-tested (TaishanPi 3M): six conformance streams bit exact, 0 hardware errors |
| HEVC Main | board-tested: 60/75 MATCH with contiguous buffers (`Iommu` 0), 14 large streams failed to allocate there; not yet re-run on the IOMMU path |
| VP9 profile 0/2 | board-tested: 215/216; the failure (vp90-2-trac3849) is fixed in the builder, not re-run |
| AV1 Main | CI only (parser and builder on Linux with `--null`) |
| IOMMU buffers (`Iommu` 1, default) | board-tested with H.264 and VP9 |

History: `git log -- drivers/video/vdpu383 tools/vdpu383test`.

## What is where

| Path | What |
|---|---|
| [lib/vdpu383_h264.c](lib/vdpu383_h264.c), [lib/v383_dxva.h](lib/v383_dxva.h) | H.264: DXVA_PicParams_H264 + DXVA_Qmatrix_H264 + bitstream to registers 0..383 and the auxiliary buffer (from Rockchip MPP, used as MIT); DXVA structures checked against dxva.h at compile time |
| [lib/vdpu383_hevc.c](lib/vdpu383_hevc.c), [lib/v383_dxva_hevc.h](lib/v383_dxva_hevc.h) | HEVC Main, from MPP, checked against mainline rkvdec-vdpu383-hevc.c; rebuilds the SPS reference picture sets, which DXVA does not carry |
| [lib/vdpu383_vp9.c](lib/vdpu383_vp9.c), [lib/vdpu383_vp9_probs.c](lib/vdpu383_vp9_probs.c), [lib/v383_dxva_vp9.h](lib/v383_dxva_vp9.h) | VP9 profile 0 and 2: registers from MPP hal_vp9d_vdpu383.c (MIT), packers written fresh to MPP's layout (byte equal output), default probabilities from mainline v4l2-vp9.c (GPL-2.0). Keeps frame contexts, segment maps and motion vectors between frames (V383_Vp9State) |
| [lib/vdpu383_av1.c](lib/vdpu383_av1.c), [lib/vdpu383_av1_cdf.c](lib/vdpu383_av1_cdf.c), [lib/v383_dxva_av1.h](lib/v383_dxva_av1.h) | AV1 Main 8/10 bit: DXVA_PicParams_AV1 to registers and the 672-byte global record, from MPP (MIT); keeps reference slot state per surface (V383_Av1State) and can wrap DXVA tile data in OBUs |
| [driver.c](driver.c), [warmup.c](warmup.c), [vdpu383_ioctl.h](vdpu383_ioctl.h), [vdpu383_check.h](vdpu383_check.h) | the KMDF driver, its interface `\\.\VDPU383`, and what a session may do (shared with wddm-mali-g52) |
| [../../../tools/vdpu383test](../../../tools/vdpu383test) | test program, parsers, conformance lists and scripts |
| [../../../tools/vdpu383-capture](../../../tools/vdpu383-capture) | Linux reference capture and register diff |

## Hardware facts and their sources

| Item | Source | Status |
|---|---|---|
| Registers at 0x27B00000 (link +0, function +0x100, cache +0x600, IOMMU +0x800/+0x900), GSIV 340/341, SRAM 0x3FF88000 480 KiB | mainline rk3576.dtsi, pm-domains.c, clk-rk3576.c, rst-rk3576.c | from source |
| Start: link 0x54 = timeout, 0x58 = 0, 0x40 = 1; interrupt: read 0x48/0x4C, write 0x03FF0000 to 0x4C and 0x00030000 to 0x48 | mainline rkvdec | from source |
| Read cache: CACHE0_CFG = 0x13, MAX_READS = 0x1C at D0 entry and after every IP reset; CLR_CACHE0 = 1 before each decode | mainline | ported (mainline measured 3.6 ms per frame vs 22.5 to 65.4 ms with power-on values) |
| Timed-out decode: IP reset (link 0x48 irq off, 0x44 = 1, wait status bit 11, clear), then drain DPCs | mainline | ported; an error status resets only with `VDPU383_RUN_RESET_ON_ERROR` |
| Warm-up decode at D0 entry and after IP reset | vendor kernel mpp_hack_rk3576.c | required: without it H.264 decodes wrong after power-on |
| SPS/PPS record written at offset 0 (MPP) and at pps_id * 176 (mainline) | the two disagree | both covered |
| profile_idc, seq_parameter_set_id, constraint_set3_flag | not in DXVA | `--dxva-only` passes guesses to test whether the hardware cares |
| IOMMU v2 page tables (4 KiB pages, IO addresses from 16 MiB, SRAM and warm-up buffer identity mapped), restored after every reset | mainline rockchip-iommu.c, rkvdec.c | ported |
| Buffers write-combined | | guessed |
| PAFF, FMO, 4:2:2, H.264 10 bit, MVC | | not supported; MBAFF untested |

## Settings and breadcrumbs

DWORDs under
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0\Device Parameters`, read
when the device starts (restart the device or reboot after a change):

| Setting | Default | Meaning |
|---|---|---|
| `WarmupAtD0` | 1 | run the warm-up decode at every D0 entry and IP reset. 0: never (H.264 is then wrong until `vdpu383test warmup` runs) |
| `Iommu` | 1 | map buffers through the decoder IOMMU. 0: physically contiguous buffers below 4 GiB, paging off (large surfaces fail with error 1450) |
| `UserAccess` | 0 | Who may open `\\.\VDPU383` from user mode. 0: SYSTEM and administrators, one handle. 1: every user, many handles. 2: low-integrity processes too. With 1 or 2, `VDPU383_RUN_NO_ADDR_CHECK` is ignored and each handle may hold at most 384 MiB. Players need none of it: they decode through the display driver (sessions, below) |

`UserAccess` 1 and 2 are for test boards only, as a back-out for the
display driver's path. The interface lets the caller write decoder
registers; the driver checks that every address register points into a
buffer of that handle, but not that the decoder stays inside it. User-mode
handles share one IOMMU directory, so one process can reach another's
buffers; with `Iommu` 0 the decoder can reach any memory.

The driver also writes breadcrumbs to the same key, flushed at once, so they
survive a hang or bugcheck (boot with the firmware option off if the decoder
stops Windows):

| Value | Meaning |
|---|---|
| `LastStep`, `PrevStep`, `StepSeq` | current step, previous step, counter. `D0Entry: first register read` after a bugcheck = the firmware did not power the decoder |
| `RegsPa`, `SramPa` | assigned resources (expect 0x27B00000, 0x3FF88000) |
| `Version`, `Mmu0Status`, `Mmu1Status`, `LinkIrqAtD0`, `LinkStatusAtD0` | first reads at D0 entry |
| `CacheCfgPowerOn`, `CacheMaxReadsPowerOn` | read cache before the driver set it |
| `WarmupIrq`, `WarmupStatus`, `WarmupTimedOut` | last warm-up |
| `FirstRunResult`, `FirstRunStatus` | first decode of the session (0 = ok) |
| `IommuSetting`, `IommuOn`, `MmuDte0`, `MmuEnableStatus0/1` | `Iommu` value read, whether paging came on, directory register, status after enable (bit 0 = paging) |
| `LastMmuFaultAddr` | IO address of the last IOMMU fault |
| `LastImportStage`, `LastImportStatus`, `LastImportPages`, `LastImportExporterStatus`, `LastImportExporterPages`, `LastImportHandle`, `LastImportPid`, `ImportFailures`, `Imports` | last failed `IOCTL_VDPU383_IMPORT` (first 16 failures, then every 64th). Stage: 1 parameters or IOMMU off, 2 no memory, 3 refresh of an unknown import, 4 no free IO addresses, 5 no exporter (maliwddm did not start it), 6 exporter refused (its status; maliwddm's `VdecLast*` says why), 7 page count differs (asked vs exporter), 8 IOMMU entries |

## Sessions (interface 4)

A session is a file object opened from kernel mode: wddm-mali-g52's
maliwddm opens one for each D3D device whose process decodes, and makes
that process's calls in it (wddm-mali-g52 docs/CHROMIUM-VIDEO.md, C2). A
session ([vdpu383_check.h](vdpu383_check.h)):

- may get information, allocate, free, import and run; nothing else (no
  register access, warm-up or reset);
- has every address register of a decode checked against its own buffers
  and imports, never the SRAM, whatever its flags; no register read-back;
  waits at most 2 s; imports only tokens given to its own process;
- decodes with an IOMMU directory of its own that holds only its buffers,
  so the decoder cannot reach another session's or user handle's memory
  even past a buffer's end (the directory is switched before its decode);
- needs `Iommu` 1; is told no physical addresses or register contents.

At most 64 sessions. `vdpu383test info` prints a `sessions` line; any user
gets the same counters with wddm-mali-g52's `vdectest kmd`.
`vdpu383test --null sessiontest` runs these rules on the null device (CI).

## Zero copy (interface 3)

`IOCTL_VDPU383_IMPORT` maps the pages of a maliwddm allocation (a D3D11
decoder output texture) at decoder IO addresses, so the decoder writes the
picture into the texture and the UMD copies nothing (wddm-mali-g52
docs/VIDEO-DECODE.md, section 8). The caller passes a token from maliwddm's
escape; vdpu383.sys gets the pages from maliwddm kernel to kernel
(`\Callback\MaliwddmVdecExport`, `vdpu383_import.h`), which checks the token
and the process. IOMMU on only. When the pages go away (eviction,
destruction), maliwddm revokes the import first: a running decode finishes,
the mapping is cleared, and a later decode naming it returns
`VDPU383_RUN_REVOKED` before starting; the UMD refreshes the import at the
same IO addresses and runs again. `vdpu383test info` prints import counts.
Status: CI only.

## Install

Firmware: enable *ACPI / Device Tree* > *Video Decoder (VDPU383, experimental)*
(config table mode must include ACPI). The serial log then shows
`RK3576Dxe: VDEC powered, version=0x...`, and `acpiview -s SSDT` in the UEFI
shell lists OEM table ID `RKVDEC`.

Driver: from the WinPE workflow artifact `woa-drivers-signed-arm64`
(`woa-drivers\vdpu383\`, `woa-drivers\signer.cer`, `woa-tools\vdpu383test.exe`).
The test certificate changes with every run, so import the one from the same
run. As administrator, after `w32tm /resync /force`:

```
certutil -addstore -f Root signer.cer
certutil -addstore -f TrustedPublisher signer.cer
pnputil /add-driver vdpu383\vdpu383.inf
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0383\0" /v ConfigFlags /t REG_DWORD /d 0x20 /f
shutdown /r /t 0
```

Staged install (mark for reinstall, reboot) is safer than `/install` on a
running device. Device Manager > System devices then shows "Rockchip RK3576
Video Decoder (VDPU383, bring-up)" without a problem code.

On failure send: `pnputil /enum-devices /instanceid "ACPI\RKCP0383\0" /drivers`
and the `RKCP0383` lines of `C:\Windows\INF\setupapi.dev.log`.

## Test

Run in order; the first failure names the feature.

| Step | Command | Pass |
|---|---|---|
| Power | `vdpu383test info` | non-zero `version register`; both IOMMU instances `paging ON` with the same `dte`; `warm-up verdict: PASS`; `buffers IO addresses, IOMMU paging on` |
| Warm-up | `vdpu383test warmup`, then `vdpu383test warmup-direct` three times | status bit 0 set, bits 1-9 clear (10-15 are not errors), `verdict: PASS`; same MD5 each time |
| Replay | `vdpu383test decode BA1_Sony_D.jsv -n 3 --dump C:\v383 --readback`, then `vdpu383test replay C:\v383\pic_0000\replay.txt` | `replay: ok ... MATCH` (mismatch with good status = non-deterministic decode; different status = state outside the register file) |
| H.264 | `vdpu383test decode STREAM --ref REFERENCE.yuv -o out.yuv`, then with `--dxva-only` | `reference: N frames; MATCH`, MD5 as in the table below |
| HEVC | `powershell -ExecutionPolicy Bypass -File hevc-conformance.ps1 -Dir D:\hevc\streams` (`-Only RPS_A` first, then `-DxvaOnly -Out .\hevc-dxva`) | `MATCH` on every line |
| VP9 | `vp9-conformance.ps1 -Dir D:\vp9\streams` (`-Only vp90-2-00` first, then `-DxvaOnly -Out .\vp9-dxva`) | `MATCH` on every line |
| AV1 | `av1-conformance.ps1 -Dir D:\av1\streams`, then `-DxvaOnly -Out .\av1-dxva` and `-DxvaOnly -Synth 1 -Out .\av1-dxva-frame` | `MATCH`; `GRAIN-OFF` = frames equal dav1d's without film grain |
| Fallback | set `Iommu` 0, reboot | `vdpu383test info` says `physical, contiguous`, BA1_Sony_D still matches; delete the value afterwards |

After a full run `vdpu383test info` should show `0 IOMMU faults`,
`0 failed allocations` and `pages mapped` back to 122. A faulting decode
prints `MMU-FAULT` and `IOMMU fault at IO address 0x...`; the next decode
runs normally.

### Streams

- H.264: ITU-T H.264.1 suite, https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/ (AVCv1/, FRExt/); each zip has the stream and reference .yuv. Start with BA1_Sony_D, CABA1_SVA_B, CABA3_SVA_B, MR2_TANDBERG_E, FRExt1_Panasonic_D, HPCAQ2LQ_BRCM_B.
- HEVC: `https://fate-suite.ffmpeg.org/hevc-conformance/NAME.bit`, the 75 streams in [hevc-conformance.txt](../../../tools/vdpu383test/hevc-conformance.txt) with FFmpeg 6.1.1 MD5s. AMP_A/B need about 170 MiB of buffers. NUT_A_ericsson_5 and RPS_D_ericsson_6 may differ (see the list).
- VP9: `https://fate-suite.ffmpeg.org/vp9-test-vectors/`, remuxed to IVF (`ffmpeg -i NAME.webm -c copy -f ivf NAME.ivf`, add `-copyinkf` for vp90-2-16-intra-only), listed in [vp9-conformance.txt](../../../tools/vdpu383test/vp9-conformance.txt). References are made with `-autoscale 0 -fps_mode passthrough`; without it FFmpeg repeats frames and correct streams read MISMATCH. `-Extra '--vp9-mpp'` repeats a run the way MPP handles frame contexts, where MPP and the spec differ.
- AV1: 17 IVF files from fate-suite `av1-test-vectors/` and `av1/` plus 7 made with libaom and SVT-AV1, listed in [av1-conformance.txt](../../../tools/vdpu383test/av1-conformance.txt) with dav1d MD5s. Order: allintra, cdfupdate, mv, mfmv, then the rest.

Per-frame reference MD5s for `--frame-md5` are in the kit's `framemd5\NAME.framemd5`.

| H.264 stream (zip) | File | Covers | Frames | Reference MD5 |
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
| CAPM3_Sony_D | CAPM3_Sony_D.jsv | CABAC, mixed slice types | 300 | 9ecc3bf5ff7cac9a068a5ba57bc87cb7 |
| CAWP1_TOSHIBA_E | CAWP1_TOSHIBA_E.264 | weighted prediction, CIF | 90 | 87946607d1d774c2da8ec8632710c84a |
| CVWP2_TOSHIBA_E | CVWP2_TOSHIBA_E.264 | weighted prediction, CAVLC | 90 | 7616ccc1e4cc89805cf5ae11e4437533 |
| FRExt1_Panasonic_D | FRExt1_Panasonic.avc | High, 8x8 transform | 8 | f7023a3a93cc98c0c8d83c1deaf11048 |
| FRExt3_Panasonic_E | FRExt3_Panasonic.avc | High | 11 | 231e4f3105f75a3602161ec484bbd8e3 |
| HPCV_BRCM_A | HPCV_BRCM_A.264 | High, CAVLC, CIF | 300 | 5eafc104844211893e1d8af0b8b66ed6 |
| HPCAQ2LQ_BRCM_B | HPCAQ2LQ_BRCM_B.264 | High, CABAC, scaling matrices | 100 | cf81785c89930d5b1c191a1ef4cb9e83 |

### What to send back on failure

For the first failing stream: the results file (`*-results\*-results.txt`)
and its `NAME.txt`, `vdpu383test decode NAME --frame-md5 --readback --regs -n 4 > win.txt`,
`vdpu383test decode NAME -n 4 --dump C:\v383` with `C:\v383` zipped,
`vdpu383test info`, the Device Parameters key, and for H.264 the Linux
capture below with `compare-regs.py` output.

### Linux reference capture

On the board under Armbian edge 7.3.x, from `woa-tools\` or
[tools/vdpu383-capture](../../../tools/vdpu383-capture):

```
sudo apt install gstreamer1.0-tools gstreamer1.0-plugins-bad gstreamer1.0-plugins-base v4l-utils python3
sudo ./capture-linux.sh                       # BA1_Sony_D and CABA3_SVA_B, fetched from the ITU site
sudo ./capture-linux.sh STREAM.264 -n 8       # or any stream
```

It writes `vdpu383-capture-<host>-<time>.tar.gz` with, per stream, the
register file after each of the first N pictures, the decoded output and its
MD5, an ftrace call graph, and clock, power domain, CRU and PMU state. If
`/dev/mem` refuses the decoder range, boot with `iomem=relaxed`.

Diff one picture (Linux counts from 1, the Windows tool from 0):

```
vdpu383test decode CABA3_SVA_B.264 -n 8 --regs > win.txt          (Windows)
python3 compare-regs.py capture/CABA3_SVA_B/regs-pic-0003.txt win.txt --picture 2
```

`vdpu383test --null decode ...` gives the same register file on any PC, with
made-up buffer addresses.
