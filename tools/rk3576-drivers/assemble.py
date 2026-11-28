#!/usr/bin/env python3
# assemble.py - put the RK3576 Windows driver package together.
#
#   assemble.py --table drivers.tsv --install install.cmd --script F ...
#               --woa DIR --gpu DIR --npu ZIP
#               --version V --source KEY=TEXT ... --out DIR
#
# --woa  build-woa.ps1's output: woa-drivers\<dir>\ and woa-drivers\signer.cer
# --gpu  wddm-mali-g52's wddm-mali-g52-driver-arm64 artifact (driver\, *.cer)
# --npu  woa-rk3576-npu's rknpu-windows-arm64.zip
#
# Writes DIR/rk3576-drivers/ and DIR/rk3576-drivers-arm64.zip (one folder,
# rk3576-drivers\, entries sorted, fixed timestamps), and prints the list.
# Every driver of drivers.tsv must be found with an .inf, a .cat and, except
# for a null driver (no service), a .sys; anything missing fails the build.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import hashlib
import json
import os
import shutil
import sys
import zipfile

BOARDS = ('tspi3m', 'cm5io', 'rock4d')
FROM = ('woa', 'gpu', 'npu')
# Files a driver folder may carry; symbols and documentation stay out.
DRIVER_FILES = ('.inf', '.cat', '.sys', '.dll', '.man')
NULL_DRIVERS = ('malivop', 'rk3576-null')
# drivers.tsv's os column: which Windows builds a driver is for.
#   win10       builds below 22000 (Windows 10 is 19041 to 19045 here)
#   win11       build 22000 and later
#   N-          build N and later (22621-: Windows 11 22H2 and later)
#   N-M         builds N to M
# Any of them may end in :<dir>, "this is the build of <dir> for these
# builds": install.cmd then says nothing about <dir> being left out there.
# Left out, every Windows.
BUILD_MAX = 99999
OS_NAMED = {'all': (0, BUILD_MAX), 'win10': (0, 21999), 'win11': (22000, BUILD_MAX)}
WIN10_BUILD = 19045   # the Windows 10 the -win10 lists are for
WIN11_BUILD = 22000   # the first Windows 11: the plain lists


def os_range(tok):
    """(min, max, stands_in_for) of an os token, or None if it is not one."""
    base, _, of = tok.partition(':')
    if base in OS_NAMED:
        lo, hi = OS_NAMED[base]
    else:
        a, dash, b = base.partition('-')
        if not dash or not a.isdigit() or (b and not b.isdigit()):
            return None
        lo, hi = int(a), int(b) if b else BUILD_MAX
        if lo > hi:
            return None
    return lo, hi, of or None


def covers(r, build):
    return r['builds'][0] <= build <= r['builds'][1]


def gaps(rows):
    """Per driver limited to builds N and later: the builds that neither it
    nor a stand-in (a row with :<dir>) covers, as (row, lo, hi)."""
    out = []
    for r in rows:
        # Only a driver for the newest Windows is missing anywhere: one for
        # older builds only (a workaround) is not, above its range.
        if r['of'] or tuple(r['builds']) == (0, BUILD_MAX) or r['builds'][1] != BUILD_MAX:
            continue
        spans = sorted(tuple(x['builds']) for x in rows if x is r or x['of'] == r['dir'])
        at = 0
        for lo, hi in spans:
            if lo > at:
                out.append((r, at, lo - 1))
            at = max(at, hi + 1)
        if at <= BUILD_MAX:
            out.append((r, at, BUILD_MAX))
    return out


def boards(field, line):
    if field == '-':
        return []
    if field == 'all':
        return list(BOARDS)
    out = field.split(',')
    for b in out:
        if b not in BOARDS:
            sys.exit(f'drivers.tsv line {line}: unknown board {b}')
    return out


