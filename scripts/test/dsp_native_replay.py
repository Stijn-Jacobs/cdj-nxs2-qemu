#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Adapted from cdj-2000nxs-mods tools/linux/compare_dsp_replay.py at fadfefef.
"""Compare a complete NXS DSP recording under interpreter and native module.

Both runs must replay every record EXACT and have identical final state,
write and RAM hashes. This proves only the recorded workload. Outputs stay
private beside the recording and are never treated as library qualification.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    p.add_argument('--runner', type=Path, default=root / 'build/c6x/c6xreplay')
    p.add_argument('--record', type=Path, default=root / 'extract/dsp.c6rec')
    p.add_argument('--module', type=Path, action='append',
                   help='native module; repeat for an ordered supplemental module chain')
    p.add_argument('--out', type=Path, default=root / 'runs/dsp-native-replay')
    a = p.parse_args()
    if a.module is None:
        folder = Path.home() / 'c14gen/curated-cdj2000nxs'
        a.module = [folder / 'm.so', folder / 'm.extra.so']
    missing = [str(f) for f in [a.runner, a.record, *a.module] if not f.is_file()]
    if missing:
        print('SKIP DSP native replay: missing private inputs/tools: ' + ', '.join(missing))
        return 0
    a.out.mkdir(parents=True, exist_ok=False)
    record_sha = sha(a.record)
    receipt = {'record_sha256': record_sha, 'runner_sha256': sha(a.runner),
               'modules': [{'path': str(m.resolve()), 'sha256': sha(m)} for m in a.module],
               'scope': 'complete recorded NXS workload', 'runs': {}}
    base = {k: v for k, v in os.environ.items() if not k.startswith(('C66X_', 'CDJ_', 'REPLAY_'))}
    base.update(REPLAY_IDLE='0xc004cb8c:0x11804ae0:0x11805c00:0', C66X_IDLE_ISR_FAST='1')
    for label in ('interpreter', 'native'):
        env = dict(base)
        if label == 'native':
            env['C66X_JIT'] = os.pathsep.join(str(m.resolve()) for m in a.module)
        if sha(a.record) != record_sha:
            raise ValueError('record changed between comparison runs')
        log = a.out / (label + '.log')
        start = time.monotonic()
        with log.open('w') as f:
            proc = subprocess.run([str(a.runner.resolve()), str(a.record.resolve())],
                                  env=env, stdout=f, stderr=subprocess.STDOUT, timeout=3600)
        text = log.read_text()
        count = re.search(r'replayed (\d+) of (\d+) records', text)
        final = re.search(r'^final: state (\S+) writes (\S+) ram (\S+)$', text, re.M)
        speed = re.search(r'in ([\d.]+) s host: ([\d.]+) M executed cycles/s', text)
        exact = proc.returncode == 0 and '\nverdict: EXACT\n' in text
        # Loader diagnostics precede the verdict; module statistics follow it.
        # Never pass a native run that silently fell back after a load failure.
        if label == 'native' and 'c66x jit:' in text.split('verdict:', 1)[0]:
            exact = False
        complete = count is not None and int(count[1]) == int(count[2]) and int(count[1]) > 0
        receipt['runs'][label] = {'returncode': proc.returncode, 'exact': exact, 'complete': complete,
                                  'records': int(count[1]) if count else None,
                                  'total_records': int(count[2]) if count else None,
                                  'wall_seconds': time.monotonic() - start,
                                  'replay_seconds': float(speed[1]) if speed else None,
                                  'million_executed_cycles_per_second': float(speed[2]) if speed else None,
                                  'final': list(final.groups()) if final else None,
                                  'log_sha256': sha(log)}
        (a.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(label, 'EXACT' if exact and complete else 'FAIL', flush=True)
    runs = receipt['runs']
    ok = all(r['exact'] and r['complete'] and r['final'] for r in runs.values())
    ok = ok and runs['native']['final'] == runs['interpreter']['final']
    ok = ok and runs['native']['records'] == runs['interpreter']['records']
    receipt['verdict'] = 'PASS' if ok else 'FAIL'
    (a.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(receipt['verdict'], a.out / 'receipt.json')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
