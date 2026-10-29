#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a patched firmware update (.UPD) against the stock one it came from.

    usage: validate.py [--model <id>] [--stock <official.UPD>] <file.UPD>
           validate.py [--model <id>] <stock.UPD> <patched.UPD>

patch_update.py runs the same checks on everything it writes and refuses a
file that fails; this is the command-line door for a file made elsewhere.

With one file, the player and version are read from its section labels and the
official update of that player (models/<id>.conf pins it by sha256) is looked
for beside the file, in fw/ and in the player's firmware folder, or named with
--stock. The file's own sha256 only says whether it is an unmodified official
update; every checksum inside it is recomputed from its own bytes. A change
nobody declared is judged by what the stock bytes were: erased space is free
only if no task reaches it and nothing points at it, existing code may only get
a call-site-sized write outside the boot-critical set.
Exit status: 0 safe as far as checked, 1 on any FAIL, 2 when nothing failed
but it could not be shown safe (no official copy to compare with, or a write
that could not be placed).

The deck's boot loader checks one thing before it runs MAIN: a 16-bit sum of
the packed image. A patched update with a right sum and wrong code is not
caught by anything below the application, and the only way out of it is the
deck's own update mode, which is MAIN code. So the checks are:

  1. Byte identity. The update-file header and trailer, the boot loader at
     offset 0, the emergency updater at 0x10000 and every record in front of
     the packed application, and every section a mod does not target, are
     the stock bytes.
  2. Structure, re-parsed here without the encoders: the section table, each
     section's label, S-record lengths and checksums, the CRC-16 trailer, and
     for the emergency image and the packed MAIN the length word, the stored
     16-bit sum and the decoded size, field by field against the stock file.
  3. Boot-critical code (bootcrit.py): no byte a mod writes, whether a call
     site or injected code or claimed free space, lies in code the boot, the
     panel, the display link, the update task or the USB stick task can run.
     The display image has no call graph; there only the load table, the
     vector copy and bytes a mod did not declare are protected.
  4. The DSP program MAIN carries as data (dsppatch.py): the words a mod edits
     are declared, and the code it places is in the program's erased run, away
     from the run's ends, or in the cave of a program carried as a boot table,
     whose stock bytes are still there, with no word, branch or constant pair
     in the program pointing into it. The program is not MAIN code, so the
     boot-critical set does not apply to it.

A player that ships one file per section (CDJ-2000, CDJ-900) has its GUI, DRIV
and PANL files checked as well: each next to the patched MAIN file must be the
shipped file, or an S-record file whose CRC-16 verifies.

