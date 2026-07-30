# Building & installing the RK3576 WOA drivers

These are **Windows ARM64 kernel-mode drivers**. They cannot be compiled on
Linux — you need Microsoft's WDK toolchain. The source in this repo is authored
on a Linux dev box; building happens on Windows or in CI.

All five drivers build clean (zero warnings, `/W4 /WX`) against
**WDK 10.0.26100** for **ARM64/Release**. That is verified by CI on every push —
see [.github/workflows/ci.yml](../.github/workflows/ci.yml). Nothing here has run
on silicon yet.

## The fastest route: let CI build it

The GitHub Actions `WDK build` matrix builds all five projects on the
`windows-2022` runner image, which already ships WDK 10.0.26100 — no toolchain
install step, no chocolatey. Each job uploads a
`rk3576-woa-<name>-arm64` artifact containing the `.sys`, the stamped `.inf`,
the `.cat` and the `.pdb`.

The job fails on any compile, link, `ApiValidator` or `inf2cat` error, and it
fails if the pinned WDK is not present. It does not skip.

## Building locally

An **ARM64 Windows machine builds ARM64 drivers natively** — the WDK is just the
target SDK, the host does not have to be x64. Install Visual Studio 2022 with the
"ARM64/ARM64EC build tools" and Spectre-mitigated ARM64 libraries, plus WDK
10.0.26100. Then, per driver:

```
msbuild drivers\gpio\rk3576gpio\rk3576gpio.vcxproj ^
    /p:Configuration=Release ^
    /p:Platform=ARM64 ^
    /p:WindowsTargetPlatformVersion=10.0.26100.0
```

Output lands in `<projdir>\ARM64\Release\<name>\` — `.sys`, `.inf`, `.cat`.

If you only have an *x64* EWDK ISO, Windows 11 on ARM runs it under emulation and
still emits ARM64 output; mount the ISO, run `LaunchBuildEnv.cmd`, then the same
`msbuild` line. Slower, identical result.

## Authoring on Linux without a WDK

The class-extension ABI (sdport, SpbCx, GpioClx, NetAdapterCx) cannot be checked
from Linux, and guessing an identifier per CI round-trip is not a workflow. Run
the [WDK headers](../.github/workflows/wdk-headers.yml) workflow and download its
artifact: it exports the whole `Include\10.0.26100.0\{km,shared}` tree from the
runner, which is the same header set CI compiles against. Grep that instead of
guessing.

## Project settings that are not obvious

Every one of these was a build failure first. Do not "simplify" them away.

| Setting | Why |
|---|---|
| `MARMASM` + explicit `marmasm.props`/`.targets` imports | The ARM64 toolset ships **no** masm build customization. `<MASM>` items are silently ignored; the SMC stub is a `<MARMASM>` item. |
| `$(SPB_INC_PATH)\$(SPB_VERSION_MAJOR).$(SPB_VERSION_MINOR)` + `SpbCxStubs.lib` | `spbcx.h` is not in `km`, and there is no `spbcx.lib`. |
| `msgpioclxstub.lib` + `ksguid.lib` | There is no `gpioclx.lib`. |
| `NetAdapterDriver=true` + `NETADAPTER_VERSION_*` | This is what makes the WDK targets add the NetCx include path and import library. Naming `netadaptercx.lib` does nothing. |
| `KMDF_VERSION_*` in the `Label="Configuration"` group | Declared after `Microsoft.Cpp.props` it is read too late: the build silently used KMDF 1.15 while stampinf wrote 1.33 into the INF. |
| `DriverTargetPlatform=Desktop` for `rkdwmmc` | `sdport.sys!SdPortInitialize` is not a Universal DDI; `ApiValidator` rejects a Universal sdport miniport. |
| `Inf2CatWindowsVersionList` starting at `10_RS3_ARM64` | The WDK's ARM64 default is `Server10_ARM64` (build 14393), older than the 16299 these INFs need for DIRID 13, so inf2cat finds no installable section. `10_GE_ARM64` (24H2) is deliberately excluded — see the ARMv8.0 ceiling in [BRINGUP-PLAN.md](BRINGUP-PLAN.md). |
| `FilesToPackage` | Without it the package directory holds only the INF and inf2cat fails with `22.9.1 ... .sys is missing`. |
| INF models sections decorated `NTARM64.10.0...16299` | DIRID 13 (run-from-driver-store) requires it; undecorated, InfVerif errors 1199. |

Note also that `_KERNEL_MODE` is predefined by the WDK toolset — defining it
yourself is C4117, which with `/WX` fails every translation unit.

## Test-signing & installing on the board

WOA in dev needs test-signing enabled or an unlocked/insecure boot policy. CI
builds with `SignMode=Off`, so sign locally:

1. On the build host, create a test certificate once and sign the catalog:
   ```
   makecert -r -pe -ss PrivateCertStore -n "CN=WOA-RK3576-Test" testcert.cer
   signtool sign /v /s PrivateCertStore /n "WOA-RK3576-Test" ^
       /fd sha256 /t http://timestamp.digicert.com rk3576gpio.cat
   ```
   The `.cat` itself is produced by the build; you do not need to run `inf2cat`
   by hand.
2. On the **target (WOA on the RK3576 board)**, from an elevated prompt:
   ```
   bcdedit /set testsigning on        :: then reboot
   certutil -addstore root testcert.cer
   certutil -addstore TrustedPublisher testcert.cer
   pnputil /add-driver rk3576gpio.inf /install
   ```
3. Check **Device Manager** → *System devices*: five
   *Rockchip RK3576 GPIO Controller* nodes should be started.

## Debugging

- Kernel debug over serial. On both ROCK 4D and CM5-IO the debug port is
  **UART0 @ `0x2AD40000`, 1,500,000 8N1** — that is what the platform `.dsc` sets
  `PcdSerialRegisterBase` to, and DBG2/SPCR derive their base address from the
  same PCD, so Windows agrees with the firmware console. (DBG2's
  `NameSpaceString` says `\_SB.UAR2`, which is a cosmetic mislabel: Windows uses
  the address register, not the name.) KDNET is an option once the GMAC or USB
  path is up.
- The drivers log via `DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...)`. Enable it in the
  debugger:
  ```
  ed nt!Kd_IHVDRIVER_Mask 0xF
  ```
