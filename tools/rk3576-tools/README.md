# RK3576 tools

English | [简体中文](README.zh-CN.md)

The user-facing programs for RK3576 boards under Windows on Arm, in one
bundle that installs into `%ProgramFiles%\RK3576`:

| file | what it is | from |
|---|---|---|
| `rk-monitor.exe` | the tray monitor and desktop widget: CPU, GPU and NPU use, temperatures ([tools/rk-monitor](../rk-monitor/README.md)) | woa-rk3576 |
| `npu\rknputest.exe` | the NPU driver's test tool (`rknputest info`, `rknputest m6`) | woa-rk3576-npu |
| `npu\charsiu_run.exe` | runs a language model (gguf) on the NPU | woa-rk3576-npu (charsiu, built for Windows) |
| `npu\rknpu-perfcounters.ps1` | registers the NPU counters by hand (the installer does it on its own) | woa-rk3576-npu |
| `setup\` | the installer's scripts, kept for the first boot, the counter task and uninstalling | woa-rk3576 |
| `setup\rk3576-browser-gpu.ps1` | Edge, Chrome and WebView2 on the GPU at WDDM 2.x ([BROWSER-GPU.md](BROWSER-GPU.md)); installed to `browser-gpu\` | woa-rk3576 |
| `VERSION.txt` | the bundle version and the commits it was built from | CI |

CI builds it on every push of woa-rk3576 as the artifact `rk3576-tools-arm64`:
rk-monitor from this repository, rknputest and charsiu_run from
woa-rk3576-npu at the commit `NPU_REF` in `.github/workflows/ci.yml` names.

## Install on a running Windows

Unpack the artifact anywhere and run `install.cmd` (it asks for
administrator rights). It:

- copies `RK3576\` to `%ProgramFiles%\RK3576` (again: an upgrade in place;
  a running rk-monitor is stopped first and started again for you);
- starts rk-monitor at every logon, for all users, with the desktop widget
  on (each user can turn it off in the tray menu), through the task
  `\RK3576\RK3576 Monitor` (any user's logon, run as that user, not
  elevated, no delay). It shows "..." for about 5 s until the first sample;
- adds `Start menu\Programs\RK3576`: RK3576 Monitor, RK3576 tools folder,
  Uninstall RK3576 tools; and the entry in Settings, Apps;
- registers the GPU (`RK3576 GPU`, maliwddm) and NPU (`RK3576 NPU`, rknpu)
  performance counters with lodctr, from the manifests in the driver store,
  and adds the task `\RK3576\Performance counters` (SYSTEM, 30 s after every
  start) that registers them again when a driver update has moved them. A
  driver that is not installed is skipped and picked up at a later start.
- makes Edge, Chrome and WebView2 render on the GPU while it runs WDDM 2.x:
  `setup\rk3576-browser-gpu.ps1 -Action Install` adds
  `--disable-direct-composition` to their shortcuts, open commands and
  WebView2's arguments, and the task `\RK3576\Browser GPU` keeps it there
  after browser updates ([BROWSER-GPU.md](BROWSER-GPU.md)).

`install.cmd /quiet /nostart` installs without the pause and without
starting rk-monitor now. Log: `%ProgramData%\RK3576\setup.log`.

## Uninstall

Settings, Apps, RK3576 tools; or the Start menu's Uninstall RK3576 tools; or
`uninstall.cmd` (next to `install.cmd`, or in `%ProgramFiles%\RK3576\setup`).
It removes everything the install added: the folder, the shortcuts, the
logon task (and the Run entry of an offline install), `HKLM\SOFTWARE\RK3576`,
the Apps entry, the counter task and the task folder, the
counter registrations it made, the browser GPU switch (taken out of every
shortcut and command first, policies given back), `%ProgramData%\RK3576`, and every user's
rk-monitor settings (`HKCU\Software\RK3576` and a per-user Run entry,
through the profiles' `NTUSER.DAT` for users who are not signed in). The
drivers are not touched.

## Offline install (for an image installer)

For a Windows that is not running: an image applied to a disk from WinPE,
or a mounted `install.wim`. Maskrom Studio uses this when it installs
Windows. `offline-install.cmd` needs only cmd and reg.exe (WinPE has no
PowerShell):

```
offline-install.cmd W:\ /hook
```

`W:\` is the root of the new Windows. The interface, should an installer
want to do the same itself:

**1. Files.** Copy the bundle's `RK3576\` folder, unchanged, to
`<root>\Program Files\RK3576\`.

**2. Registry**, in the offline `SOFTWARE` hive
(`<root>\Windows\System32\config\SOFTWARE`, e.g. `reg load HKLM\X ...`):

| key (under the hive root) | value | type | data |
|---|---|---|---|
| `Microsoft\Windows\CurrentVersion\Run` | `RK3576 Monitor` | REG_EXPAND_SZ | `"%ProgramFiles%\RK3576\rk-monitor.exe" --autostart` |
| `RK3576\Monitor` | `Widget` | REG_DWORD | `1` |
| `RK3576` | `InstallDir` | REG_EXPAND_SZ | `%ProgramFiles%\RK3576` |
| `RK3576` | `Version` | REG_SZ | first line of `VERSION.txt` |
| `RK3576` | `FirstBootPending` | REG_DWORD | `1` (the first-boot step deletes it) |
| `Microsoft\Windows\CurrentVersion\Uninstall\RK3576Tools` | `DisplayName` `RK3576 tools`, `DisplayVersion`, `Publisher` `woa-rk3576`, `InstallLocation`, `DisplayIcon`, `UninstallString` `"%ProgramFiles%\RK3576\setup\uninstall.cmd"`, `NoModify` 1, `NoRepair` 1 | | |

The `%ProgramFiles%` in the data is literal (REG_EXPAND_SZ), so the value
is right whatever drive letter the image has at install time. The Run entry
only covers the time until the first-boot step has run: that step creates
the logon task `\RK3576\RK3576 Monitor`, sets `RK3576\Monitor`
`AutostartTask` = 1 and deletes the Run entry.

**3. First boot.** Once, as SYSTEM, after Windows setup, run

```
call "%ProgramFiles%\RK3576\setup\firstboot.cmd"
```

The natural place is `<root>\Windows\Setup\Scripts\SetupComplete.cmd`.
`/hook` appends that line to it (or creates it). An installer that writes
its own `SetupComplete.cmd` after calling `offline-install.cmd` (Maskrom
Studio copies its own over it) should leave out `/hook` and put the line in
its file. The step adds the shortcuts, the logon task and the counter task and
registers the counters; it always exits 0, logs to `%ProgramData%\RK3576`, and
`install.cmd` on the running system repairs anything it could not do.

`offline-install.cmd` exits 0 on success, 1 for usage, 2 when `<root>` is
not a Windows or the bundle is incomplete, 3 when copying or the registry
failed (the hive is unloaded again).

## Verify

| Check | Expected |
|---|---|
| after `install.cmd` | tray icon and widget (top right of the main screen); three entries under Start menu > RK3576 |
| `Get-ScheduledTask -TaskPath \RK3576\` | the tasks listed above |
| `Get-Counter -ListSet "RK3576 NPU","RK3576 GPU"` | both sets; `%ProgramData%\RK3576\counters.log` says what was registered |
| reboot and sign in | rk-monitor within a few seconds; no "RK3576 Monitor" in `HKLM\...\CurrentVersion\Run` |
| after a driver update | `counters.log` shows the new driver store folder; rk-monitor's NPU and GPU rows keep their numbers |
| after `uninstall.cmd` | folder, Start menu folder, tasks and counter sets gone; `reg query HKLM\SOFTWARE\RK3576` finds nothing |
