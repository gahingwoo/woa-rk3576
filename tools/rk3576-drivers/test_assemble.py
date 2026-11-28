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


def make_inputs(top, skip_sys=None, skip_gpu_file=None):
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
    for n in ('maliwddm.cat', 'maliwddm.sys', 'maliumd.dll', 'malivk.json',
              'malivop.inf', 'malivop.cat'):
        if n != skip_gpu_file:
            touch(os.path.join(gpu, 'driver', n))
    # An INF that lists a file the extension filter does not take (the Vulkan ICD manifest).
    touch(os.path.join(gpu, 'driver', 'maliwddm.inf'),
          b'[SourceDisksFiles]\r\nmaliwddm.sys = 1\r\nmaliumd.dll = 1 ; UMD\r\nmalivk.json = 1\r\n')
    touch(os.path.join(gpu, 'wddm-mali-g52-test.cer'))
    npu = os.path.join(top, 'npu.zip')
    with zipfile.ZipFile(npu, 'w') as z:
        for n in ('rknpu/rknpu.inf', 'rknpu/rknpu.cat', 'rknpu/rknpu.sys', 'signer.cer'):
            z.writestr(n, b'x')
    return woa, gpu, npu


def run(top, woa, gpu, npu, table=TABLE):
    out = os.path.join(top, 'out')
    cmd = [sys.executable, '-I', ASSEMBLE, '--table', table,
           '--install', os.path.join(HERE, 'install.cmd'),
           '--script', os.path.join(HERE, 'install-online.ps1'),
           '--script', os.path.join(HERE, 'mark-reinstall.ps1'),
           '--script', os.path.join(HERE, 'verify-bound.ps1'),
           '--script', os.path.join(HERE, 'win10-nvme.cmd'),
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
                for osk, build, eth in (('', 22000, 'dwc_eqos'), ('-win10', 19045, 'dwc_eqos10')):
                    want = [d['dir'] for d in m['drivers'] if b in d['default']
                            and d['builds'][0] <= build <= d['builds'][1]]
                    got = lines(os.path.join(root, 'boards', b + osk + '.txt'))
                    self.assertEqual(want, got, b + osk)
                    for d in EVERY_BOARD:
                        self.assertIn(eth if d == 'dwc_eqos' else d, got, f'{b}{osk} has no {d}')
            for f in ('install.cmd', 'install-online.ps1', 'mark-reinstall.ps1', 'verify-bound.ps1', 'win10-nvme.cmd', 'VERSION.txt', 'SHA256SUMS.txt'):
                self.assertTrue(os.path.isfile(os.path.join(root, f)), f)
            self.assertTrue(os.path.isfile(os.path.join(root, 'drivers', 'rk3576-null', 'rk3576-null.inf')))
            self.assertFalse(os.path.exists(os.path.join(root, 'drivers', 'rk3576-null', 'rk3576-null.pdb')))
            with zipfile.ZipFile(os.path.join(out, 'rk3576-drivers-arm64.zip')) as z:
                names = z.namelist()
            self.assertIn('rk3576-drivers/install-online.ps1', names)
            self.assertIn('rk3576-drivers/drivers/csaudiork3x/csaudiork3x.sys', names)

    def test_kmdf_for_windows_10(self):
        # A woa driver that Windows 10 gets must not ask for more than KMDF
        # 1.31; one for Windows 11 only may. The NPU's is only warned about.
        with tempfile.TemporaryDirectory() as top:
            woa, gpu, npu = make_inputs(top)
            touch(os.path.join(woa, 'woa-drivers', 'rkled', 'rkled.inf'),
                  b'[rkled_wdfsect]\r\nKmdfLibraryVersion = 1.33\r\n')
            r, _ = run(top, woa, gpu, npu)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('rkled: rkled.inf asks for KMDF 1.33', r.stdout)
        with tempfile.TemporaryDirectory() as top:
            woa, gpu, npu = make_inputs(top)
            touch(os.path.join(woa, 'woa-drivers', 'dwc_eqos', 'dwc_eqos.inf'),
                  b'KmdfLibraryVersion = 1.33\r\n')
            touch(os.path.join(woa, 'woa-drivers', 'rkled', 'rkled.inf'),
                  b'KmdfLibraryVersion = 1.31\r\n')
            with zipfile.ZipFile(npu, 'w') as z:
                z.writestr('rknpu/rknpu.inf', b'KmdfLibraryVersion = 1.33\r\n')
                for n in ('rknpu/rknpu.cat', 'rknpu/rknpu.sys', 'signer.cer'):
                    z.writestr(n, b'x')
            r, _ = run(top, woa, gpu, npu)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('::warning::rknpu: rknpu.inf asks for KMDF 1.33', r.stdout)

    def test_win10_lists(self):
        # The real table: Windows 10 takes dwc_eqos10 for dwc_eqos and adds
        # rk3xnvme (CM5-IO, ROCK 4D); nothing else differs and nothing is
        # skipped on either.
        with tempfile.TemporaryDirectory() as top:
            r, out = run(top, *make_inputs(top))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            bd = os.path.join(out, 'rk3576-drivers', 'boards')
            for b in ('tspi3m', 'cm5io', 'rock4d'):
                for kind in ('', '-optional'):
                    w11 = lines(os.path.join(bd, f'{b}{kind}.txt'))
                    w10 = lines(os.path.join(bd, f'{b}-win10{kind}.txt'))
                    self.assertNotIn('rk3xnvme', w11, b + kind)
                    nvme = kind == '' and b in ('cm5io', 'rock4d')
                    self.assertEqual('rk3xnvme' in w10, nvme, b + kind)
                    self.assertEqual([d for d in w11 if d != 'dwc_eqos'],
                                     [d for d in w10 if d not in ('dwc_eqos10', 'rk3xnvme')], b + kind)
                    if not kind:
                        self.assertIn('dwc_eqos', w11)
                        self.assertNotIn('dwc_eqos10', w11)
                        self.assertIn('dwc_eqos10', w10)
                        self.assertNotIn('dwc_eqos', w10)
                by = [l.split('\t') for l in lines(os.path.join(bd, f'{b}-builds.txt'))]
                self.assertIn(['dwc_eqos', '22000', '99999'], by)
                self.assertIn(['dwc_eqos10', '0', '21999'], by)
                self.assertIn(['dwcsdhc', '0', '99999'], by)
                if b in ('cm5io', 'rock4d'):
                    self.assertIn(['rk3xnvme', '19041', '21999'], by)
            self.assertEqual(lines(os.path.join(bd, 'win10-skipped.txt')), [])
            self.assertEqual(lines(os.path.join(bd, 'skipped.txt')), [])

    def test_os_column(self):
        # A Windows 11 only driver leaves the Windows 10 lists and is named in
        # the skipped lists; its Windows 10 build is only in the Windows 10
        # lists; a build range (26100-) and a stand-in (:<dir>) are honoured.
        with tempfile.TemporaryDirectory() as top:
            table = os.path.join(top, 'drivers.tsv')
            with open(TABLE, encoding='utf-8') as f:
                t = f.read()
            t = t.replace('rk3xspi      all            -         woa    SPI',
                          'rk3xspi      all            -         woa    win11 SPI')
            t = t.replace('rkled        all            -         woa    Board LEDs',
                          'rkled        all            -         woa    26100- Board LEDs')
            t += 'rk3xspi10    cm5io          -         woa    win10 SPI, Windows 10 build\n'
            t += 'rkled2      all            -         woa    22000-26099:rkled Board LEDs, 23H2 build\n'
            with open(table, 'w', encoding='utf-8') as f:
                f.write(t)
            woa, gpu, npu = make_inputs(top)
            for d in ('rk3xspi10', 'rkled2'):
                p = os.path.join(woa, 'woa-drivers', d)
                for ext in ('.inf', '.cat', '.sys'):
                    touch(os.path.join(p, d + ext))
            r, out = run(top, woa, gpu, npu, table)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            bd = os.path.join(out, 'rk3576-drivers', 'boards')
            self.assertIn('rk3xspi', lines(os.path.join(bd, 'cm5io.txt')))
            self.assertNotIn('rk3xspi10', lines(os.path.join(bd, 'cm5io.txt')))
            self.assertNotIn('rk3xspi', lines(os.path.join(bd, 'cm5io-win10.txt')))
            self.assertIn('rk3xspi10', lines(os.path.join(bd, 'cm5io-win10.txt')))
            self.assertNotIn('rk3xspi10', lines(os.path.join(bd, 'tspi3m-win10.txt')))
            # rk3xspi10 is not marked as its stand-in: rk3xspi is still named
            sk = lines(os.path.join(bd, 'win10-skipped.txt'))
            self.assertIn('rk3xspi\ttspi3m,cm5io,rock4d\tSPI', sk)
            sk = [l.split('\t') for l in lines(os.path.join(bd, 'skipped.txt'))]
            self.assertIn(['rk3xspi', '0', '21999', 'tspi3m,cm5io,rock4d', 'SPI'], sk)
            # rkled: 26100-, rkled2 covers 22000-26099, Windows 10 has neither
            self.assertIn(['rkled', '0', '21999', 'tspi3m,cm5io,rock4d', 'Board LEDs'], sk)
            self.assertFalse([x for x in sk if x[0] == 'rkled' and x[1] != '0'])
            self.assertFalse([x for x in sk if x[0] in ('rkled2', 'rk3xspi10', 'dwc_eqos', 'dwc_eqos10')])
            by = [l.split('\t') for l in lines(os.path.join(bd, 'rock4d-builds.txt'))]
            self.assertIn(['rkled', '26100', '99999'], by)
            self.assertIn(['rkled2', '22000', '26099'], by)
            with open(os.path.join(out, 'rk3576-drivers', 'manifest.json')) as f:
                m = {d['dir']: d for d in json.load(f)['drivers']}
            self.assertEqual(m['rk3xspi']['os'], 'win11')
            self.assertEqual(m['rk3xspi']['what'], 'SPI')
            self.assertEqual(m['rk3xspi10']['os'], 'win10')
            self.assertEqual(m['rkled2']['of'], 'rkled')
            self.assertEqual(m['rkled']['builds'], [26100, 99999])
            self.assertEqual(m['dwcsdhc']['os'], 'all')

    def test_unknown_stand_in_fails(self):
        with tempfile.TemporaryDirectory() as top:
            table = os.path.join(top, 'drivers.tsv')
            with open(TABLE, encoding='utf-8') as f:
                t = f.read() + 'rkled2       all            -         woa    win10:nosuch LEDs\n'
            with open(table, 'w', encoding='utf-8') as f:
                f.write(t)
            woa, gpu, npu = make_inputs(top)
            r, _ = run(top, woa, gpu, npu, table)
            self.assertNotEqual(r.returncode, 0)

    def test_missing_sys_fails(self):
        with tempfile.TemporaryDirectory() as top:
            r, _ = run(top, *make_inputs(top, skip_sys='es8323'))
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('es8323: no .sys', r.stdout)

    def test_inf_listed_file_is_packed(self):
        with tempfile.TemporaryDirectory() as top:
            r, out = run(top, *make_inputs(top))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(os.path.isfile(os.path.join(out, 'rk3576-drivers', 'drivers', 'maliwddm', 'malivk.json')))

    def test_inf_listed_file_missing_fails(self):
        with tempfile.TemporaryDirectory() as top:
            r, _ = run(top, *make_inputs(top, skip_gpu_file='malivk.json'))
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('maliwddm: maliwddm.inf lists malivk.json', r.stdout)

    def test_null_inf_names_amzn0001(self):
        with open(os.path.join(HERE, '..', '..', 'drivers', 'null', 'rk3576-null.inf'), encoding='ascii') as f:
            inf = f.read()
        for hid in ('ACPI\\HYMB0001', 'ACPI\\RKCP0806', 'ACPI\\RKCP3005', 'ACPI\\HISI0031', 'ACPI\\AMZN0001'):
            self.assertIn('= Null_Inst, ' + hid + '\n', inf)


if __name__ == '__main__':
    unittest.main(verbosity=2)
