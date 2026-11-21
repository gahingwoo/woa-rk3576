# Audio

Nothing here has run on a board yet. A USB audio device works with Windows'
inbox USB Audio Class driver. On branch `audio` of this repository, with
firmware from branches `hdmi-audio-on` or `audio-acpi` of edk2-rk3576, there
are two endpoints to try:

| Endpoint | Boards | Firmware | Windows drivers |
|---|---|---|---|
| HDMI output, 48 kHz stereo | CM5-IO, TaishanPi 3M | `PcdHdmiAudioPreset` | rk3xi2sbus, csaudiork3x, pl330dma |
| Headphone jack and onboard microphone (ES8388) | TaishanPi 3M | `PcdAnalogAudioPreset` | the same, and es8323 |

The CM5-IO's ES8388 goes only to a 3-pin header (CN3) with no jack, and its
wiring is not in any device tree this project has; it is not described.

## The hardware

RK3576 audio is the Rockchip SAI (Serial Audio Interface), not the older
I²S/TDM block that RK3588's drivers and the firmware's `I2s.asl` model.

| Node | Base | GIC SPI | ACPI GSIV | DMA (tx, rx) | Power domain |
|---|---|---|---|---|---|
| sai0 | 0x2A600000 | 187 | 219 | | PD_AUDIO |
| sai1 | 0x2A610000 | 188 | 220 | dmac0 2, 3 | PD_AUDIO |
| sai2 | 0x2A620000 | 189 | 221 | | PD_AUDIO |
| sai3 | 0x2A630000 | 190 | 222 | | PD_AUDIO |
| sai4 | 0x2A640000 | 191 | 223 | | PD_AUDIO |
| sai5 | 0x27D40000 | 192 | 224 | | |
| sai6 | 0x27D50000 | 193 | 225 | dmac2 4, 5 | PD_VO0 |
| sai7 | 0x27ED0000 | 194 | 226 | | |
| sai8 | 0x27EE0000 | 195 | 227 | | |
| sai9 | 0x27EF0000 | 196 | 228 | | |

Windows has no clock, reset, power-domain or pin driver for this SoC, so the
firmware sets each SAI up once and the drivers only program the SAI itself.

## How the drivers use the SAI

`rk3xi2sbus` binds `RKCP3004` and reads the device's `_DSD`
(`rockchip,dma`, `rockchip,tx`, `rockchip,rx`, `rockchip,tplg`,
`rockchip,sai`, `rockchip,mclk-fs`, `rockchip,sai-tx-route`,
`rockchip,sai-rx-route`). With `rockchip,sai` set, `csaudiork3x` runs the
block the way mainline `rockchip_sai.c` does for I²S: bit-clock and frame
master, two 32-bit slots per frame on lane 0, sclk = 64 fs, one 32-bit FIFO
word per sample. Every SAI endpoint is therefore 48 kHz stereo with 24
valid bits in a 32-bit container. sclk and LRCK run from start-up to D3, also
between streams; start and stop gate TX or RX and its DMA.

`rockchip,sai-tx-route` and `rockchip,sai-rx-route` are mainline's
`rockchip,sai-tx-route`/`-rx-route` packed one nibble per lane, lane 0 in bits
15:12. The TaishanPi 3M's `<2 1 0 3>` is `0x2103`: SDO2 carries lane 0.

## HDMI audio

The path, from mainline `rk3576.dtsi` (`hdmi_sound`): SAI6 is the I²S master
and feeds the DW-HDMI-QP inside the SoC. Windows has no driver for the HDMI
controller, so the firmware sets it up at ExitBootServices with
`PcdHdmiAudioPreset`: `mclk_sai6` = 12.288 MHz from `clk_audio_frac_0`, and
the controller's audio path for 48 kHz 2-channel L-PCM (N 6144, CTS the TMDS
rate / 1000, channel status, audio InfoFrame, ACR/AUDI/AUDS packets). Nothing
is written for a DVI sink or a sink whose EDID declares no basic audio. It
then publishes `\_SB.SAI6` with `"rockchip,tplg" = "hdmi"`. The HDMI topology
has no capture endpoint.

## Analog audio on the TaishanPi 3M

The ES8388 is not in the mainline device tree. The wiring is LCKFB's vendor
tree as Armbian's board patch carries it: the codec on I2C3 at 0x11, MCLK
from SAI1's MCLK output, SAI1 M0 pins with lane 0 out on SDO2 and in on SDI0,
LOUT1/ROUT1 into a headphone amplifier (enable GPIO4 PB2, active high),
headphone detect on GPIO4 PB0 (pull-up), onboard microphone on LINPUT1/2,
headset microphone on RINPUT1/2.

Firmware (`PcdAnalogAudioPreset`, `Tspi3mAudio.c`): PD_AUDIO on,
`mclk_sai1` = 12.288 MHz from `clk_audio_frac_1` with the MCLK output to the
pin, the SAI1, I2C3, GPIO4 and DMAC0 clocks open, the pins muxed. The DSDT
gets `\_SB.SAI1` (`"i2s-jack"`) and `\_SB.I2C3.ES83`.

`es8323` binds the codec as `RKCP8388` and then runs a sequence of its own
(`es8388_sai.c`), with values from the Everest ES8388 datasheet (rev 5.0) and
user guide (section 10), not from Linux: codec slave, MCLK = 256 fs, 24-bit
I²S on DAC and ADC, one LRCK for both, LOUT1/ROUT1 at 0 dB, OUT2 off, the
onboard microphone (LINPUT1) on both capture channels with +24 dB PGA. It
drives the amplifier enable from the ACPI GpioIo. Jack state is not reported
to Windows yet: the headphone endpoint always shows as connected.

## What is not done

- Jack detection reaching Windows (KSJACK_DESCRIPTION is static; the
  GpioInt is connected and ignored).
- Hardware volume: Windows' volume is applied in software; the codec sits at
  0 dB.
- The headset microphone (RINPUT), sample rates other than 48 kHz, and
  16-bit formats.
- The CM5-IO's ES8388.

## Trying it

1. Firmware: the TaishanPi 3M image from the edk2-rk3576 CI run of branch
   `audio-acpi` (HDMI and analog), or `hdmi-audio-on` (HDMI only); for the
   CM5-IO, `hdmi-audio-on`.
2. Windows drivers: `woa-audio\` on the media of a WinPE workflow run of
   branch `audio`. Those packages are signed with that run's test certificate,
   which is next to them as `woa-test-signing.cer`. On the board, from an
   administrator prompt, with test signing on:

   ```
   certutil -addstore -f Root woa-test-signing.cer
   certutil -addstore -f TrustedPublisher woa-test-signing.cer
   pnputil /add-driver woa-audio\rk3xi2sbus\rk3xi2sbus.inf /install
   pnputil /add-driver woa-audio\csaudiork3x\csaudiork3x.inf /install
   pnputil /add-driver woa-audio\es8323\es8323.inf /install
   ```

   `pl330dma` is already installed from `woa-drivers\`. The CI workflow's
   artifacts carry unsigned catalogs and do not install.
3. Device Manager should show "Rockchip SAI Bus" once per SAI the DSDT
   publishes, "Rockchip I2S Audio (WDM)" under each, and (TaishanPi 3M,
   analog) "Everest 8388 Audio Codec (RK3576 SAI)".
4. Sound settings: "Rockchip HDMI" (with an HDMI monitor that has
   speakers), and on the TaishanPi 3M also "Rockchip Headphones" and
   "Rockchip Microphone".