What this cannot show is printed after the checks as NOT CHECKED.
"""
import contextlib
import hashlib
import io
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'scripts', 'firmware'))

import bootcrit
import dsppatch
import gui_decode
import main_decode
import modelfw
import patch_gui
import patch_main
import sigpatch

LABEL_SIZE = 32
EMERGENCY_OFF = 0x10000
MAX_SITE_BYTES = 32
KNOWN_GUI_SITES = ('wave3', 'wave3ov', 'phrase', 'x3000trainpos')
# Byte rewrites of display code that is equally off the boot path: the detail
# waveform message (0x37) handler runs only when MAIN sends a loaded track's
# waveform.
KNOWN_GUI_TABLES = ('x3000keepwave',)
VECTOR_COPY_DEST = 0xFFF84000
RECORD_BYTES = 12
SHOWN = 8

ADDRESS_BYTES = {b'0': 2, b'1': 2, b'2': 3, b'3': 4, b'7': 4, b'8': 3, b'9': 2}


class Report:
    def __init__(self):
        self.lines = []
        self.not_checked = []

    def ok(self, text):
        self.lines.append(('PASS', text))

    def fail(self, text):
        self.lines.append(('FAIL', text))

    def note(self, text):
        self.lines.append(('NOTE', text))

    def unproven(self, text):
        self.lines.append(('OPEN', text))

    @property
    def failed(self):
        return any(state == 'FAIL' for state, _ in self.lines)

    @property
    def open(self):
        return any(state == 'OPEN' for state, _ in self.lines)

    @property
    def exit_code(self):
        return 1 if self.failed else 2 if self.open else 0

    def text(self):
        out = ['%-4s %s' % line for line in self.lines]
        out.append(('all checks passed', 'FAILED',
                    'NOT SHOWN SAFE: nothing failed, but some of it could not be checked')[self.exit_code])
        out.append('NOT CHECKED:')
        out += ['  - %s' % item for item in self.not_checked]
        return '\n'.join(out)


def _crc_table():
    table = []
    for byte in range(256):
        crc = byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


CRC_TABLE = _crc_table()


def crc16(data):
    """CRC-16/XMODEM."""
    crc = 0
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ CRC_TABLE[(crc >> 8) ^ byte]
    return crc


def sum16(data):
    return sum(data) & 0xFFFF


class Upd:
    def __init__(self, data, fw):
        self.data = data
        if fw.bare:
            self.sizes, self.header = [len(data)], 0
        else:
            self.sizes, self.header = modelfw.parse_manifest(data)
        self.sections, pos = [], self.header
        for size in self.sizes:
            self.sections.append(data[pos:pos + size])
            pos += size
        self.trailer = data[pos:]
        self.complete = pos <= len(data)


def section_kind(section):
    """'srec' (Motorola records with a CRC-16 trailer), 'blob' (big-endian
    length, LZSS stream, CRC-16) or 'opaque'."""
    body = section[LABEL_SIZE:LABEL_SIZE + 2]
    if body[:1] == b'S' and body[1:2].isdigit():
        return 'srec'
    if len(section) >= LABEL_SIZE + 6 and struct.unpack_from('>I', section, LABEL_SIZE)[0] + LABEL_SIZE + 6 == len(section):
        return 'blob'
    return 'opaque'


class Srec:
    """A section of S-record lines, parsed from its bytes."""

    def __init__(self, section):
        self.label = section[:LABEL_SIZE]
        self.errors = []
        self.crc_ok = crc16(section[:-2]) == struct.unpack('<H', section[-2:])[0]
        body = section[LABEL_SIZE:-2]
        if not body.endswith(b'\r\n'):
            self.errors.append('body does not end with a line break')
        self.lines = body.split(b'\r\n')[:-1]
        self.data = []          # (address, bytes, line) for S1-S3
        self.addrs = []         # per line: its address if it carries data
        for line in self.lines:
            before = len(self.data)
            self._record(line)
            self.addrs.append(self.data[-1][0] if len(self.data) > before else None)

    def _record(self, line):
        kind = line[1:2]
        try:
            rec = bytes.fromhex(line[2:].decode('ascii'))
        except ValueError:
            self.errors.append('not hex: %r' % line[:20])
            return
        if line[:1] != b'S' or kind not in ADDRESS_BYTES:
            self.errors.append('unknown record %r' % line[:6])
        elif rec[0] != len(rec) - 1:
            self.errors.append('length byte wrong: %r' % line[:20])
        elif sum(rec) & 0xFF != 0xFF:
            self.errors.append('checksum wrong: %r' % line[:20])
        elif kind in (b'1', b'2', b'3'):
            n = ADDRESS_BYTES[kind]
            self.data.append((int.from_bytes(rec[1:1 + n], 'big'), rec[1 + n:-1], line))

    def in_front(self, off):
        """Every line that is not part of the packed application at `off`."""
        return [line for line, addr in zip(self.lines, self.addrs) if addr is None or addr < off]

    def run(self, off):
        """(bytes of the packed application, errors): the records from `off`
        up, which must follow each other without a gap."""
        out, pos = bytearray(), off
        for addr, data, _ in self.data:
            if addr < off:
                continue
            if addr != pos:
                return bytes(out), ['packed run has a gap or overlap at %#x' % addr]
            out += data
            pos += len(data)
        return bytes(out), []

    def prefix(self, off):
        """The bytes of everything below `off`, 0xFF where nothing is written."""
        flat = bytearray(b'\xff' * off)
        for addr, data, _ in self.data:
            if addr < off:
                flat[addr:addr + len(data)] = data
        return bytes(flat)


def stream_fields(flat, off):
    """(length word, stored sum, computed sum) of a length + stream + sum16
    block at `off` of a flat flash buffer, or None if it runs off the end."""
    if off + 4 > len(flat):
        return None
    length = struct.unpack_from('<I', flat, off)[0]
    end = off + 4 + length
    if end + 2 > len(flat):
        return None
    return length, struct.unpack_from('<H', flat, end)[0], sum16(flat[off:end])


def sha(data):
    return hashlib.sha256(data).hexdigest()[:16]


def changed_ranges(a, b, gap=3):
    """[(lo, hi)] where equal-length buffers differ, joined across gaps shorter
    than `gap` bytes."""
    out, block = [], 4096
    for base in range(0, len(a), block):
        if a[base:base + block] == b[base:base + block]:
            continue
        for i in range(base, min(base + block, len(a))):
            if a[i] != b[i]:
                if out and i - out[-1][1] < gap:
                    out[-1][1] = i + 1
                else:
                    out.append([i, i + 1])
    return [tuple(r) for r in out]


def check_untouched(stock, out, fw, targets, rep):
    """Everything outside the packed application, section by section."""
    if len(stock.sections) != len(out.sections):
        rep.fail('section count %d -> %d' % (len(stock.sections), len(out.sections)))
        return
    rep.ok('section table: %d sections, %s bytes of header' % (len(out.sections), out.header))
    allowed = {fw.sections[t] - 1 for t in fw.sections if targets is None or t in targets}
    for i, (a, b) in enumerate(zip(stock.sections, out.sections)):
        label = a[:LABEL_SIZE].decode('latin1').rstrip()
        if a[:LABEL_SIZE] != b[:LABEL_SIZE]:
            rep.fail('section %d (%s): label changed' % (i + 1, label))
        if i not in allowed:
            if a == b:
                rep.ok('section %d (%s): identical, sha256 %s' % (i + 1, label, sha(a)))
            else:
                rep.fail('section %d (%s): changed, but no mod targets it' % (i + 1, label))
    if stock.trailer == out.trailer:
        rep.ok('%d bytes after the last section identical' % len(out.trailer))
    else:
        rep.fail('bytes after the last section differ (%d -> %d)' % (len(stock.trailer), len(out.trailer)))


def check_srec_section(i, stock_sec, out_sec, fw, model, rep):
    name = 'section %d (%s)' % (i + 1, out_sec[:LABEL_SIZE].decode('latin1').rstrip())
    s, o = (Srec(stock_sec) if stock_sec else None), Srec(out_sec)
    off = fw.main_off
    if s and s.errors:
        rep.fail('%s: the stock section does not parse (%s)' % (name, s.errors[0]))
        return None
    if o.errors:
        rep.fail('%s: %s' % (name, o.errors[0]))
        return None
    if s and not s.crc_ok:
        rep.fail('%s: the stock CRC-16 trailer does not verify, so the input is damaged or edited by hand' % name)
    if o.crc_ok:
        rep.ok('%s: CRC-16 trailer and %d S-record checksums verify' % (name, len(o.lines)))
    else:
        rep.fail('%s: CRC-16 trailer does not verify' % name)
    if out_sec[LABEL_SIZE - 1:LABEL_SIZE] != b'0':
        rep.fail('%s: label byte 31 is not 0, so the updater would write the boot loader too' % name)

    if s:
        if s.in_front(off) == o.in_front(off):
            rep.ok('%s: header, boot loader, emergency updater and terminator records identical (%d lines)'
                   % (name, len(o.in_front(off))))
        else:
            rep.fail('%s: records in front of the packed application differ from the stock file' % name)
        if s.prefix(off) == o.prefix(off):
            rep.ok('%s: bytes 0-%#x (boot loader at 0, emergency updater at %#x) identical'
                   % (name, off, EMERGENCY_OFF))
        else:
            rep.fail('%s: bytes below %#x differ' % (name, off))

    for label, sec in (('stock', s), ('output', o)):
        if sec is None:
            continue
        flat = sec.prefix(off)
        got = stream_fields(flat, EMERGENCY_OFF)
        if got is None:
            continue
        length, stored, computed = got
        if stored == computed:
            if label == 'output':
                rep.ok('%s: emergency image sum %#06x over %d bytes verifies' % (name, stored, length))
        elif label == 'output':
            rep.fail('%s: emergency image stored sum %#06x, computed %#06x' % (name, stored, computed))

    s_run = s.run(off)[0] if s else None
    o_run, o_err = o.run(off)
    if o_err or not o_run:
        rep.fail('%s: %s' % (name, o_err[0] if o_err else 'no packed application at %#x' % off))
        return None
    if min(a for a, _, _ in o.data if a >= off) != off:
        rep.fail('%s: packed application does not start at %#x' % (name, off))
    got = stream_fields(o_run, 0)
    if got is None:
        rep.fail('%s: the packed length word runs past the records' % name)
        return None
    length, stored, computed = got
    if stored != computed:
        rep.fail('%s: packed MAIN stored sum %#06x, computed %#06x' % (name, stored, computed))
    else:
        rep.ok('%s: packed MAIN sum %#06x over %d bytes verifies' % (name, stored, length))
    tail = len(o_run) - (6 + length)
    if not 0 <= tail < 32 or o_run[6 + length:] != b'\xff' * tail:
        rep.fail('%s: %d bytes after the stored sum are not record padding' % (name, tail))
    end = off + len(o_run)
    window_end = modelfw.update_window_end(model)
    what = 'the update window'
    if window_end is None and s_run is not None:
        window_end = off + len(s_run)
        what = 'the stock packed application, since no update window is known for this player'
    if window_end is None:
        rep.unproven('%s: packed application ends at %#x, and no update window is known for this player' % (name, end))
    elif end > window_end:
        rep.fail('%s: packed application ends at %#x, past %s (%#x)' % (name, end, what, window_end))
    else:
        rep.ok('%s: packed application ends at %#x, inside %s (%#x)' % (name, end, what, window_end))
    return (main_decode.decode_image(o_run, 0)[1],
            main_decode.decode_image(s_run, 0)[1] if s_run else None)


def check_blob_section(i, stock_sec, out_sec, rep):
    name = 'section %d (%s)' % (i + 1, out_sec[:LABEL_SIZE].decode('latin1').rstrip())
    if section_kind(out_sec) != 'blob':
        rep.fail('%s: length word does not match the section size' % name)
        return None
    if crc16(out_sec[:-2]) == struct.unpack('>H', out_sec[-2:])[0]:
        rep.ok('%s: CRC-16 trailer verifies' % name)
    else:
        rep.fail('%s: CRC-16 trailer does not verify' % name)
    if stock_sec is None:
        return None
    if section_kind(stock_sec) != 'blob':
        rep.fail('%s: the stock section is not a length + stream + CRC blob' % name)
        return None
    if crc16(stock_sec[:-2]) != struct.unpack('>H', stock_sec[-2:])[0]:
        rep.fail('%s: the stock CRC-16 trailer does not verify, so the input is damaged or edited by hand' % name)
        return None
    return gui_decode.decode(out_sec)[3], gui_decode.decode(stock_sec)[3]


def decode_pair(stock, out, fw, model, targets, rep):
    """{target: (stock image, output image)} for every changed target section,
    after the structure checks on it."""
    images = {}
    for target, number in fw.sections.items():
        i = number - 1
        if (targets is not None and target not in targets) or i >= len(out.sections):
            continue
        kind = section_kind(out.sections[i])
        if kind == 'srec':
            pair = check_srec_section(i, stock.sections[i], out.sections[i], fw, model, rep)
        elif kind == 'blob':
            pair = check_blob_section(i, stock.sections[i], out.sections[i], rep)
        elif stock.sections[i] != out.sections[i]:
            rep.fail('section %d changed but is neither S-records nor a length + stream blob' % number)
            continue
        else:
            continue
        if pair is None:
            continue
        out_img, stock_img = pair
        if stock.sections[i] == out.sections[i]:
            continue
        if len(out_img) != len(stock_img):
            rep.fail('%s image is %d bytes, the stock one %d' % (target, len(out_img), len(stock_img)))
        else:
            rep.ok('%s image decodes to the stock size, %d bytes' % (target, len(out_img)))
            images[target] = (stock_img, out_img)
    return images


def cave_or_unproven(image, lo, hi):
    """Why [lo, hi) of the stock image is not free space, or None. A profile's
    cave is proven by its strings and hash; anything else must pass
    sigpatch.unproven."""
    if image.p.cave:
        try:
            start, end = sigpatch.find_cave(image)
        except sigpatch.NotApplied as e:
            return str(e)
        if start <= lo and hi <= end:
            return None
    return sigpatch.unproven(image, lo, hi)


def overlaps(spans, ranges):
    """[(mod, kind, lo, hi, range)] for every span that overlaps a range."""
    return [(mod, kind, lo, hi, r) for mod, kind, lo, hi in spans for r in ranges if lo < r[1] and r[0] < hi]


def flat_spans(spans, changed, stock_img, table, cave, dsp=None):
    """[(mod, kind, lo, hi)] from what the patch engine says it wrote, or
    from the changed ranges alone when it did not say. A range inside the
    profile's cave is free space although the stock bytes there are code.
    A range inside the DSP program `dsp` (file offsets lo, hi) is an edit of
    it, or code placed in its erased space."""
    if spans is None:
        def kind(lo, hi):
            if table and table[0] <= lo and hi <= table[1]:
                return 'record'
            if dsp and dsp[0] <= lo and hi <= dsp[1]:
                return 'dspfree' if set(stock_img[lo:hi]) == {0xFF} else 'dsp'
            if cave and cave[0] <= lo and hi <= cave[1]:
                return 'free'
            return 'free' if set(stock_img[lo:hi]) == {0xFF} else 'site'
        return [('?', kind(lo, hi), lo, hi) for lo, hi in changed]
    return [(mod, kind, off, off + size) for mod, items in spans.items() for off, size, kind in items]


def check_writes(target, stock_img, out_img, profile, spans, rep, table=None, dsp=None):
    """Checks that hold for every image: what changed is what was declared,
    free space was erased, a site is small. Returns the span list."""
    changed = changed_ranges(stock_img, out_img)
    image = sigpatch.Image(profile, stock_img)
    cave = None
    if profile.cave:
        try:
            cave = sigpatch.find_cave(image)
        except sigpatch.NotApplied:
            pass
    flat = flat_spans(spans, changed, stock_img, table, cave, dsp)
    if spans is not None:
        declared = bootcrit.merge([(lo, hi) for _, _, lo, hi in flat])
        stray = [r for r in changed if not any(lo <= r[0] and r[1] <= hi for lo, hi in declared)]
        if stray:
            rep.fail('%s: %d changed ranges no mod declared, first at offset %#x' % (target, len(stray), stray[0][0]))
        else:
            rep.ok('%s: all %d changed ranges are writes a mod declared' % (target, len(changed)))
    for mod, kind, lo, hi in flat:
        if kind == 'free' and not profile.cave and set(stock_img[lo:hi]) != {0xFF}:
            rep.fail('%s: %s claims %#x-%#x as free space, but it is not erased' % (target, mod, lo, hi))
        if kind == 'site' and target == 'main' and hi - lo > MAX_SITE_BYTES:
            rep.fail('%s: %s rewrites %d bytes at %#x, more than a call site' % (target, mod, hi - lo, lo))
    runs = bootcrit.merge([(lo, hi) for _, kind, lo, hi in flat if kind == 'free'])
    proven = 0
    for lo, hi in runs:
        why = cave_or_unproven(image, lo, hi)
        owners = sorted({mod for mod, kind, a, b in flat if kind == 'free' and a < hi and lo < b})
        if why:
            # With no declaration, erased-looking bytes that something points at are data we cannot place
            (rep.unproven if spans is None else rep.fail)(
                '%s: free space %#x-%#x (%s) is not proven free: %s'
                % (target, profile.flash_base + lo, profile.flash_base + hi, ', '.join(owners), why))
        else:
            proven += hi - lo
    if proven:
        rep.ok('%s: %d bytes of free space in %d runs are erased (or cave), unreferenced and outside every declared '
               'segment' % (target, proven, len(runs)))
    return flat


def check_free_unreached(stock_img, flat, base, rep, cave=None):
    """No code reachable from any task of the table enters a span a mod claims
    as free space. The table holds every task, not only the boot-critical ones.
    A profile's named cave `cave` is proven by its hash instead."""
    free = [(mod, lo, hi) for mod, kind, lo, hi in flat
            if kind == 'free' and not (cave and cave[0] <= lo and hi <= cave[1])]
    if cave and not free:
        return
    reach = bootcrit.closure(stock_img, base, every_task=True)
    entered = [(mod, lo, hi) for mod, lo, hi in free if reach.hit(lo, hi)]
    for mod, lo, hi in entered[:SHOWN]:
        rep.fail('main: %s free space %#x-%#x is entered by code the tasks reach' % (mod, base + lo, base + hi))
    if not entered:
        rep.ok('main: no code reachable from the %d routines of every task enters the free space'
               % len(reach.entries))