def read_table(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        for n, raw in enumerate(f, 1):
            s = raw.strip()
            if not s or s.startswith('#'):
                continue
            parts = s.split(None, 4)
            if len(parts) != 5:
                sys.exit(f'drivers.tsv line {n}: want 5 columns, got {len(parts)}')
            d, dflt, opt, frm, what = parts
            # The optional sixth column, before "what": which Windows builds
            # the driver is for (OS_NAMED, N-, N-M, :<dir>). Rows without it
            # are for every Windows.
            osv, rng = 'all', OS_NAMED['all'] + (None,)
            first, _, rest = what.partition(' ')
            if rest.strip() and os_range(first):
                osv, rng, what = first, os_range(first), rest.strip()
            if frm not in FROM:
                sys.exit(f'drivers.tsv line {n}: unknown source {frm}')
            de, op = boards(dflt, n), boards(opt, n)
            if set(de) & set(op):
                sys.exit(f'drivers.tsv line {n}: a board is both default and optional')
            rows.append({'dir': d, 'what': what, 'from': frm, 'default': de, 'optional': op,
                         'os': osv, 'builds': [rng[0], rng[1]], 'of': rng[2]})
    names = [r['dir'] for r in rows]
    if len(set(names)) != len(names):
        sys.exit('drivers.tsv names a folder twice')
    for r in rows:
        if r['of'] and r['of'] not in names:
            sys.exit(f"drivers.tsv: {r['dir']} is the build of {r['of']}, which is not in the table")
    return rows


def inf_source_files(inf):
    """The file names an INF's [SourceDisksFiles*] sections list."""
    raw = open(inf, 'rb').read()
    enc = 'utf-16' if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else 'latin-1'
    out, sec = set(), ''
    for line in raw.decode(enc).splitlines():
        line = line.split(';', 1)[0].strip()
        if line.startswith('[') and line.endswith(']'):
            sec = line[1:-1].strip().lower()
        elif sec.startswith('sourcedisksfiles') and '=' in line:
            out.add(line.split('=', 1)[0].strip())
    return out


def complete_driver(src_dir, dst):
    """Copy what the folder's INFs list but the extension filter left out
    (malivk.json, the Vulkan ICD manifest, was the first); return the names
    still missing. pnputil refuses a package with any of them missing."""
    missing = []
    have = {f.lower() for f in os.listdir(dst)}
    for inf in sorted(f for f in os.listdir(dst) if f.lower().endswith('.inf')):
        for name in sorted(inf_source_files(os.path.join(dst, inf))):
            if name.lower() in have:
                continue
            p = os.path.join(src_dir, name)
            if os.path.isfile(p):
                shutil.copy2(p, os.path.join(dst, name))
                have.add(name.lower())
            else:
                missing.append(f'{inf} lists {name}')
    return missing


def copy_driver(src_dir, dst, only=None, skip=None):
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in sorted(os.listdir(src_dir)):
        p = os.path.join(src_dir, name)
        if not os.path.isfile(p) or not name.lower().endswith(DRIVER_FILES):
            continue
        stem = name.rsplit('.', 1)[0].lower()
        if only is not None and stem not in only:
            continue
        if skip is not None and stem in skip:
            continue
        shutil.copy2(p, os.path.join(dst, name))
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--table', required=True)
    ap.add_argument('--install', required=True, help='install.cmd')
    ap.add_argument('--readme', action='append', default=[])
    ap.add_argument('--script', action='append', default=[],
                    help='a file for the package root next to install.cmd (install-online.ps1, mark-reinstall.ps1)')
    ap.add_argument('--woa', required=True)
    ap.add_argument('--gpu', required=True)
    ap.add_argument('--npu', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--source', action='append', default=[], help='key=text, into manifest.json')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    rows = read_table(a.table)
    root = os.path.join(a.out, 'rk3576-drivers')
    if os.path.exists(root):
        shutil.rmtree(root)
    drv = os.path.join(root, 'drivers')
    os.makedirs(drv)
    certs = os.path.join(root, 'certs')
    os.makedirs(certs)

    woa = os.path.join(a.woa, 'woa-drivers')
    gpu = os.path.join(a.gpu, 'driver')
    npu_tmp = os.path.join(a.out, '.npu')
    if os.path.exists(npu_tmp):
        shutil.rmtree(npu_tmp)
    with zipfile.ZipFile(a.npu) as z:
        for info in z.infolist():
            parts = info.filename.replace('\\', '/').split('/')
            if '..' in parts or info.filename.startswith('/'):
                sys.exit(f'{a.npu}: unsafe name {info.filename}')
        z.extractall(npu_tmp)
    npu = os.path.join(npu_tmp, 'rknpu')

    problems = []
    for r in rows:
        d, dst = r['dir'], os.path.join(drv, r['dir'])
        if r['from'] == 'woa':
            src = os.path.join(woa, d)
        elif r['from'] == 'npu':
            src = npu
        else:
            src = gpu
        if not os.path.isdir(src):
            problems.append(f'{d}: no folder {src}')
            continue
        if r['from'] == 'gpu':
            n = copy_driver(src, dst, only=('malivop',)) if d == 'malivop' else copy_driver(src, dst, skip=('malivop',))
        else:
            n = copy_driver(src, dst)
        if n:
            problems += [f'{d}: {m}, not in {src}' for m in complete_driver(src, dst)]
        files = [f.lower() for f in os.listdir(dst)] if n else []
        need = ['.inf', '.cat'] + ([] if d in NULL_DRIVERS else ['.sys'])
        for ext in need:
            if not any(f.endswith(ext) for f in files):
                problems.append(f'{d}: no {ext} in {src}')
        r['infs'] = sorted(f for f in os.listdir(dst) if f.lower().endswith('.inf')) if n else []
    if problems:
        for p in problems:
            print(f'::error::{p}')
        sys.exit(1)

    # The signers, for trusting the packages on a running Windows (install.cmd /online).
    for name, src in (('woa-rk3576.cer', os.path.join(woa, 'signer.cer')),
                      ('wddm-mali-g52.cer', os.path.join(a.gpu, 'wddm-mali-g52-test.cer')),
                      ('woa-rk3576-npu.cer', os.path.join(npu_tmp, 'signer.cer'))):
        if not os.path.isfile(src):
            sys.exit(f'no signer certificate {src}')
        shutil.copy2(src, os.path.join(certs, name))

    # boards\<board>-builds.txt (and -builds-optional.txt): each driver with
    # the builds it is for, "dir<TAB>min<TAB>max"; install.cmd and
    # install-online.ps1 take the lines that cover the target's build.
    # skipped.txt: "dir<TAB>min<TAB>max<TAB>boards<TAB>what", the builds a
    # driver has no build for (not itself, not a stand-in), to say so.
    # boards\<board>.txt is the list for Windows 11 (build 22000) as before
    # the os column, boards\<board>-win10.txt the one for 19045, and
    # win10-skipped.txt what 19045 goes without, for readers of those.
    os.makedirs(os.path.join(root, 'boards'))
    bd = os.path.join(root, 'boards')
    for b in BOARDS:
        for kind, key in (('', 'default'), ('-optional', 'optional')):
            mine = [r for r in rows if b in r[key]]
            with open(os.path.join(bd, f'{b}-builds{kind}.txt'), 'w', newline='\r\n') as f:
                for r in mine:
                    f.write(f"{r['dir']}\t{r['builds'][0]}\t{r['builds'][1]}\n")
            for osk, build in (('', WIN11_BUILD), ('-win10', WIN10_BUILD)):
                with open(os.path.join(bd, f'{b}{osk}{kind}.txt'), 'w', newline='\r\n') as f:
                    for r in mine:
                        if covers(r, build):
                            f.write(r['dir'] + '\n')
    missing = gaps(rows)
    with open(os.path.join(bd, 'skipped.txt'), 'w', newline='\r\n') as f:
        for r, lo, hi in missing:
            f.write(f"{r['dir']}\t{lo}\t{hi}\t{','.join(r['default'] + r['optional'])}\t{r['what']}\n")
    with open(os.path.join(bd, 'win10-skipped.txt'), 'w', newline='\r\n') as f:
        for r, lo, hi in missing:
            if lo <= WIN10_BUILD <= hi:
                f.write(f"{r['dir']}\t{','.join(r['default'] + r['optional'])}\t{r['what']}\n")

    sources = {}
    for s in a.source:
        k, _, v = s.partition('=')
        sources[k] = v
    manifest = {
        'format': 1,
        'name': 'RK3576 Windows driver package',
        'version': a.version,
        'boards': list(BOARDS),
        'sources': sources,
        'drivers': [{k: r[k] for k in ('dir', 'what', 'from', 'default', 'optional', 'os', 'builds', 'of', 'infs')} for r in rows],
    }
    with open(os.path.join(root, 'manifest.json'), 'w', newline='\n') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')
    with open(os.path.join(root, 'VERSION.txt'), 'w', newline='\r\n') as f:
        f.write(a.version + '\n')
    shutil.copy2(a.install, os.path.join(root, 'install.cmd'))
    for rd in a.readme + a.script:
        shutil.copy2(rd, os.path.join(root, os.path.basename(rd)))
    shutil.rmtree(npu_tmp)

    # Inside the package too: every file's SHA-256, for a check after copying it around.
    entries = []
    for dp, _, fs in os.walk(root):
        for fn in fs:
            p = os.path.join(dp, fn)
            entries.append(os.path.relpath(p, a.out).replace(os.sep, '/'))
    entries.sort()
    lines = []
    for e in entries:
        with open(os.path.join(a.out, e), 'rb') as f:
            lines.append(f'{hashlib.sha256(f.read()).hexdigest()}  {e[len("rk3576-drivers/"):]}')
    with open(os.path.join(root, 'SHA256SUMS.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    entries = sorted(entries + ['rk3576-drivers/SHA256SUMS.txt'])

    zpath = os.path.join(a.out, 'rk3576-drivers-arm64.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for e in entries:
            info = zipfile.ZipInfo(e, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(a.out, e), 'rb') as f:
                z.writestr(info, f.read())
    for r in rows:
        print(f"{r['dir']:<12} default={','.join(r['default']) or '-':<20} optional={','.join(r['optional']) or '-':<8} {' '.join(r['infs'])}")
    print(f'{zpath}: {len(entries)} files, {os.path.getsize(zpath)} bytes')


if __name__ == '__main__':
    main()
