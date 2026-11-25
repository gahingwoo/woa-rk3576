# Audio

English | [简体中文](audio.zh-CN.md)

How the audio drivers drive the RK3576 SAI, and what the firmware does for
them.

| Endpoint | Boards | Firmware setting | Windows drivers | State |
|---|---|---|---|---|
| Headphone jack and jack microphone (ES8388) | TaishanPi 3M | `PcdAnalogAudioPreset` (TRUE on this board) | rk3xi2sbus, csaudiork3x 1.0.0.135 or later, es8323, pl330dma | board-tested |
| HDMI output, 48 kHz stereo | TaishanPi 3M | `PcdHdmiAudioPreset` (TRUE) | the same, and wddm-mali-g52 driving HDMI | board-tested |
| HDMI output | CM5-IO | `PcdHdmiAudioPreset` (TRUE) | the same | CI only |
| ES8388: CN3 header (OUT2 on test pads), LIN2/RIN2 differential microphone pads | CM5-IO | edk2-rk3576 branch `cm5io-parity` | the same, es8323 with optional interrupt | branch |
| ES8388: headphone jack, LIN1 headset microphone | ROCK 4D | edk2-rk3576 branch `rock4d-parity` | the same | branch |

These need edk2-rk3576 firmware newer than 0.4.1 (the `ci` build until the
next release). A USB audio device works with Windows' inbox USB Audio Class
driver.

