# Edge, Chrome and WebView2 on the GPU

English | [简体中文](BROWSER-GPU.zh-CN.md)

At WDDM 2.x, Chromium's DirectComposition presents fail on maliwddm. The GPU
process crashes three times and the browser falls back to software
rendering. Started with `--disable-direct-composition`, Edge 158 and
Chrome 158 (ARM64) render on the Mali-G52 through ANGLE D3D11 (feature level
10_1) with no GPU process crash. This was measured on a TaishanPi 3M on
2026-11-25.

The browser process reads that switch as well as the GPU process: without
it, Windows 11 22H2 and later give the browser window no redirection
surface. So maliwddm cannot turn DirectComposition off by itself (the
analysis is in wddm-mali-g52, `docs/EDGE-PRESENT.md`, "DirectComposition off
without a flag"). Neither Edge nor Chrome has a policy for the switch or for
extra command-line switches. So `setup\rk3576-browser-gpu.ps1` puts the
switch on every launch path and keeps it there.

## What it changes

Each entry is changed only when its program is `msedge.exe`, `chrome.exe`,
`msedge_proxy.exe` or `chrome_proxy.exe`, and only when the switch is not
already there.

| where | what |
|---|---|
| Shortcuts: all users' Start menu and desktop; each user's Start menu, desktop (also a redirected one) and Quick Launch, which holds the pinned taskbar icons (`User Pinned\TaskBar`) and `ImplicitAppShortcuts` | the switch goes in front of the shortcut's arguments |
| Open commands in `HKLM\SOFTWARE\Classes` and in each user's classes: the keys `MSEdge*`, `microsoft-edge*`, `Chrome*`, `Chromium*`, `Applications\msedge.exe`, `Applications\chrome.exe`, and `Clients\StartMenuInternet\*` (machine and user) | every `shell\<verb>\command` gets the switch right after the program, before `--single-argument`. Chromium reads everything after `--single-argument` as one URL. The value type (REG_SZ or REG_EXPAND_SZ) is kept |
| `Run` values of the machine and of each user | the same, for a value that starts a browser. Edge's startup boost starts Edge from one |
| Policies `HKLM\SOFTWARE\Policies\Microsoft\Edge` `StartupBoostEnabled` 0 and `BackgroundModeEnabled` 0, and `HKLM\SOFTWARE\Policies\Google\Chrome` `BackgroundModeEnabled` 0 | so no browser instance that was started without the switch is still running. A new window joins the running instance and gets that instance's switches. Each value's earlier state is saved and given back on revert, unless an administrator changed the value after us |
| The machine environment variable `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`, and its registry fallback `HKLM\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments`, value `*` | the switch is appended to what is there. Microsoft's WebView2 reference (`CreateCoreWebView2EnvironmentWithOptions`) says that arguments from the variable or from the registry are appended to the arguments the app passes itself, so an app's own switches stay. The registry is read when the variable is not set, so a WebView2 started with an older environment block gets the switch too. A user's own variable of that name hides the machine one for that user |

Edge and Chrome show "managed by your organization" once any of their
policies is set. Maskrom Studio's Windows trim sets the two Edge policies
anyway. `Policies` = 0 (below) leaves all three alone.

Nothing else is touched: no browser files, no profiles, no preferences.

## When

The scheduled task `\RK3576\Browser GPU` runs `-Action Apply` as SYSTEM:

- 1 minute after every start;
- 30 s after any user signs in (that user's registry and shortcuts are
  loaded then; Windows creates the default taskbar pins at the first
  sign-in);
- every hour;
- 2 minutes after an MSI install, repair or removal (the Chrome MSI), or an
  Application log event from the providers `edgeupdate`, `gupdate` or
  `GoogleUpdater`. Whether these updaters write such events was not
  checked on the board; the hourly run covers it if they do not.

A browser update rewrites its shortcuts and open commands. The next run puts
the switch back, and a run that finds everything in place changes nothing
and writes nothing to the log. A browser opened between the update and that
run renders in software until it is restarted.

The gate, with the default mode: `ACPI\RKCP7402` is driven by `maliwddm`,
Device Manager shows no problem for it, and its
`Device Parameters\maliwddm\WddmVersion` is 0x2000 or more. When that is
clearly not so (no such device, another driver, a problem code, or WDDM 1.3
after a fallback), the switch is taken out again. At 1.3 Edge needs no
switch. When the state cannot be read (WMI not answering, no `WddmVersion`
yet), nothing is changed.

## Settings

`HKLM\SOFTWARE\RK3576\BrowserGpu` (all optional):

| value | meaning |
|---|---|
| `Mode` DWORD | 0 off (everything taken out at the next run), 1 auto (default), 2 always (no gate) |
| `Policies` DWORD | 0: leave the Edge and Chrome policies alone |
| `WebView2` DWORD | 0: leave WebView2 alone |
| `LastRun`, `LastGate`, `Applied`, `Version`, `Saved\` | written by the script |

Per user, `HKCU\Software\RK3576\BrowserGpu` `Mode` = 0 keeps the switch off
that user's own shortcuts, classes and Run values.

## Commands

From an administrator prompt, in `%ProgramFiles%\RK3576\browser-gpu`:

```
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Status
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Apply -DryRun
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Disable
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Enable
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Uninstall
```

`Status` prints the mode, the gate and every entry with and without the
switch. `-DryRun` works with every action and changes nothing. `Disable`
sets `Mode` 0 and takes everything out, and the task keeps it out.
`Uninstall` takes everything out, also from users who are not signed in
(their hives are loaded for it), and removes the task, the copy and the
settings. The log is `%ProgramData%\RK3576\browser-gpu.log`.

## Who installs it

- The RK3576 tools bundle: `install.cmd` and the first-boot step of an
  offline install run `-Action Install`; its uninstall runs
  `-Action Uninstall` first. So `install.cmd` on an existing Windows also
  turns this on.
- Maskrom Studio's Windows install carries its own copy of the script and
  runs `-Action Install` from `SetupComplete.cmd`, with or without the tools
  bundle.

Both put the script in `%ProgramFiles%\RK3576\browser-gpu\` and register
the same task, so only one copy runs. Install keeps the higher `$BgVersion`
of the two copies. A tools bundle install that replaces `%ProgramFiles%\RK3576`
installs the bundle's own copy again.

## Not covered

- A browser started directly from its `.exe` (Explorer, Run box, a portable
  Chrome without a shortcut), or by a program with its own command line. It
  renders in software. A shortcut on the desktop or in the Start menu is
  covered, wherever its target is.
- Shortcuts outside the folders listed above, such as a shortcut on another
  drive.
- A browser that is already running when the switch is added. It gets the
  switch at its next start.

## Testing

CI (`browser-gpu` job in `.github/workflows/ci.yml`): the unit tests in
[tests/browser-gpu.tests.ps1](tests/browser-gpu.tests.ps1) under Windows
PowerShell 5.1 and PowerShell 7, against a made-up machine and user under
`HKCU\Software\RK3576BgTest` and `.lnk` files in a temporary folder. Then,
on the runner's own Edge and Chrome: Install (the gate says no, nothing
changes), `Mode` 2, the task's own run (the switch on the Edge and Chrome
open commands and Start menu shortcuts, policies, WebView2), a second run
that changes nothing, Disable, Uninstall, and a check that everything is as
it was before.

On the board (TaishanPi 3M, WDDM 2.0):

1. `Status`: gate Yes, `WddmVersion` 0x2000.
2. Start Edge from the taskbar, open `edge://gpu`: "Compositing: Hardware
   accelerated", and in the command line `--disable-direct-composition`.
   Same for Chrome (`chrome://gpu`) from the Start menu, and a link
   opened from another program (the open command).
3. Pin Chrome to the taskbar again, wait for the hourly run or run the task
   (`schtasks /run /tn "\RK3576\Browser GPU"`), start Chrome from the new pin.
4. `Disable`: the switch is gone everywhere (`Status`); `Enable` puts it back.
