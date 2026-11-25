# Regression suite

English | [简体中文](README.zh-CN.md)

One command per board, one PASS / FAIL / SKIP / N-A row per check, for the
three RK3576 boards: LCKFB TaishanPi 3M, ArmSoM CM5-IO and Radxa ROCK 4D.
`regress.ps1` runs under Windows 11 23H2, `regress.sh` under Linux
(Armbian, Fedora; device tree boot), and `run-remote.sh` runs either one
from the PC over SSH and copies the results back.

The suite only reads. It installs and uninstalls nothing, changes no
driver, firmware or boot setting, and never restarts or powers off the
board, so it can run at any time. What it writes: its results folder, one
temporary file for the storage check (deleted afterwards) and, on Windows,
a one-off scheduled task for the signed-in user for the checks that need
the desktop (deleted afterwards). The only heavy check, FurMark, runs only
with `-Stress`.

## Running it

From the PC (the board runs OpenSSH; on Windows with PowerShell as the
shell, an administrator account and autologon):

```sh
tools/regress/run-remote.sh woo@tspi3m.lan             # Windows, every check but -Stress
tools/regress/run-remote.sh woo@cm5io.lan -Quick       # the short ones only
tools/regress/run-remote.sh woo@tspi3m.lan -Stress -StressSeconds 300
tools/regress/run-remote.sh root@rock4d.lan --quick    # Linux (told apart automatically)
```

The results land in `tools/regress/results/<host>-<time>/` (`results.md`,
`results.json` and the output of each tool). The exit code is the suite's:
0 no FAIL, 1 at least one FAIL, 2 the suite could not run (not elevated,
unknown board), 3 SSH or the copy failed. Before a Windows run it fetches
the decode streams (`fetch-streams.sh`) and copies them along; tools
placed in `tools/regress/bin/` (for example `vdpu383test.exe`) go along as
well.

