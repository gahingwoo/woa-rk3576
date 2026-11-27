# Edge, Chrome and WebView2 on the GPU

English | [简体中文](BROWSER-GPU.zh-CN.md)

Edge, Chrome and WebView2 need no switch on the fixed maliwddm
(wddm-mali-g52 2df1808 or later). They render through DirectComposition at
60 fps at 1440p (TaishanPi 3M and CM5-IO, 2026-11-27).

Version 1 of `setup\rk3576-browser-gpu.ps1` added
`--disable-direct-composition` to their launch paths. That switch never
helped. With it, Chromium presents through ANGLE's blt-model swap chain:
those presents never reached the screen on an older maliwddm (a white Edge
window), and they reach it at 32 fps on the fixed one. An older maliwddm
without the switch falls back to software rendering, which is usable.
wddm-mali-g52 `docs/EDGE-PRESENT.md` has the measurements.

The script now only takes out what version 1 added. It never adds anything.

## What it takes out

Only from entries whose program is `msedge.exe`, `chrome.exe`,
`msedge_proxy.exe` or `chrome_proxy.exe`:

- the switch in shortcuts: all users' Start menu and desktop, each user's
  Start menu, desktop and Quick Launch (pinned taskbar icons);
- the switch in open commands (`shell\<verb>\command`) of the `MSEdge*`,
  `microsoft-edge*`, `Chrome*`, `Chromium*` and `Applications\` keys in
  `HKLM\SOFTWARE\Classes` and each user's classes, and of
  `Clients\StartMenuInternet\*`;
- the switch in the `Run` values of the machine and of each user;
- the policies version 1 set (Edge `StartupBoostEnabled` and
  `BackgroundModeEnabled`, Chrome `BackgroundModeEnabled`). Each gets back
  the value version 1 saved, unless an administrator changed it since;
- the switch in `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` (machine) and in
  `HKLM\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments`.

`Install` and `Uninstall` also remove version 1's task
`\RK3576\Browser GPU`, its copy in `%ProgramFiles%\RK3576\browser-gpu` and
its settings `HKLM\SOFTWARE\RK3576\BrowserGpu`.

## Commands

From an administrator prompt:

```
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Status
powershell -ExecutionPolicy Bypass -File rk3576-browser-gpu.ps1 -Action Install
```

| action | what it does |
|---|---|
| `Status` | lists what version 1 left behind |
| `Install`, `Uninstall` | take everything out, also for users who are not signed in, and remove the task, the copy and the settings |
| `Apply`, `Revert`, `Enable`, `Disable` | take the switch, the policies and the WebView2 arguments out; kept for old callers (version 1's task runs `Apply`) |

`-DryRun` changes nothing. The log is `%ProgramData%\RK3576\browser-gpu.log`.
Close and reopen the browser afterwards.

## Who runs it

- The RK3576 tools bundle: `install.cmd` and an offline install's first boot
  run `-Action Install`; its uninstall runs `-Action Uninstall`.
- Maskrom Studio's Windows install runs `-Action Install` from
  `SetupComplete.cmd`, after the tools bundle.

## Testing

CI (`browser-gpu` job in `.github/workflows/ci.yml`) runs
[tests/browser-gpu.tests.ps1](tests/browser-gpu.tests.ps1) against a fake
machine set up the way version 1 left it. On the runner it puts version 1's
switch on the real Edge and Chrome, then checks that version 1's task
running this script, `Install`, `Revert` and `Uninstall` each give back
what was there.