def dsp_range(stock_img, dsp):
    """(lo, hi) file offsets of the DSP program in the stock MAIN image, or
    None when the player has none or this image does not carry it."""
    if dsp is None:
        return None
    try:
        start = dsppatch.find_blob(stock_img, dsp)
    except sigpatch.NotApplied:
        return None
    return start, start + dsp.length


def check_dsp_writes(stock_img, flat, dsp, start, rep):
    """The DSP program is data to MAIN, so nothing here is boot-critical code;
    what has to hold is that the code a mod placed in its erased run was
    erased, is away from the run's ends and is not pointed at by the program,
    or, in a boot table, that it is inside the cave, which still held its stock
    code, and that the program does not point into it."""
    placed = [(mod, lo, hi) for mod, kind, lo, hi in flat if kind in ('dspfree', 'dspcave')]
    blob = bytes(stock_img[start:start + dsp.length])
    table = isinstance(dsp, dsppatch.DspTable)
    failed = False
    for mod, lo, hi in placed:
        a = dsppatch.program_address(dsp, blob, lo - start)
        b = a + hi - lo
        if table and not (dsp.cave[0] <= a and b <= dsp.cave[1]):
            why = 'not inside the cave %#x-%#x' % dsp.cave[:2]
        elif table and not dsppatch.cave_is_stock(dsp, blob):
            why = 'in a cave that does not hold the code of the firmware it was found in'
        elif not table and set(stock_img[lo:hi]) != {0xFF}:
            why = 'not erased'
        elif not table and not (dsp.free[0] + dsppatch.EDGE <= a and b <= dsp.free[1] - dsppatch.EDGE):
            why = 'not inside the erased run %#x-%#x, %d bytes from its ends' % (dsp.free + (dsppatch.EDGE,))
        else:
            refs = dsppatch.program_references(dsp, blob, a, b)
            why = 'the program points into it, at %#x' % refs[0] if refs else None
        if why:
            failed = True
            rep.fail('dsp: %s places code at %#x-%#x, but it is %s' % (mod, a, b, why))
    edits = sum(1 for _, kind, _, _ in flat if kind == 'dsp')
    if (edits or placed) and not failed:
        rep.ok('dsp: %d words of the DSP program edited, %d bytes of code placed in its %s, which '
               'nothing in the program points into' % (edits, sum(hi - lo for _, lo, hi in placed),
                                                       'cave' if table else 'erased run'))


