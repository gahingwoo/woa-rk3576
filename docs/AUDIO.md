# Audio

There is no on-board audio under Windows yet. A USB audio device works with
Windows' inbox USB Audio Class driver. HDMI audio is written but has not run
on hardware; see [HDMI audio](#hdmi-audio-not-run-on-hardware) below.

## The hardware

RK3576 audio is the Rockchip SAI (Serial Audio Interface), not the older
I²S/TDM block that RK3588's drivers and the firmware's `I2s.asl` model.

| Node | Base | GIC SPI | ACPI GSIV |
|---|---|---|---|
| sai0 | 0x2A600000 | 187 | 219 |
| sai1 | 0x2A610000 | 188 | 220 |
| sai2 | 0x2A620000 | 189 | 221 |
| sai3 | 0x2A630000 | 190 | 222 |
| sai4 | 0x2A640000 | 191 | 223 |
| sai5 | 0x27D40000 | 192 | 224 |
| sai6 | 0x27D50000 | 193 | 225 |
| sai7 | 0x27ED0000 | 194 | 226 |
| sai8 | 0x27EE0000 | 195 | 227 |
| sai9 | 0x27EF0000 | 196 | 228 |

Each SAI needs `MCLK_SAIx` and `HCLK_SAIx` from the CRU, an audio PLL, a power
domain, resets and a DMA channel on `dmac2` (PL330). The codec is an Everest
ES8388 (the kernel's `es8328` driver) on I²C, with a GPIO jack-detect
interrupt; the firmware has an `Es8388.asl` for it.

## What is missing in the firmware

- `I2s.asl` still has RK3588 addresses and CRU offsets, says so in its header,
  and is included in no RK3576 DSDT.
- It models I²S, not SAI, and its `_HID` is `RKCP3003`, which is already this
  project's SPI. An SAI device needs its own ID.
- Nothing sets up the audio PLL, the SAI clocks, the `dmac2` channel, the power
  domain or the resets.

## What a driver would need after that

A PortCls/WaveRT miniport that moves samples between the WaveRT buffer and the
SAI through `dmac2`, and an ES8388 init over I²C ported from `es8328.c`. It
would use the [I²C](../drivers/i2c/rk3xi2c) and [GPIO](../drivers/gpio/rk3xgpio)
drivers already here. The audio drivers imported from worproject target
RK3588's I²S-TDM and are not built into the WinPE image.

## HDMI audio (not run on hardware)

Branch `sai-hdmi-audio` here and branch `acpi-hdmi-audio` of edk2-rk3576
add one fixed-format HDMI render endpoint: 48 kHz, stereo, 24 valid bits in
32-bit samples. Nothing of it has run on a board.

The path, from mainline `rk3576.dtsi` (`hdmi_sound`): SAI6 (0x27D50000,
GSIV 225, TX on `dmac2` request 4) is the I²S master and feeds the
DW-HDMI-QP inside the SoC. Windows has no driver for the HDMI controller, so
the firmware sets it up once, at ExitBootServices, with
`PcdHdmiAudioPreset`: `mclk_sai6` = 12.288 MHz from `clk_audio_frac_0`, and
the controller's audio path for 48 kHz 2-channel L-PCM (N 6144, CTS the TMDS
rate / 1000, channel status, audio InfoFrame, ACR/AUDI/AUDS packets). It
then publishes `\_SB.SAI6`, `RKCP3004`, with `"rockchip,tplg" = "hdmi"`
and `"rockchip,sai" = 1`.

On the driver side, `rk3xi2sbus` binds `RKCP3004` (not `RKCP3003`, which is
the SPI here) and `csaudiork3x` programs the SAI when `"rockchip,sai"` is
set: bit-clock master, two 32-bit slots, sclk = 64 fs, kept running between
streams. The HDMI topology installs no capture endpoint.

Neither audio driver is in the WinPE image or added by the deploy script.
To try it: flash a firmware image with the preset on (the branch's TaishanPi
3M build has it on), install `rk3xi2sbus`, `csaudiork3x` and `pl330dma`
from the CI artifact with test signing on, and look for "Rockchip SAI Bus"
and a digital output in Sound settings.
