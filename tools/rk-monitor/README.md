# rk-monitor

A tray icon for RK3576 boards under Windows on Arm. Windows 11 23H2, the
newest release these boards run, has no NPU page in Task Manager and shows
no temperatures; this shows them, with CPU and GPU state next to them.

<!-- screenshot placeholder: the popup on the TaishanPi 3M, to be taken on
     the board and added here as rk-monitor.png -->
*Screenshot to come (the popup on the TaishanPi 3M).*

The icon is the hottest ACPI thermal zone in degrees Celsius (red from
85 °C). Its tooltip has CPU use per cluster, the temperature, GPU use and
NPU busy. A left click opens the popup, refreshed every second:

| section | row | source |
|---|---|---|
| CPU | `A53, cores 0-3`, `A72, cores 4-7`: use and clock, with a 60 second history | `\Processor(*)\% Processor Time`, `\Processor Information(*)\Processor Frequency` |
| CPU | one row per thermal zone | `\Thermal Zone Information(*)\High Precision Temperature` (tenths of a kelvin). Not WMI's `MSAcpi_ThermalZoneTemperature`, which returns a stale cached value on these boards |
| GPU | utilization, per engine type, with history | `\GPU Engine(*)\Utilization Percentage`, summed per engine as Task Manager does |
| GPU | memory | `\GPU Adapter Memory(*)\Shared Usage` and `Dedicated Usage`, all adapters |
| GPU | busy, temperature, clock, throttling | `\RK3576 GPU(*)\% busy`, `Temperature (C)`, `Clock (MHz)`, `Throttled` from maliwddm |
| NPU | `core0`, `core1`: % busy and jobs per second, with history | `\RK3576 NPU(*)\% Busy Time`, `Jobs/sec` from rknpu.sys |

The clusters come from the efficiency classes Windows reports; when it
reports one class for every core and there are 8, cores 0-3 are taken as
the A53s and 4-7 as the A72s.

A counter set that is not there is not an error. The row says so (`not
reported`, `driver has no counters`) with the command that registers it,
and rk-monitor looks for it again every 30 seconds, so it picks the
counters up once the driver or the registration arrives:

- GPU: `maliwddm-counters.cmd install` from the maliwddm package
  (wddm-mali-g52), after every driver update;
- NPU: `rknpu-perfcounters.ps1` from the rknpu package (woa-rk3576-npu,
  docs/PERFCOUNTERS.md), as administrator, after every driver update.

Kernel-mode counter providers are registered with lodctr, which no INF can
run, hence the separate step.

## Desktop widget

Tray menu, **Desktop widget**: a small panel on the desktop with four
lines, CPU (per cluster, with a graph), GPU (use, temperature, clock), NPU
(per core, with a graph) and the thermal zones. It is a child of the window
that holds the desktop icons, so it stays under every application and is
still there after Win+D or "show desktop". Its background is translucent
and follows light or dark mode. Drag it anywhere; the place is remembered
per user (and it goes back to the default, the top right of the main
screen, when that monitor is gone). **Widget click-through** lets clicks
pass to the desktop; it can then be moved again after turning that off.
Right-click on the widget opens the tray menu.

It is redrawn only when something on it changes, which on an idle board
is the graphs moving; a sample costs one PDH collection a second. Should
Explorer restart, the widget comes back with the tray icon. If the desktop
window cannot take a child, the widget falls back to a bottom-most
ordinary window, which Win+D does hide.

Installed with the RK3576 tools ([../rk3576-tools](../rk3576-tools/README.md)),
the widget is on by default (`HKLM\Software\RK3576\Monitor`, `Widget` = 1);
each user's own choice is kept in `HKCU\Software\RK3576\Monitor`
(`Widget`, `WidgetClickThrough`, `WidgetX`, `WidgetY`).

## Use

It is one portable exe, `rk-monitor.exe`. Copy it anywhere and run it, or
install it for every user with the RK3576 tools bundle
([../rk3576-tools](../rk3576-tools/README.md)), which also registers the
GPU and NPU counters. Right-click the icon for **Start with Windows** (an
entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, pointing
at the exe where it is now; shown checked and greyed when the installer
already starts it for all users) and **Exit**. A second start opens the popup of
the one already running. It follows the light or dark app mode, and the
taskbar's mode for the icon.

`rk-monitor.exe --once` prints one sample as text, followed by which
counter sets were found, and exits:

```
rk-monitor.exe --once > R:\rk-monitor.txt
```

## Build

```cmd
msbuild tools\rk-monitor\rk-monitor.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

C and Win32 only (PDH, GDI, the shell's notification area), with the C
runtime linked statically. CI builds ARM64 and x64 on every push, runs the
x64 build with `--once`, and uploads `rk-monitor-arm64` and
`rk-monitor-x64`.