def check_main_image(stock_img, out_img, profile, spans, rep, dsp=None):
    base = profile.flash_base
    span = dsp_range(stock_img, dsp)
    flat = check_writes('main', stock_img, out_img, profile, spans, rep, dsp=span)
    if span:
        check_dsp_writes(stock_img, flat, dsp, span[0], rep)
    cave = None
    if profile.cave:
        try:
            cave = sigpatch.find_cave(sigpatch.Image(profile, stock_img))
        except sigpatch.NotApplied:
            pass
    if spans is None:
        summarize_writes('main', flat, base, rep)
    closure = bootcrit.closure(stock_img, base)
    if 'UpDtae_TASK' not in closure.roots:
        if spans is not None:
            rep.fail('main: the update task is not in the image, so the boot-critical set cannot be built')
            return
        if any(kind != 'free' for _, kind, _, _ in flat):
            rep.unproven('main: no update task found in this image, so a write to existing code cannot be placed '
                         'inside or outside the boot-critical set')
        check_free_unreached(stock_img, flat, base, rep, cave)
        rep.not_checked.append('the boot-critical set: this image has no task of the names it is built from')
        return
    rep.ok('main: boot-critical set built from %s: %d routines, %d bytes, %d ranges'
           % (', '.join(sorted(closure.roots)) or 'no task', len(closure.entries),
              sum(hi - lo for lo, hi in closure.ranges), len(closure.ranges)))
    bad = overlaps(flat, closure.ranges)
    for mod, kind, lo, hi, (rlo, rhi) in bad[:SHOWN]:
        where = closure.routine_at(max(lo, rlo))
        func = ('routine %#x, reached from %s' % (base + where[0], where[1])) if where else 'the vector area or a task descriptor'
        rep.fail('main: %s %s at %#x-%#x overlaps boot-critical code (%s)' % (mod, kind, base + lo, base + hi, func))
    if len(bad) > SHOWN:
        rep.fail('main: %d more overlaps' % (len(bad) - SHOWN))
    if not bad:
        rep.ok('main: %d declared writes, none in boot-critical code' % len(flat))
    check_free_unreached(stock_img, flat, base, rep, cave)
    rep.not_checked.append('%d indirect calls and jumps in the walked code are not followed, so the boot-critical set '
                           'is a lower bound' % closure.unresolved)
    missing = [n for n in bootcrit.TASK_ROOTS if n not in closure.roots]
    if closure.roots:
        rep.not_checked.append('no task of these names in this image, so no root from them: %s' % ', '.join(missing))


