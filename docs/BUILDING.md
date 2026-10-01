# Building and installing the drivers

These are Windows ARM64 kernel drivers. They build with the WDK on Windows or
in CI, not on Linux.

## From CI

- [ci.yml](../.github/workflows/ci.yml) builds `build/RockchipDrivers.sln` and
  `rk3xspi` on every push with WDK 10.0.26100 and fails on any error. Its
  packages are not signed.
- [winpe.yml](../.github/workflows/winpe.yml) builds the same drivers,
  test-signs them with a throwaway certificate, injects them into an ADK 22621
  WinPE and uploads the image. The signed packages are in its `woa-drivers\`
  folder. That is the set to install.

## Locally

An ARM64 Windows machine builds natively. Install Visual Studio 2022 with the
ARM64 build tools and Spectre-mitigated ARM64 libraries, and WDK 10.0.26100.

```cmd
msbuild build\RockchipDrivers.sln /p:Configuration=Release /p:Platform=ARM64
msbuild drivers\spi\rk3xspi\rk3xspi.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

Packages land in `build\ARM64\Release\Output\<driver>\`. Sign them the way
`winpe.yml` does: `inf2cat /os:10_RS3_ARM64`, then `signtool sign` the `.cat`
and `.sys`.

## Installing

The board needs test signing: `bcdedit /set testsigning on`, then reboot. A
signature that does not chain to a trusted root is then accepted; an unsigned
driver still is not.

- New installation: `tools/woa-deploy/deploy-windows.cmd` injects the drivers.
  See [INSTALL.md](INSTALL.md).
- Existing installation, offline from WinPE: `tools/woa-deploy/add-drivers.cmd`.
- Existing installation, one driver, online: `pnputil /add-driver x.inf
  /install` stages it, but Windows may keep the version already installed.
  `tools/woa-debug/net-update.cmd` shows how to remove the old one.

## Headers without a WDK

The [wdk-headers](../.github/workflows/wdk-headers.yml) workflow exports the
runner's `Include\10.0.26100.0\{km,shared}` tree as an artifact, the same
headers CI compiles against. Grep that instead of guessing at an identifier.

## Project settings that are not obvious

Each of these was a build failure first.

| Setting | Why |
|---|---|
| `MARMASM` with explicit `marmasm.props`/`.targets` imports | The ARM64 toolset has no masm build customization; `<MASM>` items are silently ignored. |
| `$(SPB_INC_PATH)\$(SPB_VERSION_MAJOR).$(SPB_VERSION_MINOR)` and `SpbCxStubs.lib` | `spbcx.h` is not in `km`, and there is no `spbcx.lib`. |
| `msgpioclxstub.lib` and `ksguid.lib` | There is no `gpioclx.lib`. |
| `NetAdapterDriver=true` and `NETADAPTER_VERSION_*` | This adds the NetCx include path and import library. Naming `netadaptercx.lib` does nothing. |
| `KMDF_VERSION_*` in the `Label="Configuration"` group | Declared after `Microsoft.Cpp.props` it is read too late: the build used KMDF 1.15 while stampinf wrote 1.33 into the INF. |
| `Inf2CatWindowsVersionList` starting at `10_RS3_ARM64` | The ARM64 default, `Server10_ARM64` (14393), is older than the 16299 these INFs need for DIRID 13. `10_GE_ARM64` (24H2) is left out because 24H2 cannot run on RK3576. |
| `FilesToPackage` | Without it the package holds only the INF and inf2cat fails with `22.9.1 ... .sys is missing`. |
| INF models decorated `NTARM64.10.0...16299` | DIRID 13 requires it; undecorated, InfVerif reports error 1199. |

`_KERNEL_MODE` is predefined by the WDK toolset. Defining it again is C4117,
which `/WX` turns into an error in every file.
