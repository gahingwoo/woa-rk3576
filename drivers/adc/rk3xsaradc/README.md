# rk3xsaradc: RK3576 SARADC

KMDF driver for the RK3576 successive-approximation ADC (8 channels, 12 bits,
1.8 V reference) at `0x2AE00000`. It binds `ACPI\RKCP0ADC`, which
edk2-rk3576 declares as `\_SB.ADC0` (`Silicon/Rockchip/RK3576/AcpiTables/Saradc.asl`)
on boards whose DSDT says what is wired to the ADC.

Status: CI only (builds, host unit test); not run on a board yet. See the
board test plan below.

What it does with the ADC, all from the firmware's `_DSD`:

| Use | How it reaches Windows |
|---|---|
| adc-keys (a resistor-ladder button group on one channel) | HID buttons through `vhf.sys`: volume, mute and media keys as Consumer Control usages, power, sleep and wake as System Control, anything else (Recovery, Maskrom) as buttons 1 to 8 on vendor page `0xFF00` |
| a headphone jack read on an ADC input | the callback object `\Callback\Rk3xSaradcJack`, which `es8323.sys` listens to (see below) |
| raw conversions | `\\.\RK3XSARADC`, used by [`tools/adctest`](../../../tools/adctest) |

With no `\_SB.ADC0` in the firmware nothing binds this INF. With the device but
no keys and no jack in `_DSD`, the driver starts, converts only when `adctest`
asks, and enumerates no HID device.

## The converter

Single conversions, one channel at a time, in the "v2" register layout of
Linux `drivers/iio/adc/rockchip_saradc.c` (RK3576 shares it with RK3588) and
RK3576 TRM V1.2 Part 1 chapter 18. End of conversion is polled in
`SARADC_END_INT_ST` (about 60 us at 1 MHz), not taken as an interrupt; the
status bit is cleared after every conversion. Ten timeouts in a row stop the
polling (`DiagStopped` 1) instead of stalling a worker thread every poll.

The driver never writes the CRU. Linux and U-Boot reset the controller through
`SRST_P_SARADC` (CRU `SOFTRST_CON13` bit 6) before every conversion, because
reading another channel can leave channel 1 wrong. Here the firmware does that
reset: `\_SB.ADC0.RSTC` asserts and releases that one bit, and the driver
evaluates it at start and whenever a conversion is on a different channel from
the previous one. The firmware also sets the converter clock to 1 MHz
(24 MHz / 24, the rate Linux sets) and leaves both clock gates open (their
reset state).

## `_DSD` (UUID `daffd814-6eba-4d8c-8a91-bc9bbf4aa301`)

| Property | Meaning |
|---|---|
| `rockchip,vref-microvolt` | the reference, 1800000 on all three boards |
| `rockchip,poll-interval-ms` | sampling period, 100 |
| `rockchip,adc-keys-channel` | `io-channels = <&saradc N>` of the board's adc-keys node |
| `rockchip,adc-keys-keyup-microvolt` | `keyup-threshold-microvolt` |
| `rockchip,adc-keys-press-microvolt` | Package, `press-threshold-microvolt` of each button |
| `rockchip,adc-keys-code` | Package, `linux,code` of each button |
| `rockchip,adc-keys-hid-usage` | optional Package, HID usage (`page << 16 \| id`) per button; 0 means "from the code" |
| `rockchip,jack-channel` | the channel the jack is read on |
| `rockchip,jack-plugged-below-microvolt` | plugged below this |
| `rockchip,jack-unplugged-above-microvolt` | unplugged above this (between the two: no change) |
| `rockchip,jack-debounce-ms` | how long a new jack state must hold, 150 |

Keys follow Linux `adc-keys.c`: the button whose press voltage is closest to
the sample wins, unless the key-up voltage is strictly closer. A key state must
be seen in two samples in a row before it is reported.

Keys are armed only after they have been seen released: three key-up samples
in a row (300 ms at the default poll) after the driver starts and after every
resume. Before that no press is reported, however long a key is held. A key
held through boot, or a converter that reads 0 V (where these buttons sit when
pressed), therefore never sends a press. This matters for a key the firmware
maps to the power button: on the TaishanPi 3M the Recovery key carries
`rockchip,adc-keys-hid-usage` = `0x00010081` (Generic Desktop, System Power
Down, the same usage `rk806pwrkey` reports), and Windows applies the "When I
press the power button" action from Power Options to it. `DiagKeysArmed` is 1
once the keys are armed.