def match_known_mods(target, stock_img, out_img, model, version, rep):
    """Say which of this project's mods the change is, when the patcher would
    write the very same bytes into the stock image: each mod row on its own,
    then all of them together (they share one placement). Whatever is left is
    reported as not from any of them."""
    import patch_update
    module = patch_update.TARGETS[target][2]
    available = module.mods_for(model)
    registry = patch_update.registry(model)
    rows = {row: [n for n in knobs if registry.get(n, (None,))[0] == target and version in available[n].fw_versions]
            for row, knobs in patch_update.conf_rows().items()}
    rows = {row: names for row, names in rows.items() if names}
    candidates = list(rows.items()) + [('all the mods together', [n for names in rows.values() for n in names])]
    changed = changed_ranges(stock_img, out_img)
    covered, found = [], []
    for row, names in candidates:
        with contextlib.redirect_stdout(io.StringIO()):
            patched = module.patch(stock_img, names, model, {}, {})
        ranges = changed_ranges(stock_img, patched)
        if ranges and all(out_img[lo:hi] == patched[lo:hi] for lo, hi in ranges):
            found.append(row)
            covered += ranges
    left = [r for r in changed if not any(lo <= r[0] and r[1] <= hi for lo, hi in covered)]
    if found:
        rep.note('%s: writes the same bytes as our patcher does for: %s' % (target, ', '.join(found)))
    if left:
        rep.note('%s: %d of %d changed ranges are in none of our mods' % (target, len(left), len(changed)))


