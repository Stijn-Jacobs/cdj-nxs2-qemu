#!/usr/bin/env python3
"""Local stock timing experiment; owns and stops only processes it starts.

Firmware, media and native DSP modules are read-only inputs. Outputs go to
OUT; media writes use QEMU's snapshot overlay. No qualification setup imports.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time

STOCK_FIELDS = {
    'sync_enabled': (0x04cf2168, 4), 'player_number': (0x04cf2488, 4),
    'master_valid': (0x04e34ea8, 4), 'sync_override': (0x04e34eac, 4),
    'sync_state_a': (0x04e34eb0, 4), 'rate': (0x04e34ebc, 4),
    'deviation': (0x04e34ec8, 2), 'sync_state_b': (0x04e34edc, 4),
    'sync_locked': (0x051dd8a4, 4), 'track_phase': (0x051dd974, 4),
    'sync_flag': (0x04cf2164, 4), 'report_stream': (0x051dd880, 4),
    'report_timestamp': (0x051de9b0, 4),
}


class Monitor:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(path)); self.sock.settimeout(5); self.read()
        self.lock = threading.Lock()

    def read(self):
        buf = b''
        while not buf.endswith(b'(qemu) '):
            part = self.sock.recv(65536)
            if not part: raise EOFError('monitor closed')
            buf += part
        return buf.decode(errors='replace')

    def cmd(self, command):
        with self.lock:
            self.sock.sendall(command.encode() + b'\n')
            return self.read()

    def word(self, address):
        import re
        match = re.search(r':\s*(0x[0-9a-f]+)', self.cmd(f'xp /1wx {address:#x}'))
        return int(match[1], 16) if match else None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--qemu', type=Path, required=True)
    p.add_argument('--inputs', type=Path, required=True)
    p.add_argument('--usb', type=Path, required=True)
    p.add_argument('--modules', required=True)
    p.add_argument('--libdir', required=True)
    p.add_argument('--hub-root', type=Path)
    p.add_argument('--decks', type=int, default=1)
    p.add_argument('--icount')
    p.add_argument('--seconds', type=float, default=90)
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--play', action='store_true', help='stock TRACK browser load, then MASTER/SYNC')
    p.add_argument('--clock', choices=['wall'], help='shared bounded host clock, without icount')
    p.add_argument('--nor', type=Path, help='copy this NOR into each owned run; boot through reset vector')
    p.add_argument('--display', type=Path, help='override the display UPD (read-only)')
    p.add_argument('--track-index', type=int, default=0, help='zero-based position in stock TRACK list')
    p.add_argument('--profile-at', type=float, help='Mac sample profiles once both guest clocks reach this time')
    p.add_argument('--screen-period', type=float, default=0, help='continuous screendump observer, wall seconds')
    p.add_argument('--session-io', action='store_true', help='enable session.py display capture and debugger endpoints')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    procs, logs, samples = [], [], []
    observer_stop = threading.Event()
    env = {k: v for k, v in os.environ.items() if not k.startswith(('CDJ_', 'C66X_'))}
    env.update(CDJ_REPORT='1', CDJ_ATA='1', CDJ_C6747_IDLE='0xC004CB8C:0x11804AE0:0x11805C00',
               C66X_IDLE_ISR_FAST='1', C66X_JIT=a.modules, C66X_JIT_LIBDIR=a.libdir,
               CDJ_BF531_UPD=str(a.inputs.resolve() / 'display.upd'), CDJ_DSP_AUDIO='1:3000:150:450')
    env.update(dict(s.split('=', 1) for s in a.env))
    if a.display: env['CDJ_BF531_UPD'] = str(a.display.resolve())
    if a.clock:
        if a.icount or a.decks < 2: p.error('--clock needs a pair without --icount')
        env.update(CDJ_NETSIM_CLOCK=a.clock, CDJ_NETSIM_CLOCK_FILE=str(out / 'epoch'))
    def launch(argv, env, cwd, log):
        stream = log.open('wb'); logs.append(stream)
        q = subprocess.Popen(argv, env=env, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL)
        procs.append(q)
        return q
    def stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    commands = []
    try:
        if a.decks > 1:
            assert a.hub_root
            he = dict(os.environ, PYTHONPATH=str(a.hub_root.resolve()), PYTHONDONTWRITEBYTECODE='1')
            hub = out / 'hub.sock'
            launch([sys.executable, str(Path(__file__).with_name('netspeed_hub.py').resolve()),
                    '--hub-root', str(a.hub_root.resolve()), str(out / 'hub'),
                    '--listen', 'unix:' + str(hub), '--sync', '--decks', str(a.decks)],
                   he, out, out / 'hub.log')
            for _ in range(100):
                if hub.exists(): break
                time.sleep(.05)
            env['CDJ_NETSIM'] = 'unix:' + str(hub)
        decks = []
        for i in range(a.decks):
            d = out / str(i + 1); d.mkdir()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            de = dict(env, CDJ_PANEL_KEYSOCK=str(port), CDJ_ETHER_MAC=f'00:1c:c0:20:00:{i+1:02x}',
                      CDJ_VCLOCK_FILE=str(d / 'vclock'), CDJ_NETSIM_CENSUS='5',
                      CDJ_NETSIM_TRACE=str(d / 'netsim.jsonl'))
            argv = [str(a.qemu.resolve()), '-M', 'cdj2000nxs', '-kernel',
                    str(a.inputs.resolve() / 'main_unpacked.bin'), '-display', 'none', '-audio', 'none',
                    '-serial', 'file:serial.log', '-monitor', 'unix:mon.sock,server,nowait',
                    '-drive', f'if=none,id=usbstk,file={a.usb.resolve()},format=raw,snapshot=on',
                    '-device', 'usb-storage,drive=usbstk,port=1']
            if a.session_io:
                de.update(CDJ_BF531_GDB='gui-gdb.sock', CDJ_LINK_CAPTURE=str(d / 'main-link.bin'))
                argv += ['-qmp', 'unix:qmp.sock,server,nowait',
                         '-gdb', 'unix:gdb.sock,server=on,wait=off']
            if a.icount: argv += ['-icount', a.icount]
            if a.nor:
                nor = a.nor.read_bytes()
                if len(nor) > 4 * 1024 * 1024: p.error('NOR exceeds 4 MiB')
                (d / 'nor.bin').write_bytes(nor.ljust(4 * 1024 * 1024, b'\xff'))
                k = argv.index('-kernel'); del argv[k:k+2]
                argv += ['-drive', 'if=pflash,unit=0,format=raw,file=nor.bin']
            q = launch(argv, de, d, d / 'qemu.log'); decks.append((q, d, port))
            commands.append(dict(argv=argv, environment={k:v for k,v in de.items()
                                                       if k.startswith(('CDJ_', 'C66X_'))}, pid=q.pid))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2))
        inputs = [a.qemu.resolve(), a.inputs.resolve() / 'main_unpacked.bin',
                  Path(env['CDJ_BF531_UPD']), *map(Path, a.modules.split(':'))]
        if a.nor: inputs.append(a.nor.resolve())
        (out / 'hashes.json').write_text(json.dumps({str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                                    for p in inputs}, indent=2))
        mons = {}
        stages = [0] * a.decks
        controls = [0] * a.decks
        profiled = False
        # Each wait is guest time, including key release. The firmware/card
        # must still be checked in the resulting screens and state samples.
        keys = [(30, 'u'), *[(33 + j*.5, 'up') for j in range(6)],
                (37, 'down'), (38, 'down'), (40, 'ret'),
                *[(55 + j*.5, 'up') for j in range(6)],
                *[(58 + j*.5, 'down') for j in range(a.track_index)],
                (max(60, 58 + a.track_index*.5 + 3), 'ret')]
        start = time.monotonic()
        while time.monotonic() - start < a.seconds:
            row = dict(wall=time.monotonic() - start, guest=[])
            for i, (q, d, port) in enumerate(decks):
                if q.poll() is not None: raise RuntimeError(f'QEMU exit {q.returncode}; {d}')
                try: v = int((d / 'vclock').read_text()) / 1e9
                except (ValueError, FileNotFoundError): v = None
                row['guest'].append(v)
                if a.play and v is not None:
                    if i not in mons and (d / 'mon.sock').exists():
                        mons[i] = Monitor(d / 'mon.sock')
                        if a.screen_period:
                            def observe(mon=mons[i], directory=d):
                                while not observer_stop.is_set():
                                    try: mon.cmd(f'screendump {directory / "observer.ppm"}')
                                    except (OSError, EOFError): return
                                    observer_stop.wait(a.screen_period)
                            threading.Thread(target=observe, daemon=True).start()
                    mon = mons.get(i)
                    if mon:
                        row.setdefault('position', []).append(mon.word(0x0491b278))
                        row.setdefault('state', []).append(mon.word(0x04e34e48))
                        if len(samples) % 25 == 0:
                            fields = {name: mon.word(addr) & ((1 << (size*8))-1)
                                      for name, (addr, size) in STOCK_FIELDS.items()}
                            row.setdefault('engine', []).append(fields)
                            mon.cmd(f'screendump {d / "latest.ppm"}')
                        if v >= 30 and not (d / 'module-mappings.txt').exists():
                            mapped = subprocess.check_output(['lsof', '-a', '-p', str(q.pid), '-Fn'], text=True)
                            (d / 'module-mappings.txt').write_text(mapped)
                            if not all('\nn' + path + '\n' in mapped for path in a.modules.split(':')):
                                raise RuntimeError('native DSP chain not mapped')
                        if stages[i] < len(keys) and v >= keys[stages[i]][0] + (2*i if stages[i] == len(keys)-1 else 0):
                            mon.cmd(f'sendkey {keys[stages[i]][1]} 150')
                            mon.cmd(f'screendump {d / (str(stages[i]) + ".ppm")}')
                            stages[i] += 1
                        elif stages[i] == len(keys) and v >= 80 + 2*i:
                            if i != 0 or mon.word(0x04e34ea8) != 0:
                                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as key:
                                    key.sendto(f'21:{4 if i == 0 else 2}:400:or'.encode(), ('127.0.0.1', port))
                            mon.cmd(f'screendump {d / "sync.ppm"}')
                            stages[i] += 1
                        elif stages[i] > len(keys) and i == 1 and controls[i] < 2 and v >= (105, 110)[controls[i]]:
                            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as key:
                                key.sendto(b'21:2:400:or', ('127.0.0.1', port))
                            controls[i] += 1
            if len(samples) % 25 == 0:
                cpu = subprocess.check_output(['ps', '-o', 'pid=,time=', '-p',
                                              ','.join(str(q.pid) for q, _, _ in decks)], text=True)
                row['cpu'] = cpu
            samples.append(row)
            if a.profile_at and not profiled and all(v is not None and v >= a.profile_at for v in row['guest']):
                for i, (q, d, _) in enumerate(decks):
                    launch(['/usr/bin/sample', str(q.pid), '5', '1', '-file', str(d / 'sample.txt')],
                           os.environ, out, d / 'sample.log')
                profiled = True
            if len(samples) % 25 == 0:
                (out / 'samples.json').write_text(json.dumps(samples, indent=2))
            time.sleep(.2 if a.play else 1)
    finally:
        observer_stop.set()
        for q in reversed(procs):
            if q.poll() is None: q.terminate()
        for q in procs:
            try: q.wait(timeout=15)
            except subprocess.TimeoutExpired: q.kill(); q.wait()
        for stream in logs: stream.close()
        (out / 'samples.json').write_text(json.dumps(samples, indent=2))
    print(json.dumps(samples[-1] if samples else {}))


if __name__ == '__main__':
    main()
