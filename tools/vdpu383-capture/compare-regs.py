#!/usr/bin/env python3
#
# compare-regs.py - diff the register file Linux programmed for one picture
# (capture-linux.sh: regs-pic-NNNN.txt) against the one vdpu383test built for
# the same picture (vdpu383test decode CLIP -n N --regs, the "reg" lines of
# "# picture N-1").
#
#   compare-regs.py LINUX/regs-pic-0003.txt windows-regs.txt [--picture 2]
#
# Buffer addresses cannot match (IOMMU addresses on Linux, physical on
# Windows), so address registers are compared only as zero / non-zero.
# Registers Linux never writes (mainline leaves the statistics registers
# 28-30 and the CABAC error masks 20-21 alone) show up as differences too;
# that is expected, and is listed separately.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only
#
import re
import sys

ADDR = set(range(128, 135)) | {r for r in range(140, 161, 2)} | set(range(168, 186)) | \
       {192} | set(range(194, 211)) | set(range(216, 233))
NOT_WRITTEN_BY_MAINLINE = {9, 20, 21, 28, 29, 30, 80, 81, 82}


def read_linux(path):
    regs = {}
    for line in open(path):
        m = re.match(r"0x([0-9a-f]{8}) 0x([0-9a-f]{8})", line)
        if not m:
            continue
        addr, val = int(m.group(1), 16), int(m.group(2), 16)
        if 0x27b00100 <= addr < 0x27b00700:
            regs[(addr - 0x27b00100) // 4] = val
    return regs


def read_windows(path, picture):
    regs, cur = {}, None
    for line in open(path):
        m = re.match(r"# picture (\d+) registers", line)
        if m:
            cur = int(m.group(1))
            continue
        m = re.match(r"reg\s+(\d+) 0x([0-9a-f]{8})", line)
        if m and (picture is None or cur == picture):
            regs[int(m.group(1))] = int(m.group(2), 16)
    return regs


def main():
    args = sys.argv[1:]
    picture = None
    if "--picture" in args:
        i = args.index("--picture")
        picture = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__ or "usage: compare-regs.py LINUX WINDOWS [--picture N]")
    lin, win = read_linux(args[0]), read_windows(args[1], picture)
    if not lin or not win:
        sys.exit("no registers read (linux %d, windows %d)" % (len(lin), len(win)))
    diff, expected = [], []
    for r in sorted(set(win) | (set(lin) & set(range(8, 233)))):
        a, b = lin.get(r, 0), win.get(r, 0)
        if r in ADDR:
            if (a == 0) != (b == 0):
                diff.append("reg %3d  linux 0x%08x  windows 0x%08x  (address: zero vs non-zero)" % (r, a, b))
            continue
        if r == 15:
            continue    # status
        if a != b:
            line = "reg %3d  linux 0x%08x  windows 0x%08x" % (r, a, b)
            (expected if r in NOT_WRITTEN_BY_MAINLINE else diff).append(line)
    print("differences:" if diff else "no unexpected differences")
    for d in diff:
        print("  " + d)
    if expected:
        print("registers mainline does not program (MPP does):")
        for d in expected:
            print("  " + d)


if __name__ == "__main__":
    main()