def default_main_profile(model):
    """The profile of a MAIN image nobody wrote patches for: where it is
    loaded, and nothing about free space, so only erased bytes that nothing
    points into and no task reaches count as free."""
    return sigpatch.ImageProfile(flash_base=modelfw.main_base(model), loadtab_off=0, loadtab_n=0,
                                 ram_base=None, ram_end=None, byteorder='<')


def summarize_writes(target, flat, base, rep):
    """What a change that came with no declaration did, in firmware addresses."""
    names = {'free': 'in erased space', 'site': 'over existing code', 'record': 'in the load table',
             'dsp': 'over words of the DSP program', 'dspfree': 'in the erased run of the DSP program',
             'dspcave': 'in the cave of the DSP program'}
    for kind in ('site', 'free', 'record', 'dsp', 'dspfree', 'dspcave'):
        runs = [(lo, hi) for _, k, lo, hi in flat if k == kind]
        if not runs:
            continue
        shown = ', '.join('%#x-%#x' % (base + lo, base + hi) for lo, hi in runs[:SHOWN])
        more = ' and %d more' % (len(runs) - SHOWN) if len(runs) > SHOWN else ''
        rep.note('%s: %d bytes in %d ranges %s: %s%s'
                 % (target, sum(hi - lo for lo, hi in runs), len(runs), names[kind], shown, more))


def load_records(img, profile):
    return [struct.unpack_from(profile.byteorder + 'III', img, profile.loadtab_off + RECORD_BYTES * k)
            for k in range(profile.loadtab_n)]


def known_gui_sites(stock_img, profile):
    """Byte ranges of the display image where a mod may rewrite code: the
    routine sites patch_gui.py ships, found in the stock image by their
    instructions, and the bytes of the known rewrites.
    All are inside pixel renderers (the centre waveform's and the overview
    strip's), which the display task runs only when it draws a waveform, long
    after the load table, the vector copy and the update screen are up. The
    image has no call graph, so any other site cannot be shown to stay off the
    boot path."""
    image = sigpatch.Image(profile, stock_img)
    ranges = []
    for name in KNOWN_GUI_SITES:
        mod = patch_gui.MODS[name]
        for at in sigpatch.sig_hits(image, mod.sig):
            ranges.append((at + 2 * mod.start, at + 2 * mod.end))
    shipped = [name for name in KNOWN_GUI_TABLES if name in patch_gui.TABLES]
    spans = {}
    sigpatch.patch_tables(stock_img, patch_gui.TABLES, shipped, 'validate', {}, spans)
    for name in shipped:
        ranges += [(lo, lo + size) for lo, size, _ in spans.get(name, [])]
    return ranges


def check_gui_image(stock_img, out_img, profile, spans, rep):
    table = (profile.loadtab_off, profile.loadtab_off + RECORD_BYTES * profile.loadtab_n)
    flat = check_writes('gui', stock_img, out_img, profile, spans, rep, table)
    before, after = load_records(stock_img, profile), load_records(out_img, profile)
    protected = [table]
    for dest, src, size in before:
        if dest == VECTOR_COPY_DEST and size:
            protected.append((src - profile.flash_base, src - profile.flash_base + size))
    known = known_gui_sites(stock_img, profile)
    unknown = [(mod, lo, hi) for mod, kind, lo, hi in flat
               if kind == 'site' and not any(a <= lo and hi <= b for a, b in known)]
    for mod, lo, hi in unknown[:SHOWN]:
        rep.fail('gui: %s rewrites %#x-%#x, which is not one of the known display sites, and the display image has '
                 'no call graph to show it stays off the boot path' % (mod, lo, hi))
    if flat and not unknown:
        rep.ok('gui: every site rewritten is one of the known pixel-renderer sites')
    filled = 0
    for k, (was, now) in enumerate(zip(before, after)):
        if was == now:
            continue
        dest, src, size = now
        if was[2] != 0:
            rep.fail('gui: load record %d was in use and changed' % k)
        elif not (profile.ram_base <= dest and dest + size <= profile.ram_end):
            rep.fail('gui: load record %d puts %d bytes at %#x, outside on-chip RAM' % (k, size, dest))
        elif not 0 <= src - profile.flash_base <= len(out_img) - size:
            rep.fail('gui: load record %d reads from outside the image' % k)
        elif any(d < dest + size and dest < d + z for j, (d, _, z) in enumerate(after) if j != k and z):
            rep.fail('gui: load record %d overlaps another record in RAM' % k)
        else:
            filled += 1
    if filled:
        rep.ok('gui: %d load records filled, each a free one inside on-chip RAM with no overlap' % filled)
    bad = overlaps([s for s in flat if s[1] != 'record'], protected)
    for mod, kind, lo, hi, _ in bad:
        rep.fail('gui: %s %s at offset %#x-%#x overlaps the load table or the vector copy' % (mod, kind, lo, hi))
    if not bad:
        rep.ok('gui: no write touches the load table or the vector copy')
    rep.not_checked.append('the display image has no call graph: only its load table, vector copy and undeclared '
                           'bytes are protected, not the routines it runs at boot')


