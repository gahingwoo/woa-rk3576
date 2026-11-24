# pageverify

English | [简体中文](README.zh-CN.md)

Finds 4 KiB pages of a file that hold someone else's data.

Every page the tool writes carries a magic, the seed, its own page index and a
self-check, followed by 4064 bytes of a splitmix64 stream keyed by seed and
index. A verify pass regenerates each page and compares it. For every bad page
it prints the file offset, how many bytes differ and the first and last
differing byte, and it says what the page holds instead:

- `misplaced page of THIS file`: another page of the same file (stale or
  misdirected I/O inside the file); the header names the page it came from
- `page of ANOTHER pageverify file`: a page from an earlier test file
- `all zero`, `all 0xFF`, `starts with MZ`
- `looks like ARM64 code`: counts pacibsp, autibsp, ret, nop and frame-setup
  words
- `unknown foreign data`

With `--dump DIR` each bad page is saved as a `.bin` file, so it can be
searched for in Windows binaries later.

## Why

On 2026-11-23 a 1.3 GB file copied over scp to R: on the TaishanPi 3M eMMC
had one 4 KiB page replaced by a page of
`Windows.Security.Authentication.Web.Core.dll` (10.0.22621.2070, RVA
0x12e000, file offset 0x12c600, byte identical). This tool makes that kind of
event countable, so changes can be compared by rate instead of by anecdote.

## Build

- Windows ARM64: `msbuild tools\pageverify\pageverify.vcxproj
  /p:Configuration=Release /p:Platform=ARM64`. CI uploads it as
  `rk3576-woa-pageverify-arm64`.
- Linux (for making pattern files on the PC): `gcc -std=c11 -O2 -o
  pageverify tools/pageverify/pageverify.c`. CI uploads it as
  `pageverify-linux-x64`.

The pattern is the same on both builds; `selftest` checks a known value of
the stream.

## Use

```
pageverify write  FILE MiB  [--seed HEX] [--direct] [--chunk KiB]
pageverify verify FILE      [--direct] [--passes N] [--dump DIR]
pageverify loop   DIR  MiB  [--rounds N] [--continue] [--keep] [--direct]
pageverify selftest
```

`--log FILE` appends everything to a log file too. `verify` reads the seed
from page 0.

- Cached `verify` right after `write` mostly reads the file cache.
- `--direct` reads bypass the cache and come from the disk (NTFS writes
  any dirty cached pages first). `--direct` writes are unbuffered and
  write-through, so the DMA goes straight from the tool's own buffer.
- If `--direct --passes 3` gives different bad pages on each pass, the
  damage happens on the read path. If it gives the same pages every time,
  they are on the disk.

`loop` writes a file, verifies it cached, verifies it again unbuffered, then
deletes it. It stops at the first bad round and keeps that file, unless you
pass `--continue`, which deletes the file and goes on counting.

Exit code: 0 clean, 1 bad pages found, 2 usage or I/O error.

## Counting

One bad page in 1.3 GB is a rare event. A clean run proves little unless it is
long:

- First reproduce the fault at least three times on the unchanged setup, and
  note how many GiB that took (V).
- Then call a change a fix only if it stays clean for at least 3 x V. Zero
  events in three times the volume makes "the rate did not change" unlikely
  (roughly 90 % confidence, allowing for how loosely three events pin down
  a rate).
