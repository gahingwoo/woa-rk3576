# pageverify

[English](README.md) | 简体中文

找出文件中装着别人数据的 4 KiB 页。

工具写入的每一页都带有一个魔数、种子、它自己的页号和一个自检值，后面是 4064 字节由种子和页号生成的 splitmix64 数据流。
校验时重新生成每一页并比较。对每个坏页，它打印文件偏移、有多少字节不同、第一个和最后一个不同的字节，并说明这一页里装的是什么：

- `misplaced page of THIS file`：同一个文件的另一页（文件内部的陈旧或错位 I/O）；页头会指出它来自哪一页
- `page of ANOTHER pageverify file`：来自之前某个测试文件的一页
- `all zero`、`all 0xFF`、`starts with MZ`
- `looks like ARM64 code`：统计 pacibsp、autibsp、ret、nop 和建立栈帧的指令字
- `unknown foreign data`

加上 `--dump DIR` 时，每个坏页都会保存成一个 `.bin` 文件，以后可以拿去 Windows 二进制文件里搜索。

## 为什么要做这个

2026-11-23，一个 1.3 GB 的文件通过 scp 复制到泰山派 3M eMMC 上的 R: 后，有一个 4 KiB 页被换成了
`Windows.Security.Authentication.Web.Core.dll`（10.0.22621.2070，RVA 0x12e000，文件偏移 0x12c600）的一页，逐字节相同。
这个工具让这类事件可以计数，这样比较各种改动时看的是发生率，而不是个别经历。

## 构建

- Windows ARM64：`msbuild tools\pageverify\pageverify.vcxproj /p:Configuration=Release /p:Platform=ARM64`。CI 上传的产物名为
  `rk3576-woa-pageverify-arm64`。
- Linux（在电脑上生成测试文件用）：`gcc -std=c11 -O2 -o pageverify tools/pageverify/pageverify.c`。CI 上传的产物名为
  `pageverify-linux-x64`。

两种构建生成的数据完全相同；`selftest` 会检查数据流的一个已知值。

## 使用

```
pageverify write  FILE MiB  [--seed HEX] [--direct] [--chunk KiB]
pageverify verify FILE      [--direct] [--passes N] [--dump DIR]
pageverify loop   DIR  MiB  [--rounds N] [--continue] [--keep] [--direct]
pageverify selftest
```

`--log FILE` 会把所有输出同时追加到日志文件。`verify` 从第 0 页读取种子。

- 在 `write` 之后马上做带缓存的 `verify`，读到的大多是文件缓存。
- `--direct` 读取会绕过缓存，直接从磁盘读（NTFS 会先把缓存中的脏页写出去）。`--direct` 写入不经缓冲且直写，
  所以 DMA 直接从工具自己的缓冲区出发。
- 如果 `--direct --passes 3` 每一遍报出的坏页都不同，损坏发生在读取路径上。如果每次都是同样的页，它们就在磁盘上。

`loop` 写一个文件，带缓存校验一遍，再不经缓冲校验一遍，然后删除它。遇到第一个坏的轮次就停下并保留那个文件，
除非你加了 `--continue`，这时会删除文件并继续计数。

退出码：0 表示干净，1 表示发现坏页，2 表示用法或 I/O 错误。

## 计数

1.3 GB 里出一个坏页是罕见事件。除非跑得够久，否则一次干净的运行说明不了什么：

- 先在没改动的配置上至少复现三次故障，记下这用了多少 GiB（V）。
- 然后只有当改动后至少 3 x V 都保持干净，才算修好。在三倍数据量里零事件，使得"发生率没变"不太可能
  （大约 90 % 的置信度，已考虑到三次事件对发生率的估计很粗）。
