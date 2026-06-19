# Audio bring-up — RK3576 (SAI + ES8388)

Short version: **a Windows audio (WaveRT) driver cannot be written usefully yet**
— the RK3576 audio block is not enumerated or clocked in the firmware/ACPI, so a
driver would have nothing correct to bind to. This is the same class of blocker
as the display path. This doc records the verified hardware facts and the
firmware-side prerequisites, and notes a working stopgap.

## What the hardware actually is

RK3576 audio is the Rockchip **SAI** (Serial Audio Interface) block — **not** the
older I²S/TDM IP that the firmware's `I2s.asl` models. There are ten instances:

| Node | Base | GIC SPI | ACPI GSIV |
|------|------|---------|-----------|
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

Each SAI needs: an `MCLK_SAIx` + `HCLK_SAIx` clock from the CRU (and an audio
PLL), a power domain (`PD_VO0/VO1` or the bus domain), resets, and a **DMA**
channel on `dmac2` (the PL330). All instances are `status = "disabled"` by
default in the device tree.

The codec is the Everest **ES8388** (register-compatible with the kernel's
`es8328` driver) on **I²C**, with a jack-detect **GPIO** interrupt — see the
firmware `Es8388.asl` (`I2cSerialBusV2` + `GpioInt`, addressed by board macros).

## Why it's blocked (firmware / ACPI prerequisites)

1. **No correct ACPI.** The firmware's `I2s.asl` carries a file-header warning
   that it contains **RK3588** MMIO addresses and CRU offsets throughout and
   "must NOT be included in any RK3576 platform DSDT until all addresses have
   been corrected." No RK3576 board DSDT includes it, and no `.dsc` sets
   `PcdI2S0Supported`. So Windows sees **no** audio controller.
2. **Wrong IP model.** Even with corrected addresses, `I2s.asl` models the old
   I²S, while RK3576 is SAI — a different register block. The ACPI must describe
   SAI (addresses above) with its DMA + interrupt.
3. **No clock / PLL / DMA / power setup.** SAI needs the audio PLL, `MCLK/HCLK`,
   the `dmac2` channel, the power domain and resets brought up. None of this is
   established for RK3576 in firmware; the `I2s.asl` `_DSM` pokes RK3588 CRU/PLL
   registers.
4. **`_HID` collision.** `I2s.asl` uses `_HID "RKCP3003"`, which this project
   already assigns to **SPI**. A distinct id (e.g. `RKCP3004`) is needed for the
   SAI device.

A WaveRT miniport binds an ACPI device, programs the controller's audio DMA to
move samples to/from the WaveRT cyclic buffer, and configures the codec over
I²C. Without items 1–3 there is no device, no clock and no DMA to drive, so the
driver has nothing to attach to.

## What a real driver would need (later, large)

- A **PortCls/WaveRT** miniport (wave + topology) — a C++ COM driver, the audio
  equivalent in size/complexity to the storage/NIC class extensions.
- SAI controller init + the **PL330 (dmac2)** audio DMA to/from the WaveRT buffer.
- **ES8388 codec** init over I²C (port the `es8328.c` register sequence: power,
  clocking, format, DAC/ADC routing, volume) — this driver depends on the
  [I²C driver](../drivers/i2c/rk3xi2c) and the jack-detect [GPIO](../drivers/gpio/rk3576gpio).
- Firmware ACPI exposing the SAI + codec with correct resources (items 1–4).

## Stopgap that works today: USB Audio

The USB host stack is the inbox xHCI driver (see [ARCHITECTURE.md](ARCHITECTURE.md)),
and Windows has an **inbox USB Audio Class** driver. A USB DAC / headset / dock
gives working audio **with no driver from this repo** — the practical path to
sound on RK3576 WOA until the on-board SAI + ES8388 prerequisites are done in
firmware.

## Recommendation

Do not write the WaveRT driver yet. Sequence the firmware-side work first
(SAI ACPI enumeration + clocks/DMA/power), then port the codec sequence and build
the WaveRT miniport. Use USB Audio in the meantime. The SAI addresses/IRQs above
are the starting point for the firmware ACPI work.
