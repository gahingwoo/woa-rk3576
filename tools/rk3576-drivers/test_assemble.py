#!/usr/bin/env python3
# test_assemble.py - assemble.py against made-up inputs (no Windows needed).
#
#   python3 -I tools/rk3576-drivers/test_assemble.py
#
# Checks that the real drivers.tsv puts audio and the null driver on every
# board, that the board lists and the manifest agree, that the online
# install scripts are in the package, and that a missing .sys fails the
# build instead of leaving a driver out.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ASSEMBLE = os.path.join(HERE, 'assemble.py')
TABLE = os.path.join(HERE, 'drivers.tsv')
NULLS = ('malivop', 'rk3576-null')
# Every board needs these; the 2026-11-26 ROCK 4D run had no audio because a
# fallback dropped it silently.
EVERY_BOARD = ('dwcsdhc', 'dwcmshc', 'dwc_eqos', 'maliwddm', 'vdpu383',
               'rk3xi2sbus', 'csaudiork3x', 'es8323', 'rk3576-null')


def table_rows():
    return [s.split(None, 4) for s in lines(TABLE) if not s.startswith('#')]


def lines(path):
    with open(path, encoding='utf-8') as f:
        return [l.strip() for l in f if l.strip()]


def touch(path, data=b'x'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def make_inputs(top, skip_sys=None):
    woa = os.path.join(top, 'woa')
    gpu = os.path.join(top, 'gpu')
    for d, _, _, frm, _ in table_rows():
        if frm == 'woa':
            p = os.path.join(woa, 'woa-drivers', d)
            touch(os.path.join(p, d + '.inf'))
            touch(os.path.join(p, d + '.cat'))
            touch(os.path.join(p, d + '.pdb'))  # must stay out
            if d not in NULLS and d != skip_sys:
                touch(os.path.join(p, d + '.sys'))
    touch(os.path.join(woa, 'woa-drivers', 'signer.cer'))
    for n in ('maliwddm.inf', 'maliwddm.cat', 'maliwddm.sys', 'maliumd.dll',
              'malivop.inf', 'malivop.cat'):
        touch(os.path.join(gpu, 'driver', n))
    touch(os.path.join(gpu, 'wddm-mali-g52-test.cer'))
    npu = os.path.join(top, 'npu.zip')
    with zipfile.ZipFile(npu, 'w') as z:
        for n in ('rknpu/rknpu.inf', 'rknpu/rknpu.cat', 'rknpu/rknpu.sys', 'signer.cer'):
            z.writestr(n, b'x')
    return woa, gpu, npu


def run(top, woa, gpu, npu):
    out = os.path.join(top, 'out')
    cmd = [sys.executable, '-I', ASSEMBLE, '--table', TABLE,
           '--install', os.path.join(HERE, 'install.cmd'),
           '--script', os.path.join(HERE, 'install-online.ps1'),
           '--script', os.path.join(HERE, 'mark-reinstall.ps1'),
           '--readme', os.path.join(HERE, 'README.md'),
           '--woa', woa, '--gpu', gpu, '--npu', npu,
           '--version', 'ci-test', '--source', 'package=test', '--out', out]
    return subprocess.run(cmd, capture_output=True, text=True), out


class Assemble(unittest.TestCase):
    def test_package(self):
        with tempfile.TemporaryDirectory() as top:
            r, out = run(top, *make_inputs(top))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            root = os.path.join(out, 'rk3576-drivers')
            with open(os.path.join(root, 'manifest.json')) as f:
                m = json.load(f)
            for b in m['boards']:
                want = [d['dir'] for d in m['drivers'] if b in d['default']]
                got = lines(os.path.join(root, 'boards', b + '.txt'))
                self.assertEqual(want, got, b)
                for d in EVERY_BOARD:
                    self.assertIn(d, got, f'{b} has no {d}')
            for f in ('install.cmd', 'install-online.ps1', 'mark-reinstall.ps1', 'VERSION.txt', 'SHA256SUMS.txt'):
                self.assertTrue(os.path.isfile(os.path.join(root, f)), f)
            self.assertTrue(os.path.isfile(os.path.join(root, 'drivers', 'rk3576-null', 'rk3576-null.inf')))
            self.assertFalse(os.path.exists(os.path.join(root, 'drivers', 'rk3576-null', 'rk3576-null.pdb')))
            with zipfile.ZipFile(os.path.join(out, 'rk3576-drivers-arm64.zip')) as z:
                names = z.namelist()
            self.assertIn('rk3576-drivers/install-online.ps1', names)
            self.assertIn('rk3576-drivers/drivers/csaudiork3x/csaudiork3x.sys', names)

    def test_missing_sys_fails(self):
        with tempfile.TemporaryDirectory() as top:
            r, _ = run(top, *make_inputs(top, skip_sys='es8323'))
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('es8323: no .sys', r.stdout)

    def test_null_inf_names_amzn0001(self):
        with open(os.path.join(HERE, '..', '..', 'drivers', 'null', 'rk3576-null.inf'), encoding='ascii') as f:
            inf = f.read()
        for hid in ('ACPI\\HYMB0001', 'ACPI\\RKCP0806', 'ACPI\\RKCP3005', 'ACPI\\HISI0031', 'ACPI\\AMZN0001'):
            self.assertIn('= Null_Inst, ' + hid + '\n', inf)


if __name__ == '__main__':
    unittest.main(verbosity=2)