The CM5-IO and the ROCK 4D have no jack-detect line, and their ES8388 is
wired differently from the TaishanPi 3M's; see
[Other boards](#other-boards-no-jack-detect-and-the-routing-properties).

The original plan is in [history](../history/2026-11-21-audio-plan.md).

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
drives the amplifier enable from the ACPI GpioIo.

### Jack detection (CI only)

Firmware (edk2-rk3576 branch `tspi3m-hp-jack`, on top of `rk3576-gpio-pull`,
which puts the pull-up on GPIO4 PB0): the codec's `_CRS` has the detect line
as a GpioInt on both edges and as a GpioIo input, both Shared, pull-up, and a
`_DSD` with `rockchip,hp-det-gpio-io` (which GpioIo is the detect line),
`rockchip,hp-det-active-high` (1 on the TaishanPi 3M:
`hp-det-gpios = <&gpio4 RK_PB0 GPIO_ACTIVE_HIGH>`) and
`rockchip,hp-det-debounce-ms` (150).

`es8323` (`es8388_jack.c`): every edge restarts a 150 ms passive timer; when
it fires the driver reads the level, switches the headphone amplifier to
match, and on a change sends `CSAudioEndpointJackState` for the headphone
endpoint through `\Callback\CsAudioCallbackAPI`. `csaudiork3x` keeps the
state, answers `KSPROPERTY_JACK_DESCRIPTION` with it (and
`JACKDESC2_PRESENCE_DETECT_CAPABILITY` in `KSPROPERTY_JACK_DESCRIPTION2`) on
the SAI headphone topology only, and raises `KSEVENT_PINCAPS_JACKINFOCHANGE`
on its bridge pin. Windows then shows the "Headphones" endpoint as
unplugged (DeviceState 8) or active (1). Until the codec reports, and with
older firmware that has no `_DSD`, the endpoint stays connected as before.

Windows does not move the default device between two adapters on a jack
event, so the switch to the headphones and back to HDMI is done by
rk-monitor ([tools/rk-monitor](../../tools/rk-monitor/README.md)).

Registry, `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP8388\0\Device Parameters`:
`JackDetect` = 0 turns reporting off, `HpDetInvert` = 1 flips the active
level, `JackAmpGate` = 0 leaves the amplifier on when unplugged (all read at
driver start). The driver writes `JackSetupStatus`, `JackOpenStatus`,
`JackReadStatus`, `JackLevel`, `JackConnected`, `JackEdges`, `JackChanges`
and `JackNotifies` there; csaudiork3x writes `JackConnected`, `JackReports`,
`JackEvents` and `JackTopologyRegistered` under the SAI1 adapter's key.

## Other boards: no jack detect, and the routing properties

On a board with no detect line the firmware defines `BOARD_CODEC_NO_HPDET`
(edk2-rk3576 `Es8388.asl`): no detect GpioIo, no GpioInt and no
`rockchip,hp-det-*` properties. es8323 then creates no interrupt object (it
creates it in `OnPrepareHardware`, from the GpioInt in `_CRS`, instead of in
`EvtDriverDeviceAdd`, where an interrupt object with no resource would cost
the device its start), sets up no jack detection, and the headphone endpoint
stays plugged. The SAI headphone topology still claims
`JACKDESC2_PRESENCE_DETECT_CAPABILITY`; `IsConnected` is always TRUE, which
is what Windows asks of an endpoint without detection.

Which ES8388 outputs and input the board wires comes from the same `_DSD`
UUID (`daffd814-6eba-4d8c-8a91-bc9bbf4aa301`), set by the firmware's
`BOARD_CODEC_OUTPUT_ROUTE` and `BOARD_CODEC_INPUT_SELECT`:

| Property | Value | Registers (datasheet section 6, mainline `es8328.c`) |
|---|---|---|
| `rockchip,output-route` | bit 0 LOUT1/ROUT1, bit 1 LOUT2/ROUT2; default 1 | reg 0x04 DAC Power: bit 5 LOUT1, 4 ROUT1, 3 LOUT2, 2 ROUT2 (0x30, 0x0C, 0x3C); with OUT2, reg 0x30/0x31 (LOUT2/ROUT2 volume) = 0x1E, 0 dB (reset value 0x00, -45 dB) |
| `rockchip,input-select` | 1 LIN1/RIN1 (default), 2 LIN2/RIN2, 3 LIN1-RIN1 differential, 4 LIN2-RIN2 differential | reg 0x0A ADC Control 2: LINSEL 7:6, RINSEL 5:4 (00 input 1, 01 input 2, 11 differential): 0x00, 0x50, 0xF0, 0xF0; for 3 and 4 also reg 0x0B ADC Control 3 bit 7 DS (pair select): 0x02, 0x82 |

Without either property the writes are exactly the TaishanPi 3M sequence.
REG_DWORD `OutputRoute` and `InputSelect` in the codec's `Device Parameters`
override the firmware (for trying a wiring without reflashing); the driver
writes what it used as `ActiveOutputRoute`, `ActiveInputSelect`, and
`JackInterrupt` (1 if `_CRS` had a GpioInt).

| Board | Wiring (schematic) | `output-route` | `input-select` |
|---|---|---|---|
| TaishanPi 3M | LOUT1/ROUT1 to the amplifier and jack; LINPUT1 onboard microphone | (absent: 1) | (absent: 1) |
| CM5-IO | LOUT1/ROUT1 to the 3-pin header CN3 (HPR, HPL, HP_GND); LOUT2/ROUT2 (AUD_LINEOUT_L/R) only to test pads T36/T37, not to the 40-pin header; microphone pads T34/T35 (MIC2P/MIC2N, biased through R56/R57/R58) on LIN2/RIN2, marked "Differential routing". HP_DET_L is tied low by R411, so no detect | 3 | 4 |
| ROCK 4D | LOUT1/ROUT1 to the jack J18; LOUT2/ROUT2 only to the speaker amplifier, not fitted; headset microphone MIC_INP_PHONE on LIN1, RIN1 to HP_GND, LIN2/RIN2 open | 1 | 1 |

## ROCK 4D: the jack on an ADC input

The ROCK 4D has no detect switch on J18. Its microphone contact is biased
from VCCA3V3_CODEC through 100R + 2.2K and halved by 100K/100K into
`SARADC_VIN3_HP_HOOK` (schematic V1.12 page 16): about 1.63 V with nothing
plugged, near 0 V with a 3-pole plug (the contact lands on the sleeve), and
about 0.5 to 1.2 V with a headset. So the jack is read by the SARADC driver
([rk3xsaradc](../../drivers/adc/rk3xsaradc/README.md)), with hysteresis
(plugged below 1.45 V, unplugged above 1.52 V) and 150 ms of debounce, and
es8323 follows it when its `_DSD` has

| Property | Value |
|---|---|
| `rockchip,hp-det-saradc` | 1: the jack state comes from rk3xsaradc, not from a GPIO |

(firmware: `BOARD_CODEC_HPDET_SARADC` next to `BOARD_CODEC_NO_HPDET`). es8323
registers on `\Callback\Rk3xSaradcJack`, asks for the current state, and
from then on handles every change the way it handles a GPIO edge: the amplifier
gate and `CSAudioEndpointJackState` to csaudiork3x. Until the SARADC driver
answers, or when it is not installed, the endpoint stays plugged. It writes
`JackSaradc` (1 when following rk3xsaradc) and `JackSaradcStates` next to the
other `Jack*` values. `JackDetect` = 0 turns this off too. CI only.

## What is not done

- Hardware volume: Windows' volume is applied in software; the codec sits at
  0 dB.
- The headset microphone (RINPUT), sample rates other than 48 kHz, and
  16-bit formats.
- The CM5-IO's and the ROCK 4D's ES8388: described and configured, not run
  on either board.

## Installing

1. Firmware: edk2-rk3576 newer than 0.4.1 (see above).
2. The signed packages are on the WinPE media in `woa-audio\`, next to the
   run's test certificate. They are not injected by the deploy script.
   Install them as in [drivers](../drivers.md): trust the certificate, then
   `pnputil /add-driver ... /install` for rk3xi2sbus, csaudiork3x and es8323.
   `pl330dma` is already installed from `woa-drivers\`. The CI workflow's
   artifacts carry unsigned catalogs and do not install.
3. Device Manager shows "Rockchip SAI Bus" once per SAI the DSDT publishes,
   "Rockchip I2S Audio (WDM)" under each, and on the TaishanPi 3M "Everest
   8388 Audio Codec (RK3576 SAI)".
4. Sound settings show "Rockchip HDMI" (with an HDMI monitor that has
   speakers) and, on the TaishanPi 3M, "Rockchip Headphones" and "Rockchip
   Microphone".
