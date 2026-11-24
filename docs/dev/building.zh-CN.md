# 构建和安装驱动

[English](building.md) | 简体中文

这些是 Windows ARM64 内核驱动。它们在 Windows 上用 WDK 构建，或者在 CI 里构建，不能在 Linux 上构建。

## 用 CI 构建

- [ci.yml](../../.github/workflows/ci.yml) 在每次 push 时用 WDK 10.0.26100 构建 `build/RockchipDrivers.sln`、
  `rk3xspi`、`gt9271`、`bcm4345bt` 和 `tools/pageverify`，任何错误都会让它失败。它产出的驱动包没有签名。
- [winpe.yml](../../.github/workflows/winpe.yml) 构建同样的驱动，用一次性的证书做测试签名，注入 ADK 22621 的
  WinPE，然后上传映像。已签名的驱动包在映像的 `woa-drivers\` 文件夹里。要安装的就是这一套。

## 本地构建

ARM64 的 Windows 电脑可以直接本机构建。安装 Visual Studio 2022（带 ARM64 构建工具和 Spectre 缓解的 ARM64 库）
以及 WDK 10.0.26100。

```cmd
msbuild build\RockchipDrivers.sln /p:Configuration=Release /p:Platform=ARM64
msbuild drivers\spi\rk3xspi\rk3xspi.vcxproj /p:Configuration=Release /p:Platform=ARM64
msbuild drivers\input\gt9271\gt9271.vcxproj /p:Configuration=Release /p:Platform=ARM64
msbuild drivers\bluetooth\bcm4345bt\bcm4345bt.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

驱动包输出到 `build\ARM64\Release\Output\<driver>\`。按 `winpe.yml` 的做法签名：先 `inf2cat /os:10_RS3_ARM64`，
再用 `signtool sign` 给 `.cat` 和 `.sys` 签名。

## 安装

板子需要打开测试签名：`bcdedit /set testsigning on`，然后重启。这样，证书链不到受信任根证书的签名也会被接受；
但没有签名的驱动仍然不行。

- 新安装：`tools/woa-deploy/deploy-windows.cmd` 会注入驱动。见[安装 Windows](../INSTALL.zh-CN.md)。
- 已有的系统，在 WinPE 下离线安装：`tools/woa-deploy/add-drivers.cmd`。
- 已有的系统，在线安装单个驱动：见[驱动](../drivers.zh-CN.md)。

## 没有 WDK 时查看头文件

[wdk-headers](../../.github/workflows/wdk-headers.yml) 工作流把 runner 上的
`Include\10.0.26100.0\{km,shared}` 目录导出为 artifact，就是 CI 编译时用的那套头文件。要找某个标识符，在里面 grep，
不要靠猜。

## 不明显的项目设置

下面每一条都先造成过一次构建失败。

| 设置 | 原因 |
|---|---|
| `MARMASM`，并显式导入 `marmasm.props`/`.targets` | ARM64 工具集没有 masm 的构建自定义；`<MASM>` 项会被悄悄忽略。 |
| `$(SPB_INC_PATH)\$(SPB_VERSION_MAJOR).$(SPB_VERSION_MINOR)` 和 `SpbCxStubs.lib` | `spbcx.h` 不在 `km` 里，也没有 `spbcx.lib`。 |
| `msgpioclxstub.lib` 和 `ksguid.lib` | 没有 `gpioclx.lib`。 |
| `NetAdapterDriver=true` 和 `NETADAPTER_VERSION_*` | 这样才会加入 NetCx 的 include 路径和导入库。只写 `netadaptercx.lib` 没有用。 |
| `KMDF_VERSION_*` 放在 `Label="Configuration"` 组里 | 如果在 `Microsoft.Cpp.props` 之后才声明，就读得太晚：构建用的是 KMDF 1.15，而 stampinf 往 INF 里写的是 1.33。 |
| `Inf2CatWindowsVersionList` 从 `10_RS3_ARM64` 开始 | ARM64 的默认值 `Server10_ARM64`（14393）比这些 INF 为 DIRID 13 所需的 16299 更旧。没有包含 `10_GE_ARM64`（24H2），因为 24H2 无法在 RK3576 上运行。 |
| `FilesToPackage` | 没有它，驱动包里只有 INF，inf2cat 会报 `22.9.1 ... .sys is missing` 失败。 |
| INF 的 models 加上 `NTARM64.10.0...16299` 修饰 | DIRID 13 要求这样；不加修饰时 InfVerif 报错误 1199。 |

`_KERNEL_MODE` 已由 WDK 工具集预定义。再定义一次会产生 C4117，而 `/WX` 会让每个文件都把它当成错误。

## 一次 push 会运行什么

`ci.yml` 在每次 push 时运行。`winpe.yml` 在 push 改动了
`tools/woa-debug/**`、`tools/woa-deploy/**`、`drivers/bluetooth/**` 或这个工作流本身时运行。

在 `main` 上，两者都把结果放到滚动的 `ci` 预发布版：`winpe.yml` 放 WinPE 映像，`ci.yml` 放 RK3576 工具包
`rk3576-tools-arm64.zip`。两者都经过 `.github/scripts/ci-release.sh`：它先上传新文件，保留另一个工作流的文件和它们在
`SHA256SUMS.txt` 里的行，移动 tag，最后才删除被替换的文件。它从不删除整个 release，所以某一步失败时，上一次的构建仍然留着。
