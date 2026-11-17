#!/usr/bin/env python3
"""Host SDHI/card integration tests via QEMU qtest, without guest firmware.
Usage: python3 scripts/test/test_cdj2000_sdhi.py [candidate-qemu] [baseline-qemu]
Uses sparse temporary raw images; verifies source bytes never change.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

BASE = 0xffe40000

class Host:
    def __init__(self, binary, image, directory):
        env = dict(os.environ, CDJ_SD_INSERT='0')
        if image is None:
            env.pop('CDJ_SD_IMAGE', None)
        else:
            env['CDJ_SD_IMAGE'] = str(image)
        env.pop('CDJ_BF531_UPD', None)
        self.log = open(directory / 'qemu.log', 'wb')
        self.q = subprocess.Popen([binary, '-M', 'cdj2000nxs', '-display', 'none',
            '-accel', 'qtest', '-monitor', 'none', '-qtest',
            'unix:%s,server=on,wait=off' % (directory / 'qt'), '-qmp',
            'unix:%s,server=on,wait=off' % (directory / 'qm')],
            env=env, stdout=self.log, stderr=self.log)
        self.qt = self.connect(directory / 'qt')
        self.qm = self.connect(directory / 'qm')
        self.qf = self.qm.makefile('rwb', buffering=0)
        self.qf.readline()
        self.qmp('qmp_capabilities')
    def connect(self, path):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(str(path)); s.settimeout(10); return s
            except OSError:
                s.close()
                if self.q.poll() is not None:
                    raise RuntimeError(Path(self.log.name).read_text())
                time.sleep(.02)
        raise RuntimeError('qtest socket timeout')
    def qt_cmd(self, cmd):
        self.qt.sendall(cmd.encode() + b'\n')
        line = b''
        while not line.endswith(b'\n'):
            line += self.qt.recv(4096)
        assert line.startswith(b'OK'), line
        return line.decode().split()[1:]
    def qmp(self, name, **args):
        self.qf.write(json.dumps({'execute':name, 'arguments':args}).encode()+b'\n')
        while True:
            r=json.loads(self.qf.readline())
            if 'event' in r: continue
            assert 'error' not in r, r
            return r.get('return')
    def medium(self, action):
        self.qmp('qom-set', path='/machine/sdhi', property='card', value=action)
    def rd(self, off):
        return int(self.qt_cmd('readw 0x%x' % (BASE+off))[0], 16)
    def wr(self, off, val):
        self.qt_cmd('writew 0x%x 0x%x' % (BASE+off,val))
    def tick(self):
        self.qt_cmd('clock_step 20000')
    def command(self, cmd, arg=0):
        self.wr(0x1c, 0xfffe)
        self.wr(4,arg&0xffff); self.wr(6,arg>>16); self.wr(0,cmd)
        assert self.rd(0x1c)&1
        assert not self.rd(0x1e)&0x40, (cmd,hex(self.rd(0x1e)))
        return self.rd(0xc) | self.rd(0xe)<<16
    def init_card(self, hc):
        self.medium('insert')
        assert not self.rd(0x1c)&0x20
        self.command(0x300)
        assert self.command(0x408,0x1aa)==0x1aa
        for _ in range(20):
            self.command(0x437)
            ocr=self.command(0x769,0x40ff8000)
            if ocr&0x80000000: break
            self.qt_cmd('clock_step 10000000')
        assert ocr&0x80000000
        assert bool(ocr&0x40000000)==hc
        self.command(0x602)
        cid=[self.rd(0xc+2*i) for i in range(8)]
        assert cid[7]<256 and any(cid)
        rca=self.command(0x403)&0xffff0000
        assert rca
        self.command(0x609,rca)
        self.command(0x507,rca)
        self.command(0x410,512)
        return rca
    def data_read(self, cmd, arg, blocks=1, dma=False):
        self.wr(0x26,512); self.wr(0xa,blocks); self.wr(8,0x100)
        self.command(cmd,arg)
        assert not self.rd(0x1e)&0x100 # no IRQ inside CMD store
        out=bytearray()
        for block in range(blocks):
            assert not self.rd(0x1c)&4
            self.tick(); assert self.rd(0x1e)&0x100
            self.wr(0x1e,0xfeff) # ISR ack does NOT drain FIFO
            if dma:
                for off,value in [(0x80,BASE+0x30),(0x84,0x04010000),
                    (0x88,256),(0x8c,0x4000480d)]:
                    self.qt_cmd('writel 0x%x 0x%x' % (0xff608000+off,value))
                assert int(self.qt_cmd('readl 0xff608088')[0],16)==0
                assert int(self.qt_cmd('readl 0xff60808c')[0],16)&2
                raw=self.qt_cmd('read 0x04010000 512')[0]
                out.extend(bytes.fromhex(raw.removeprefix('0x')))
            else:
                for _ in range(128):
                    v=int(self.qt_cmd('readl 0xffe40030')[0],16)
                    out.extend(v.to_bytes(4,'little'))
        assert self.rd(0x1c)&4
        self.tick(); assert not self.rd(0x1e)&0x100
        return bytes(out)
    def data_write(self,arg,data,dma):
        blocks=len(data)//512
        assert blocks and len(data)==blocks*512
        self.wr(0x26,512); self.wr(0xa,blocks); self.wr(8,0x100)
        self.command(0x2c19 if blocks>1 else 0xc18,arg)
        assert not self.rd(0x1e)&0x200
        for block in range(blocks):
            assert not self.rd(0x1c)&4
            self.tick(); assert self.rd(0x1e)&0x200
            self.wr(0x1e,0xfdff)
            payload=data[block*512:(block+1)*512]
            if dma:
                self.qt_cmd('write 0x04010000 512 0x'+payload.hex())
                for off,value in [(0x70,0x04010000),(0x74,BASE+0x30),
                    (0x78,256),(0x7c,0x4000180d)]:
                    self.qt_cmd('writel 0x%x 0x%x' % (0xff608000+off,value))
                assert int(self.qt_cmd('readl 0xff608078')[0],16)==0
            else:
                for i in range(0,len(payload),4):
                    self.qt_cmd('writel 0xffe40030 0x%x'%int.from_bytes(payload[i:i+4],'little'))
        assert self.rd(0x1c)&4
    def close(self):
        try: self.qmp('quit')
        finally:
            self.q.wait(timeout=10); self.qt.close(); self.qf.close(); self.qm.close(); self.log.close()


def run(binary):
    for hc,size in [(False,64<<20),(True,4<<30)]:
        with tempfile.TemporaryDirectory(prefix='sdhi-',dir='/tmp') as name:
            d=Path(name); image=d/'card.raw'
            initial=bytes((i*37+11)&255 for i in range(1024))
            with image.open('wb') as f:
                f.write(initial); f.truncate(size)
            h=Host(binary,image,d)
            try:
                assert h.rd(0x1c)&0x20
                h.wr(0,0x408); assert h.rd(0x1e)&0x40
                h.wr(0x1e,0xffbf); assert not h.rd(0x1e)&0x40
                rca=h.init_card(hc)
                # Width/endian coverage: byte writes preserve the other byte;
                # combined status reads contain INFO1 in the low half.
                h.qt_cmd('writeb 0xffe40004 0x34')
                h.qt_cmd('writeb 0xffe40005 0x12')
                assert h.rd(4)==0x1234
                combined=int(h.qt_cmd('readl 0xffe4001c')[0],16)
                assert combined==(h.rd(0x1c) | h.rd(0x1e)<<16)
                h.wr(0x20,0xffff); h.wr(0x22,0xffff)
                assert h.rd(0x20)==h.rd(0x22)==0xffff
                h.command(0x1c11,0); h.tick()
                byte_data=bytes(int(h.qt_cmd('readb 0xffe40030')[0],16) for _ in range(512))
                assert byte_data==initial[:512] and h.rd(0x1c)&4
                assert int(h.qt_cmd('readb 0xffe40030')[0],16)==0
                assert h.data_read(0x1c11,0)==initial[:512]
                assert h.data_read(0x3c12,0,2,dma=True)==initial
                assert h.command(0x40d,rca)&0x1e00==0x800 # auto CMD12 -> TRAN
                for dma in [False,True]:
                    new=bytes([0xa5 if dma else 0x5a])*512
                    h.data_write(1 if hc else 512,new,dma)
                    assert h.data_read(0x1c11,1 if hc else 512)==new
                for dma in [False,True]:
                    payload=bytes([0x3c,0xc3])*512
                    h.data_write(2 if hc else 1024,payload,dma)
                    assert h.command(0x40d,rca)&0x1e00==0x800
                    assert h.data_read(0x3c12,2 if hc else 1024,2,dma=True)==payload
                # Out-of-range card R1 must not produce invented FIFO data.
                h.command(0x1c11,(size//512 if hc else size))
                h.tick()
                assert h.rd(0x1e)&0x20 and not h.rd(0x1e)&0x100
                h.wr(0x1e,0xffdf)
                # Eject with a pending data timer: nothing may arrive afterwards.
                h.command(0x1c11,0); h.medium('eject'); h.tick()
                assert h.rd(0x1c)&0x20
                assert not h.rd(0x1e)&0x300
                h.wr(0,0x408); assert h.rd(0x1e)&0x40
                h.init_card(hc)
                assert h.data_read(0x1c11,0)==initial[:512]
            finally: h.close()
            with image.open('rb') as f:
                assert f.read(2048)==initial+bytes(1024) # snapshot writes cannot touch raw source
                f.seek(size-512); assert f.read()==bytes(512)
            print('%s: initialization, R2, PIO/DMA, multi-block auto-stop, snapshot writes, eject/reinsert PASS'%('SDHC' if hc else 'SDSC'))

def empty_slot_parity(binary, baseline):
    transcripts=[]
    for executable in [baseline,binary]:
        with tempfile.TemporaryDirectory(prefix='sdhi-empty-',dir='/tmp') as name:
            h=Host(executable,None,Path(name)); transcript=[]
            try:
                for off in range(0,0x1000,2):
                    transcript.append(h.rd(off))
                for width,word in [('writeb',0xa5),('writew',0x1234),('writel',0x89abcdef)]:
                    for off in range(0,0x100,4):
                        h.qt_cmd('%s 0x%x 0x%x'%(width,BASE+off,word))
                        transcript.extend([h.rd(off),h.rd(off+2)])
                h.wr(0x1c,0xfffe); h.wr(0x1e,0xffbf); h.wr(0,0x408)
                transcript.extend([h.rd(0x1c),h.rd(0x1e)])
            finally:h.close()
            transcripts.append(transcript)
    assert transcripts[0]==transcripts[1], 'empty-slot MMIO transcript changed'
    print('Unconfigured model: empty-slot MMIO transcript identical to baseline PASS')

if __name__=='__main__':
    binary=str(Path(sys.argv[1] if len(sys.argv)>1 else 'qemu-build/qemu-system-sh4').resolve())
    run(binary)
    if len(sys.argv)>2:
        empty_slot_parity(binary,str(Path(sys.argv[2]).resolve()))