On the board itself, as administrator:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File regress.ps1 [-Board auto|tspi3m|cm5io|rock4d] [-Quick] [-Out DIR]
```

```sh
sudo ./regress.sh [--board auto|tspi3m|cm5io|rock4d] [--quick] [--out DIR]
```

### Options of regress.ps1

| Option | Meaning |
|---|---|
| `-Board` | `auto` (default) reads SMBIOS: type 1 manufacturer and product (`LCKFB TaishanPi 3M`, `ArmSoM CM5-IO`, `Radxa ROCK 4D`, from each board's `PcdPlatformVendorName` and `PcdPlatformName`), or the BIOS version `rk3576-<PLATFORM_NAME>-...`. The ACPI OEM IDs (`RKCP` / `RK3576`) are the same on all three boards, so they are checked, not used to tell boards apart |
| `-Quick` | 16 MiB storage check instead of 64; no conformance streams |
| `-Stress`, `-StressSeconds N` | FurMark through GpuTest for N seconds (default 300), 1024x640, in the user's session. Stopped at once when any thermal zone or the GPU reaches 90 C, and refused when anything is at 80 C or more before it starts |
| `-GpuTest PATH` | GpuTest.exe; the default is the path on the boards (`C:\图吧工具箱\...\GpuTest_Windows x64\GpuTest.exe`) |
| `-Out DIR` | results folder (default `C:\RK3576-regress\<board>-<time>`) |
| `-ToolsDir DIR` | where `rknputest.exe` and `vdpu383test.exe` are; also searched: `bin\` next to the script, `%ProgramFiles%\RK3576`, `%ProgramFiles%\RK3576\npu` (the RK3576 tools bundle), `PATH` |
| `-User NAME` | the desktop user for the session checks (default: the signed-in user) |
| `-ExpectFirmware REGEX` | also require the firmware version string to match, for example a release tag |
| `-Require nvme,sd,...` | treat optional parts as required for this run: `nvme`, `sd`, `emmc`, `wifi`, `bluetooth`, `npu`, `vdec`, `analog`, `hdmiaudio`, and `jack` (the headphone endpoint must be active: plug something into the jack) |
| `-FetchStreams` | download missing decode streams on the board itself |
| `-DryRun -Mock FILE` | evaluate the checks against recorded facts (see Tests) |

## The checks

Every check is gated by the board manifest in [`boards/`](boards), written
from the firmware's per-board feature list. A manifest value is `true` (must be there: missing is a FAIL), `"optional"`
(missing is a SKIP, for parts that are not always fitted or not always
enabled) or `false` (N-A). Edit the manifest when a board changes.

### Windows (regress.ps1)

| id | What is checked | Source of the rule |
|---|---|---|
| `fw.banner` | SMBIOS BIOS version is `rk3576-<PLATFORM_NAME>-<git describe>` of this board (`-dirty` is noted). It names the board the image was built for; the revision in it can be stale when the module that holds it was not rebuilt, so it does not prove which build is flashed | `scripts/build.sh` `FIRMWARE_VER` |
| `fw.acpi` | the DSDT is there and its OEM ID / table ID is `RKCP` / `RK3576` | `AcpiTables.h` |
| `board.smbios` | SMBIOS manufacturer and product are this board's | the board's `.dsc` |
| `cpu.count`, `cpu.freq` | 8 logical CPUs; the highest clock (`~MHz` and `\Processor Information(*)\Processor Frequency`) at least 1600 MHz | CPU Maximum Frequency default 1608 MHz |
| `devmgr` | no device with a problem code (22, disabled, is fine) outside the board's whitelist; IDs cut after the hardware ID | Maskrom Studio's first-boot self-test |
| `gpu.driver` | `ACPI\RKCP7402` bound to `maliwddm`, status OK | |
| `gpu.wddm` | `\RK3576 GPU(*)\WDDM version` = 20; on a FAIL the fallback reason counter is shown | maliwddm counters (`maliwddm-counters.cmd`) |
| `gpu.clock` | `\RK3576 GPU(*)\Clock (MHz)`, or `GpuClkHz` in Device Parameters: 702 MHz on the TaishanPi 3M (775 mV rail), 594 elsewhere; a throttled GPU is named as the reason | |
| `gpu.temp` | `\RK3576 GPU(*)\Temperature (C)` between 10 and 90 C | |
| `display.mode` | in the user's session (`EnumDisplaySettings`): the current mode is the largest one offered at 59 or 60 Hz, and runs at 59 or 60 Hz | |
| `display.tdr` | no Display event 4101 (driver reset) in the System log since the run started; evaluated last | |
| `audio.hdmi`, `audio.analog` | endpoints in every state through the MMDevice API (PnP's present endpoints when that fails). HDMI: an active render endpoint of the csaudiork3x device on the manifest's `hdmiSai` (`ACPI\RKCP3004\6`), found by its KS filter (`topologyhdmi`) or its name (`HDMI` or `Digital Output`); it needs a monitor with speakers. Analog, on a board with `analogCodec`: the codec (`ACPI\RKCP8388`) and the csaudiork3x device on `analogSai` started; an empty jack hides the headphone endpoint, so it is only reported unless `-Require jack`. Without `analogCodec`: an active headphone endpoint | manifest `audio.*Endpoint`, `hdmiSai`, `analogSai`, `analogCodec` |
| `net.ethernet`, `net.gateway` | the adapter on `ACPI\RKCP6543` is Up with an IPv4 address; the default gateway answers a ping | |
| `net.wifi`, `net.bluetooth` | TaishanPi 3M only: an 802.11 adapter (optional: its driver is on a branch); `RKCP4345` OK and a Bluetooth device OK | |
| `usb.controllers`, `usb.devices` | at least two `ACPI\PNP0D10` xHCI controllers OK; the USB devices are listed, and on the CM5-IO the onboard hub must be there | |
| `storage.emmc`, `storage.sd`, `storage.nvme` | a disk behind `ACPI\RKCP0D40` (eMMC), `ACPI\RKCPFE2C` (SD), on PCIe (NVMe); Healthy | |
| `storage.rw` | 64 MiB (16 with `-Quick`) written to the system disk with write-through, read back, SHA-256 compared, deleted | Maskrom Studio's self-test |
| `npu.info`, `npu.m3` | when `ACPI\RKCP3576` is there (the NPU setup option is on): bound to `rknpu`, `rknputest info` exits 0 and reports the manifest's `npuCores` (2), `rknputest m3 --core both --iters 1` exits 0 (bit-exact) | woa-rk3576-npu `tools/rknputest` |
| `vdec.warmup`, `vdec.<codec>.<stream>` | when `ACPI\RKCP0383` is there: bound to `vdpu383`, `vdpu383test warmup` exits 0, then every stream of [`streams.txt`](streams.txt) through `vdpu383test decode` with frame count, hardware errors 0 and the MD5 of the output | `tools/vdpu383test/hevc-conformance.ps1` |
| `thermal.zones` | at least three ACPI thermal zones, each between 0 and 90 C | rk-monitor's counter |
| `time.clock`, `time.rtc` | the year is 2026..2040 and the clock is within 120 s of `time.windows.com` (`w32tm /stripchart`, which reads only); `HYMB0001` OK | |
| `stress.furmark`, `stress.gpu-after` | `-Stress` only: FurMark ran its time without reaching 90 C; the GPU is still OK after it; the GpuTest score is recorded | |

Programs that need the desktop (the display mode probe, GpuTest) are
started in the signed-in user's session, because an SSH session is session
0 and has none: `schtasks /create /ru <user> /it` with `conhost.exe
--headless`, run once, deleted. Without a signed-in user (autologon off)
those checks are a SKIP.

### Checks per board

| | TaishanPi 3M | CM5-IO | ROCK 4D |
|---|---|---|---|
| Device Manager whitelist | EC20 interface 4 (`USB\VID_2C7C&PID_0125&MI_04`) | none | AIC8800D80 USB Wi-Fi (`USB\VID_A69C`) |
| GPU clock | 702 MHz | 594 MHz | 594 MHz |
| Analog audio | required | optional (ES8388 on branch `cm5io-parity`) | required |
| Wi-Fi / Bluetooth | optional / required | N-A | N-A (USB module, no driver on 23H2) |
| USB hub | | required | |
| eMMC / SD / NVMe | required / optional / optional | required / optional / required | N-A / optional / optional |
| NPU | when enabled in setup | optional (setup option only on `cm5io-parity`) | when enabled in setup |
| Video decoder | when enabled in setup | when enabled in setup | when enabled in setup |
| RTC | no battery fitted: time comes from the network | battery | battery |

### Linux (regress.sh)

`board.dt` (device tree compatible), `fw.banner` (SMBIOS when booted
through UEFI, SKIP through U-Boot), `cpu.count`, `cpu.freq` (cpufreq),
`dmesg` (no `err`, `crit`, `alert` or `emerg` line outside the manifest's
`linux.dmesgIgnore`; the lines go to `dmesg-errors.txt`), `net.ethernet`
and `net.gateway` (an interface bound to the GMAC driver, up, IPv4, the
gateway answers), `usb.controllers`, `usb.devices` (the CM5-IO hub),
`storage.emmc`, `storage.sd`, `storage.nvme`, `storage.rw` (in `/var/tmp`,
read back with `iflag=direct`), `display.hdmi` (DRM connector
`connected`), `gpu.driver` (panfrost), `npu.driver` and `vdec.driver`
(reported when the kernel has them), `thermal.zones` (the six zones of
`rk3576.dtsi`), `fan.pwm` (a `pwmfan` hwmon; the mainline device trees have
none, so optional), `time.rtc` (the HYM8563), `time.clock`, and
`power.off`, always N-A: the suite never powers the board off.

## The decode streams

[`streams.txt`](streams.txt) lists nine small streams (four H.264, five
HEVC Main; together about 380 KB) from FFmpeg's fate-suite copies of the
ITU-T conformance streams, each with its SHA-256, frame count and the MD5
of the decoded I420 output. They are not stored in git:
`fetch-streams.sh` downloads them into `streams/` and checks the hashes,
and `run-remote.sh` copies them to the board.

## Tests

No board is needed:

* `tests/run-dryrun.ps1` parses every `.ps1` here, checks that a file with
  non-ASCII text has a UTF-8 BOM (Windows PowerShell 5.1 reads it as ANSI
  otherwise), and runs `regress.ps1 -DryRun` against each
  `tests/mock-*.json`. A mock holds the facts a board would report and the
  results it expects (good boards, planted regressions, missing counters,
  a stress run stopped at 90 C). It also checks that the suite writes
  nothing outside `-Out`.
* `tests/test-regress-sh.sh` builds `/sys` and `/proc` trees for made-up
  boards (`REGRESS_ROOT`), puts stub `dmesg`, `ip`, `ping` and
  `timedatectl` first in `PATH`, and checks five cases; then runs
  `run-remote.sh` with stub `ssh` and `scp`.

CI runs both on every push (`regress-linux`, `regress-windows` in
[`ci.yml`](../../.github/workflows/ci.yml)), under PowerShell 7 and Windows
PowerShell 5.1, plus one live `regress.ps1` run on the x64 runner (most
rows FAIL there; it must finish, pass the storage check and leave nothing
behind).

## Adding a check

1. Collect the facts in a `Fact 'name' { ... }` block, so that a mock can
   supply them.
2. Decide with `Add-Result id name status measured expected reason`, gated
   by `Cap` on a manifest value.
3. Add the fact to the mocks and the expected result to their `expect`.