## Registry

`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP0ADC\0\Device Parameters`, read at
start (disable and re-enable the device, or reboot):

| Value | Effect |
|---|---|
| `Keys` = 0 | no key reports |
| `Jack` = 0 | no jack reports |
| `PollIntervalMs` | 20 to 2000 |
| `KeySamples` | samples in a row a key state must be seen (default 2) |
| `JackPluggedBelowMicrovolts`, `JackUnpluggedAboveMicrovolts`, `JackDebounceMs` | override the firmware's jack thresholds |

Written by the driver: `DiagDsdStatus`, `DiagVrefMicrovolts`, `DiagPollMs`,
`DiagKeysChannel`, `DiagKeyCount`, `DiagHidStarted`, `DiagJackChannel`,
`DiagJackConnected` (0xFFFFFFFF before the first decision),
`DiagJackMicrovolts`, `DiagConversions`, `DiagTimeouts`, `DiagStopped`,
`DiagNotifications`, `DiagHidReports`, `DiagKeysArmed`, `DiagResetAvailable`,
`DiagResets`.

## The jack interface for es8323

`rk3xsaradc_ioctl.h` has the protocol. In short: both drivers open
`\Callback\Rk3xSaradcJack` with `ExCreateCallback(..., TRUE, TRUE)`, so either
may start first. A consumer registers a callback, then notifies
`RK3X_SARADC_JACK_QUERY`; the SARADC driver answers from a work item with
`RK3X_SARADC_JACK_STATE` and a `RK3X_SARADC_JACK_INFO`, and sends the same on
every debounced change. Notifications arrive at PASSIVE_LEVEL. `es8323.sys`
uses it when its own `_DSD` has `rockchip,hp-det-saradc` = 1 (the ROCK 4D).

## Building and the host test

    msbuild drivers\adc\rk3xsaradc\rk3xsaradc.vcxproj /p:Configuration=Release /p:Platform=ARM64

The threshold, debounce and HID-mapping code (`saradc_logic.c`) has no WDK
dependency; CI runs its unit test on Linux:

    gcc -std=c11 -Wall -Wextra -Werror -o t test/saradc_logic_test.c saradc_logic.c && ./t

## Board test plan

Per board, with firmware that declares
`\_SB.ADC0` and this driver installed (`pnputil /add-driver rk3xsaradc.inf /install`):

1. Device Manager: "Rockchip RK3576 SARADC" started, no code 10. `reg query`
   the Device Parameters key: `DiagDsdStatus` 0, `DiagResetAvailable` 1,
   `DiagTimeouts` 0.
2. `adctest info`, then `adctest scan` twice. Expected idle values below;
   the second scan must match the first (channel 1 in particular, which is
   what the reset is for).
3. Keys: `adctest read <ch> 50 100` while pressing the button, then check the
   HID report with a HID viewer (vendor page 0xFF00 button 1) or, for a
   consumer key, that Windows reacts.
4. Jack (ROCK 4D): `adctest jack --watch` while plugging a 3-pole plug, a
   4-pole headset, pressing the headset button, and unplugging. Then the
   Sound settings: the Headphones endpoint must follow.

| Board | Channel | Wired to | Idle (computed) | Active (computed) |
|---|---|---|---|---|
| TaishanPi 3M | 1 | Recovery key (mainline dts adc-keys) | 1.8 V, raw ~4095 | ~18 mV |
| CM5-IO | 0 | Maskrom key SW3 against the CM5's boot strap (100K up, 11.3K down) | 183 mV, raw ~416 | ~2 mV |
| ROCK 4D | 0 | Maskrom key against R6 10K up, R45 6.8K down | 729 mV, raw ~1658 | ~0 |
| ROCK 4D | 3 | J18 microphone contact (`SARADC_VIN3_HP_HOOK`, 100K/100K from a 2.2K bias on VCCA3V3_CODEC) | ~1.63 V, raw ~3710 | 3-pole plug ~0, headset ~0.5 to 1.2 V |

The ROCK 4D jack thresholds (plugged below 1.45 V, unplugged above 1.52 V) are
the ones most likely to need moving after a measurement.