RESIDUAL = (
    'the patched code has not been run: nothing here boots the modified image through the real boot path, and no '
    'mod has been flashed to a deck',
    'that the boot loader and emergency updater in a unit equal the ones in the file (only a flash read-out proves it)',
    'whether the updater accepts an update of equal version',
)


def check_other_files(directory, model, rep):
    """The files of a one-file-per-section update that sit next to the MAIN
    file: GUI, DRIV and PANL are never patched, so each must be the shipped
    file or, failing that, still hold together (S-record sections with their
    CRC-16). Returns [(name, path)] of the ones found, to be carried along."""
    found = []
    for name, digest in modelfw.other_files(model):
        path = os.path.join(directory, name)
        if not os.path.exists(path):
            rep.fail('%s is missing next to the MAIN file' % name)
            continue
        with open(path, 'rb') as f:
            data = f.read()
        found.append((name, path))
        if hashlib.sha256(data).hexdigest() == digest:
            rep.ok('%s: the shipped file, sha256 %s' % (name, digest[:12]))
        elif section_kind(data) == 'srec' and Srec(data).crc_ok:
            rep.note('%s: not the shipped file, but its CRC-16 verifies' % name)
        elif section_kind(data) == 'srec':
            rep.fail('%s: not the shipped file and its CRC-16 does not verify' % name)
        else:
            rep.note('%s: not the shipped file and its format has no checksum to verify' % name)
    return found


def verify(stock_data, out_data, model, targets=None, spans=None, profiles=None):
    """Report for a patched update. `targets` are the images a mod was asked
    for ('main', 'gui'; None means any the model has), `spans` is
    {target: {mod: [(offset, size, kind)]}} from the patch engine, `profiles`
    is {target: ImageProfile}."""
    rep = Report()
    fw = modelfw.load(model)
    stock, out = Upd(stock_data, fw), Upd(out_data, fw)
    if not stock.complete or not out.complete:
        rep.fail('the section table is longer than the file')
        return rep
    if stock_data == out_data:
        rep.note('the output is the stock file byte for byte')
    spans, profiles = spans or {}, profiles or {}
    check_untouched(stock, out, fw, targets, rep)
    if not spans:
        for i, (a, b) in enumerate(zip(stock.sections, out.sections)):
            if a != b:
                rep.note('section %d (%s): changed, %d -> %d bytes (%+d)'
                         % (i + 1, a[:LABEL_SIZE].decode('latin1').rstrip(), len(a), len(b), len(b) - len(a)))
    images = decode_pair(stock, out, fw, model, targets, rep)
    for target, (stock_img, out_img) in images.items():
        profile = profiles.get(target)
        if spans.get(target) is None:
            label = stock.sections[fw.sections[target] - 1][:LABEL_SIZE].decode('latin1')
            match_known_mods(target, stock_img, out_img, model, label_version(label), rep)
        if target == 'main':
            profile = profile or default_main_profile(model)
            check_main_image(stock_img, out_img, profile, spans.get('main'), rep, modelfw.dsp_profile(model))
        elif profile is None:
            rep.unproven('gui changed, and there is no image profile of the display for %s: its load table and '
                         'known renderer sites are not known, and the image has no call graph' % model)
        else:
            check_gui_image(stock_img, out_img, profile, spans.get('gui'), rep)
    rep.not_checked += RESIDUAL
    if 'gui' in fw.sections:
        rep.not_checked.append('recovery after a bad display image: its loader sits below the image in the display '
                               'flash and is in no update')
    return rep


LABEL = re.compile(r'(.+?)\s*(MAIN|GUI|DRIV|PANL)\s*Ver(\d+\.\d+)')

# The product part of a section label, as the updater of each player writes it:
# the title of its profile.
PLAYERS = {modelfw.cdj_model.load(m).title: m for m in modelfw.cdj_model.list_models()}


def label_version(label):
    found = LABEL.match(label)
    return found.group(3) if found else None


def identify(data):
    """(player id, section role, version) from the section labels of a file, or
    None. The MAIN label names the player; the CDJ-2000 and CDJ-900 files hold
    one section each, so whichever of the four it is names it."""
    sizes, pos = modelfw.parse_manifest(data)
    labels, at = [], pos
    for size in sizes or [len(data)]:
        labels.append(data[at:at + LABEL_SIZE].decode('latin1'))
        at += size
    found = [m for m in map(LABEL.match, labels) if m]
    found.sort(key=lambda m: m.group(2) != 'MAIN')
    if not found or found[0].group(1) not in PLAYERS:
        return None
    return PLAYERS[found[0].group(1)], found[0].group(2), found[0].group(3)


def find_stock(model, directory):
    """Path of the official MAIN file of `model` (the one models/<id>.conf
    pins by sha256) in the folder of the file under test, the checkout's fw/
    tree or the model's firmware folder; None when there is none."""
    from launcher import layout
    from launcher import model as cdj_model
    name, digest = modelfw.main_file(model)
    lay = layout.Layout()
    folders = [directory, cdj_model.extract_dir(lay, cdj_model.load(model))]
    for root, _, _ in os.walk(os.path.join(lay.root, 'fw')):
        folders.append(root)
    for folder in folders:
        for candidate in (name, cdj_model.DISPLAY_UPD_IMAGE):
            found = os.path.join(folder, candidate)
            if os.path.isfile(found):
                with open(found, 'rb') as f:
                    if hashlib.sha256(f.read()).hexdigest() == digest:
                        return found
    return None


