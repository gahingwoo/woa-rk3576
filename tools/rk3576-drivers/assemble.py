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
            if frm not in FROM:
                sys.exit(f'drivers.tsv line {n}: unknown source {frm}')
            de, op = boards(dflt, n), boards(opt, n)
            if set(de) & set(op):
                sys.exit(f'drivers.tsv line {n}: a board is both default and optional')
            rows.append({'dir': d, 'what': what, 'from': frm, 'default': de, 'optional': op})
    names = [r['dir'] for r in rows]
    if len(set(names)) != len(names):
        sys.exit('drivers.tsv names a folder twice')
    return rows


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

    os.makedirs(os.path.join(root, 'boards'))
    for b in BOARDS:
        for kind, key in (('', 'default'), ('-optional', 'optional')):
            with open(os.path.join(root, 'boards', f'{b}{kind}.txt'), 'w', newline='\r\n') as f:
                for r in rows:
                    if b in r[key]:
                        f.write(r['dir'] + '\n')

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
        'drivers': [{k: r[k] for k in ('dir', 'what', 'from', 'default', 'optional', 'infs')} for r in rows],
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
