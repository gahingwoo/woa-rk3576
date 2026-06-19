# Building & installing the RK3576 WOA drivers

These are **Windows ARM64 kernel-mode drivers**. They cannot be compiled on
Linux — you need Microsoft's WDK toolchain. The source in this repo is authored
on a Linux dev box; building and signing happens on Windows.

## You can build on ARM64 Windows — no x64 host required

An **ARM64 Windows machine builds ARM64 drivers natively**. The WDK is just the
target SDK; the build host doesn't have to be x64. Two ways, both fine on
ARM64-only:

1. **Native ARM64 toolchain (recommended).** Recent Visual Studio 2022 ships a
   native ARM64 build of the IDE + MSVC, and the Windows 11 24H2 **WDK
   (10.0.26100)** supports ARM64 as a *host*. Install VS 2022 (ARM64) + that WDK
   and build — host and target are both ARM64, no cross-compile, no emulation.
2. **x64 EWDK/WDK under emulation.** If you only have an *x64* EWDK ISO or an
   older x64-only WDK, Windows 11 on ARM runs x64 binaries via built-in
   emulation, so `msbuild` / `cl.exe` / `LaunchBuildEnv.cmd` run fine. They
   still emit **ARM64** `.sys` output (you're cross-compiling x64-host → ARM64).
   Slower to build, identical result.

| Option | Best for | Notes |
|--------|----------|-------|
| **VS 2022 (ARM64) + WDK 26100** | your case (ARM64-only) | native, fastest; get the "ARM64 + ARM64EC" and Spectre-mitigated ARM64 lib components |
| **EWDK ISO** | CI / headless / no install | native ARM64 EWDK if available, else the x64 ISO runs under emulation |

So: do everything on your ARM64 Windows box. The RK3576 board is only needed to
*run/test* the driver. If your ARM64 Windows is a Parallels VM (Apple Silicon),
that works too — it's a normal ARM64 Windows install.

## Build with the EWDK

1. Download the **EWDK for Windows 11** ISO from Microsoft and mount/extract it.
2. From the EWDK root, start a build environment:
   ```
   LaunchBuildEnv.cmd
   ```
3. Build the GPIO driver:
   ```
   cd drivers\gpio\rk3576gpio
   build.cmd Release
   ```
   Output: `ARM64\Release\rk3576gpio\rk3576gpio.sys` + `.inf` + `.cat`.

## Build with Visual Studio + WDK

If `msbuild` cannot locate the WDK targets from the bare `.vcxproj`, create the
project from the IDE template (this guarantees the right import props for your
WDK version):

1. Install Visual Studio 2022 + the matching **WDK** (and "Spectre-mitigated
   ARM64 libs" component).
2. New Project → **Kernel Mode Driver, Empty (KMDF)**.
3. Add the existing files: `driver.c`, `controller.c`, `rk3576gpio.h`,
   `rk3576gpio_regs.h`, `..\..\inc\rk3576_soc.h`, `rk3576gpio.inf`.
4. Project properties → set **Configuration = Release**, **Platform = ARM64**.
5. Linker → Input → Additional Dependencies: add `gpioclx.lib`.
6. Build.

> The committed `rk3576gpio.vcxproj` mirrors this template; use it directly if
> your `msbuild` already resolves `WindowsKernelModeDriver10.0`.

## A note on the GpioClx ABI

The driver `#include <gpioclx.h>` and links `gpioclx.lib` — it does **not**
redefine any GpioClx type. The `CLIENT_*` callback signatures, the
`GPIO_*_PARAMETERS` struct field names, and the `GPIO_CLX_*` helper prototypes
all come from the WDK header. They are stable across recent WDKs; any name that
differs surfaces as a compile error at the exact line, and these are being
settled as the drivers are built up (WIP).

## Test-signing & installing on the board

WOA in dev needs test-signing enabled or an unlocked/insecure boot policy.

1. Generate a test certificate (once) and test-sign the catalog **on the build
   host**:
   ```
   makecert -r -pe -ss PrivateCertStore -n "CN=WOA-RK3576-Test" testcert.cer
   inf2cat /driver:. /os:10_VB_ARM64
   signtool sign /v /s PrivateCertStore /n "WOA-RK3576-Test" ^
       /fd sha256 /t http://timestamp.digicert.com rk3576gpio.cat
   ```
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

- Kernel debug over serial (RK3576 UART2 @ **1,500,000 8N1**) or KDNET if the
  GMAC/USB path is up.
- The driver logs via `DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...)`. Enable it in the
  debugger:
  ```
  ed nt!Kd_IHVDRIVER_Mask 0xF
  ```