def check_alone(data, model, rep):
    """The checks that need no official copy: every section, its checksums and
    the packed MAIN against the window of the player's updater."""
    fw = modelfw.load(model)
    upd = Upd(data, fw)
    if not upd.complete or not upd.sections:
        rep.fail('the section table is longer than the file, or there is none')
        return
    rep.ok('section table: %d sections, %d bytes of header, %d after the last section'
           % (len(upd.sections), upd.header, len(upd.trailer)))
    for i, section in enumerate(upd.sections):
        name = 'section %d (%s)' % (i + 1, section[:LABEL_SIZE].decode('latin1').rstrip())
        kind = section_kind(section)
        if i == fw.sections['main'] - 1:
            if kind == 'srec':
                check_srec_section(i, None, section, fw, model, rep)
            else:
                rep.fail('%s: the MAIN section is not S-records' % name)
        elif kind == 'srec':
            parsed = Srec(section)
            if parsed.errors:
                rep.fail('%s: %s' % (name, parsed.errors[0]))
            elif parsed.crc_ok:
                rep.ok('%s: CRC-16 trailer and %d S-record checksums verify' % (name, len(parsed.lines)))
            else:
                rep.fail('%s: CRC-16 trailer does not verify' % name)
        elif kind == 'blob':
            check_blob_section(i, None, section, rep)
        else:
            rep.note('%s: a format with no checksum to verify' % name)
    rep.not_checked += [
        'the boot loader and emergency updater against the official ones (no official copy to compare with)',
        'what the change to MAIN or the display image is, and whether it touches boot-critical code',
    ] + list(RESIDUAL)


def profiles_for(model):
    profiles = {}
    for target, module in (('main', patch_main), ('gui', patch_gui)):
        found = module.PATCH_SETS.get(model)
        if found:
            profiles[target] = found.profile
    return profiles


def check_file(path, stock_path, model):
    """Report for one update file of unknown origin; `model` None means read
    it from the file. Every checksum is recomputed from the file's own bytes
    and compared with what the file stores; the official file is only used to
    see what differs from it."""
    rep = Report()
    with open(path, 'rb') as f:
        data = f.read()
    who = identify(data)
    if model is None:
        if who is None:
            rep.fail('no section label of a known player: this is not a firmware update, or one for a player '
                     'this project has no profile for (--model names one)')
            return rep
        model = who[0]
    version = who[2] if who else None
    info = modelfw.cdj_model.load(model)
    rep.note('%s update%s' % (info.title, ' v' + version if version else ''))
    fw = modelfw.load(model)
    name, digest = modelfw.main_file(model)
    here = os.path.dirname(os.path.abspath(path))
    if fw.bare and who and who[1] != 'MAIN':
        main_path = os.path.join(here, name)
        if not os.path.isfile(main_path):
            rep.fail('%s needs its MAIN file %s beside it' % (os.path.basename(path), name))
            return rep
        return check_file(main_path, stock_path, model)

    sha = hashlib.sha256(data).hexdigest()
    if sha == digest:
        rep.ok('%s is the official %s update: its sha256 is the one models/%s.conf pins' % (name, info.title, model))
        rep.not_checked.append(RESIDUAL[1])
        if fw.bare:
            check_other_files(here, model, rep)
        return rep
    rep.note('modified, or another version: sha256 %s... is not the official file of any version this project '
             'pins (an edited file never matches; its own checksums are checked below)' % sha[:12])

    if stock_path is None:
        stock_path = find_stock(model, here)
        if version and version != info.fw_version:
            stock_path = None
    elif not os.path.isfile(stock_path):
        rep.fail('--stock %s does not exist' % stock_path)
        return rep
    if stock_path is None:
        check_alone(data, model, rep)
        rep.unproven('no official %s update of version %s to compare with (this project pins v%s): what the file '
                     'changes, and whether that is safe, could not be checked'
                     % (info.title, version or '?', info.fw_version))
        return rep
    with open(stock_path, 'rb') as f:
        stock_data = f.read()
    checked = Report()
    if hashlib.sha256(stock_data).hexdigest() == digest:
        checked.ok('compared with the official update %s' % stock_path)
    else:
        checked.unproven('%s is not an official file this project knows, so a change it shares with the file '
                         'under test goes unseen' % stock_path)
    verified = verify(stock_data, data, model, profiles=profiles_for(model))
    verified.lines = rep.lines + checked.lines + verified.lines
    if fw.bare:
        check_other_files(here, model, verified)
    return verified


def main():
    explicit = '--model' in sys.argv[1:]
    model, args = modelfw.model_id(sys.argv[1:])
    stock = None
    if '--stock' in args:
        i = args.index('--stock')
        if i + 1 >= len(args):
            raise SystemExit('--stock needs a file')
        stock, args = args[i + 1], args[:i] + args[i + 2:]
    if len(args) == 1:
        rep = check_file(args[0], stock, model if explicit else None)
    elif len(args) == 2 and stock is None:
        with open(args[0], 'rb') as f:
            stock_data = f.read()
        with open(args[1], 'rb') as f:
            out = f.read()
        rep = verify(stock_data, out, model, profiles=profiles_for(model))
        if modelfw.load(model).bare:
            check_other_files(os.path.dirname(os.path.abspath(args[1])), model, rep)
    else:
        raise SystemExit(__doc__)
    print(rep.text())
    sys.exit(rep.exit_code)


if __name__ == '__main__':
    main()
