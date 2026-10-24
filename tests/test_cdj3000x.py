# SPDX-License-Identifier: GPL-2.0-or-later
"""The cdj3000x MAIN mod, USB identity half. The descriptor table and the
bcdDevice routine are rewritten in place in the real 1.87 MAIN; what the
driver serves from them is read back the way the driver reads it. The key
plumbing is run on a dummy key file made here."""
import importlib.util
import os
import struct

import pytest

import modelfw
import patch_main
import patch_update
import sigpatch
import validate
from helpers import mod_row, path, run_script
from test_main_phrase import RET, STACK, Sh4

REAL_MAIN = path('..', 'extract', 'main_unpacked.bin')
REAL_UPD = path('..', 'fw', 'unzipped', 'C2KNXS2.UPD')
needs_main = pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
needs_upd = pytest.mark.skipif(not os.path.exists(REAL_UPD), reason='fw/unzipped/C2KNXS2.UPD not present')

BASE = 0x08000000
DEVICE = 0x56F01C       # the Windows/Serato device descriptor
IAP_DEVICE = 0x56FFA4   # the vendor-class one
STRINGS = 0x56F264      # language ids, then manufacturer, product, MIDI, HID, serial
PIPE_TABLE = 0x56F49C   # five rows of pipe, PIPECFG, PIPEBUF, PIPEMAXP, spare
STRING_SIZES = (4, 46, 26, 36, 34, 26)
BCD_ROUTINE = 0x425208
BOTH = ['x3000ident', 'x3000bcd']


@pytest.fixture(scope='module')
def stock():
    with open(REAL_MAIN, 'rb') as f:
        return f.read()


@pytest.fixture(scope='module')
def patched(stock):
    return patch_main.patch(stock, BOTH, 'cdj2000nxs2', {}, {})


def string_at(image, slot):
    off = STRINGS + sum(STRING_SIZES[:slot])
    return image[off + 2:off + image[off]].decode('utf-16-le')


# -- the descriptors the driver serves ------------------------------------------------

@needs_main
def test_the_device_descriptor_is_a_3000x_with_the_vendor_kept(stock, patched):
    was, now = stock[DEVICE:DEVICE + 18], patched[DEVICE:DEVICE + 18]
    assert struct.unpack_from('<HH', was, 8) == (0x2B73, 0x0005)
    assert struct.unpack_from('<HH', now, 8) == (0x2B73, 0x004E)
    assert now[:8] == was[:8] and now[12:] == was[12:]


@needs_main
def test_the_strings_name_a_3000x_and_each_says_how_long_it_is(stock, patched):
    assert [string_at(stock, k) for k in range(1, 5)] == [
        'Pioneer DJ Corporation', 'CDJ-2000NXS2', 'CDJ-2000NXS2 MIDI', 'CDJ-2000NXS2 HID']
    assert [string_at(patched, k) for k in range(1, 5)] == [
        'AlphaTheta Corporation', 'CDJ-3000X', 'CDJ-3000X MIDI', 'CDJ-3000X HID']
    for k, size in enumerate(STRING_SIZES):
        off = STRINGS + sum(STRING_SIZES[:k])
        assert patched[off] <= size and stock[off] <= size
        assert set(patched[off + patched[off]:off + size]) <= {0}
    assert patched[STRINGS:STRINGS + 4] == stock[STRINGS:STRINGS + 4]
    assert patched[STRINGS + sum(STRING_SIZES[:5]):STRINGS + sum(STRING_SIZES)] == \
        stock[STRINGS + sum(STRING_SIZES[:5]):STRINGS + sum(STRING_SIZES)]


@needs_main
def test_only_the_identity_changes_not_the_configuration_or_the_other_device(stock, patched):
    for lo, hi in validate.changed_ranges(stock, patched):
        assert lo in range(DEVICE + 10, DEVICE + 12) or lo in range(STRINGS, STRINGS + sum(STRING_SIZES)) \
            or lo in range(BCD_ROUTINE, BCD_ROUTINE + 28)
    config = slice(DEVICE + 18, STRINGS)
    assert patched[config] == stock[config]
    assert patched[IAP_DEVICE:IAP_DEVICE + 0x400] == stock[IAP_DEVICE:IAP_DEVICE + 0x400]


@needs_main
def test_the_pid_is_the_one_asked_for(stock, monkeypatch):
    monkeypatch.setenv('CDJ_3000X_PID', '0x0051')
    out = patch_main.patch(stock, ['x3000ident'], 'cdj2000nxs2', {}, {})
    assert struct.unpack_from('<H', out, DEVICE + 10) == (0x0051,)


@pytest.mark.parametrize('text', ['zz', '0x10000', '-1'])
def test_a_pid_that_is_not_16_bit_hex_is_refused(text):
    with pytest.raises(SystemExit):
        modelfw.mod_patch('cdj3000x', 'cdj2000nxs2').product_id({'CDJ_3000X_PID': text})


# -- applying it ----------------------------------------------------------------------

@needs_main
def test_it_is_reported_applied_then_present_and_a_second_run_changes_nothing(stock, patched):
    report = {}
    again = patch_main.patch(patched, BOTH, 'cdj2000nxs2', report, {})
    assert report == {name: ('present', '') for name in BOTH}
    assert again == patched


@needs_main
def test_a_firmware_whose_strings_differ_is_left_alone(stock):
    odd = bytearray(stock)
    odd[STRINGS + 4 + 2] ^= 1
    report = {}
    out = patch_main.patch(bytes(odd), ['x3000ident'], 'cdj2000nxs2', report, {})
    assert report['x3000ident'][0] == 'skipped' and out == bytes(odd)


def test_an_image_without_the_table_is_left_alone():
    report = {}
    image = bytes(range(256)) * 64
    assert patch_main.patch(image, BOTH, 'cdj2000nxs2', report, {}) == image
    assert report['x3000ident'] == ('skipped', 'table found 0 times')


@needs_main
def test_written_ranges_are_what_changed_and_none_is_longer_than_a_call_site(stock, patched):
    spans = {}
    patch_main.patch(stock, BOTH, 'cdj2000nxs2', {}, spans)
    written = sorted((off, off + size) for items in spans.values() for off, size, kind in items)
    assert written == validate.changed_ranges(stock, patched)
    assert all(hi - lo <= validate.MAX_SITE_BYTES for lo, hi in written)


# -- the MIDI-OUT repack --------------------------------------------------------------

MIDI = BOTH + ['x3000midi']


@pytest.fixture(scope='module')
def repacked(stock):
    return patch_main.patch(stock, MIDI, 'cdj2000nxs2', {}, {})


def usb_descriptors(config):
    """Walk a configuration by bLength up to its wTotalLength, the way a host
    enumerates it, returning (offset, bytes) for each descriptor and asserting
    nothing overruns."""
    total = config[2] | config[3] << 8
    out, o = [], 0
    while o < total:
        length = config[o]
        assert length >= 2 and o + length <= total, 'descriptor at %d does not fit' % o
        out.append((o, config[o:o + length]))
        o += length
    assert o == total
    return out, total


def midi_streaming(descs):
    """The MIDIStreaming interface's bNumEndpoints, its header wTotalLength, the
    span its class-specific descriptors actually cover, its jacks and its
    endpoint addresses."""
    in_ms = header = span = None
    jacks, eps, nep = [], [], None
    for o, d in descs:
        if d[1] == 4:
            in_ms = (d[5] == 1 and d[6] == 3)
            if in_ms:
                nep = d[4]
        if in_ms and d[1] == 0x24 and d[2] == 1:
            header = d[5] | d[6] << 8
            span = 0
        if in_ms and header is not None:
            span += d[0]
        if in_ms and d[1] == 0x24 and d[2] in (2, 3):
            jacks.append((d[2], d[3], d[4]))
        if in_ms and d[1] == 5:
            eps.append(d[2])
    return nep, header, span, jacks, eps


@needs_main
@pytest.mark.parametrize('off', [0x56F038, 0x56F132])
def test_both_configurations_parse_and_carry_the_bulk_out_endpoint(stock, repacked, off):
    was, _ = usb_descriptors(stock[off:off + 256])
    assert midi_streaming(was)[0] == 1 and midi_streaming(was)[4] == [0x85]
    descs, total = usb_descriptors(repacked[off:off + 256])
    assert total == 230
    nep, header, span, jacks, eps = midi_streaming(descs)
    assert nep == 2 and sorted(eps) == [0x04, 0x85]
    assert jacks == [(3, 1, 1), (2, 2, 2), (2, 1, 3), (3, 2, 4)]
    assert header == span               # wTotalLength spans exactly what it says it does


def pipe_rows(config):
    """The pipe-table row each endpoint is programmed from, the way the
    firmware's SET_CONFIGURATION walker numbers them: the endpoints of every
    other interface descriptor (alternates included) before it, plus its place
    in its own interface."""
    descs, _ = usb_descriptors(config)
    rows, before, interface = {}, 0, None
    for o, d in descs:
        if d[1] == 4:
            if interface:
                before += interface[4]
            interface, place = d, 0
        elif d[1] == 5:
            rows[(interface[2], interface[3], d[2])] = before + place
            place += 1
    return rows


@needs_main
@pytest.mark.parametrize('off', [0x56F038, 0x56F132])
def test_every_endpoint_is_programmed_from_its_own_pipe_row(stock, repacked, off):
    for image in (stock, repacked):
        for (_, _, address), row in pipe_rows(image[off:off + 256]).items():
            pipe, cfg, _, maxp, _ = struct.unpack_from('<5H', image, PIPE_TABLE + 10 * row)
            assert cfg & 0xF == address & 0xF and bool(cfg & 0x10) == bool(address & 0x80)
            assert pipe >> 12 == (1 if address & 0x80 else 2)
    ins = {a: r for (i, alt, a), r in pipe_rows(repacked[off:off + 256]).items() if i == 3}
    assert ins == {0x85: 1, 0x04: 2}
    assert struct.unpack_from('<5H', repacked, PIPE_TABLE + 10)[0] & 0xF == 5    # MIDI IN keeps pipe 5


@needs_main
def test_the_24_bit_audio_alternate_is_gone(stock, repacked):
    descs, _ = usb_descriptors(repacked[0x56F038:0x56F038 + 256])
    alternates = [d[3] for o, d in descs if d[1] == 4 and d[2] == 1]
    assert alternates == [0, 1]         # the 24-bit alternate 2 is dropped


@needs_main
def test_the_repack_leaves_the_identity_and_the_strings_where_they_were(stock, repacked):
    identity_only = patch_main.patch(stock, BOTH, 'cdj2000nxs2', {}, {})
    pipes = slice(PIPE_TABLE, PIPE_TABLE + 50)
    assert repacked[STRINGS:pipes.start] == identity_only[STRINGS:pipes.start]
    assert repacked[pipes.stop:] == identity_only[pipes.stop:]
    assert repacked[DEVICE:DEVICE + 18] == identity_only[DEVICE:DEVICE + 18]


@needs_main
def test_the_repack_is_declared_data_and_stays_out_of_boot_critical_code(stock):
    spans = {}
    out = patch_main.patch(stock, MIDI, 'cdj2000nxs2', {}, spans)
    assert all(kind == 'data' for _, _, kind in spans['x3000midi'])
    rep = validate.Report()
    validate.check_main_image(stock, out, modelfw.player('cdj2000nxs2').MAIN_PROFILE, spans, rep)
    assert not rep.failed, [text for state, text in rep.lines if state == 'FAIL']


@needs_main
def test_the_repack_is_idempotent(repacked):
    report = {}
    again = patch_main.patch(repacked, ['x3000midi'], 'cdj2000nxs2', report, {})
    assert report['x3000midi'] == ('present', '') and again == repacked


# -- bcdDevice ------------------------------------------------------------------------

def bcd_after(routine, version):
    cpu = Sh4()
    cpu.write(BASE + BCD_ROUTINE, routine)
    cpu.put32(BASE + BCD_ROUTINE + 0x1C, 0x10DDFBA0)
    cpu.write(0x20000, version)
    cpu.r[4] = 0x20000
    cpu.r[15] = STACK
    cpu.pr = RET
    cpu.run(BASE + BCD_ROUTINE)
    return cpu.read(0x10DDFBA0 + 12, 2)


@needs_main
def test_bcd_device_is_the_firmware_version_stock_and_1_40_with_the_mod(stock, patched):
    version = b'1.87'
    assert bcd_after(stock[BCD_ROUTINE:BCD_ROUTINE + 28], version) == bytes([0x87, 0x01])
    assert bcd_after(patched[BCD_ROUTINE:BCD_ROUTINE + 28], version) == bytes([0x40, 0x01])


# -- the whole update -----------------------------------------------------------------

@needs_upd
def test_a_real_update_with_the_mod_passes_the_validator(tmp_path):
    out = tmp_path / 'identity.UPD'
    r = run_script('mods/patch_update.py', REAL_UPD, out, 'cdj3000x')
    assert r.returncode == 0, r.stdout + r.stderr
    assert 'FAIL' not in r.stdout
    again = run_script('mods/validate.py', '--stock', REAL_UPD, out)
    assert again.returncode == 0, again.stdout
    assert 'writes the same bytes as our patcher does for: cdj3000x' in again.stdout


# -- registered and switched on as a mod ----------------------------------------------

def test_registered_for_main_as_two_table_patches_on_1_87():
    mods = patch_main.mods_for('cdj2000nxs2')
    for name in BOTH:
        assert isinstance(mods[name], sigpatch.DataPatch)
        assert mods[name].target == 'main' and mods[name].fw_versions == ('1.87',)


def test_the_mod_row_is_off_by_default_and_says_it_is_experimental():
    row = mod_row('cdj3000x')
    assert (row.env, row.on, row.off, row.default) == ('CDJ_MAIN_X3000IDENT+CDJ_MAIN_X3000BCD+CDJ_MAIN_X3000HS+CDJ_MAIN_X3000IN+CDJ_MAIN_X3000OUT'
                                                         '+CDJ_MAIN_X3000RXBUF+CDJ_MAIN_X3000RXVIEW+CDJ_MAIN_X3000SEGMENTS'
                                                         '+CDJ_MAIN_X3000KEY+CDJ_MAIN_X3000BROWSE'
                                                         '+CDJ_GUI_X3000KEEPWAVE+CDJ_GUI_X3000TRAINPOS', '1', '0', 'off')
    assert row.models == 'cdj2000nxs2'
    assert row.description.startswith('Experimental') and 'CDJ-3000X' in row.description
    assert patch_update.conf_rows()['cdj3000x'] == BOTH + ['x3000hs', 'x3000in', 'x3000out', 'x3000rxbuf', 'x3000rxview',
                                                           'x3000segments', 'x3000key', 'x3000browse',
                                                           'x3000keepwave', 'x3000trainpos']


# -- the key comes from the person's own firmware -------------------------------------

def keysource():
    spec = importlib.util.spec_from_file_location('keysource', path('mods', 'cdj3000x', 'keysource.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


DUMMY = '00112233445566778899aabbccddeeff'


def unpacked_cabinet(tmp_path, text):
    folder = tmp_path / 'cabinet'
    (folder / 'encryption').mkdir(parents=True)
    (folder / 'encryption' / 'key.dat').write_text(text)
    return folder


def test_the_key_is_read_from_the_unpacked_cabinet_of_the_named_firmware(tmp_path):
    folder = unpacked_cabinet(tmp_path, DUMMY + '\n')
    assert keysource().load({'CDJ_3000X_FW': str(folder)}) == bytes.fromhex(DUMMY)


def test_no_firmware_named_means_the_mod_does_not_build():
    with pytest.raises(SystemExit, match='does not build without it'):
        keysource().load({})


@pytest.mark.parametrize('text', ['', DUMMY[:-2], DUMMY + '00', 'g' * 32])
def test_a_key_file_that_is_not_32_hex_characters_is_refused_without_quoting_it(tmp_path, text):
    folder = unpacked_cabinet(tmp_path, text)
    with pytest.raises(SystemExit) as e:
        keysource().load({'CDJ_3000X_FW': str(folder)})
    assert text not in str(e.value) or text == ''


def test_a_folder_without_the_key_file_is_named_as_such(tmp_path):
    with pytest.raises(SystemExit, match='holds no'):
        keysource().load({'CDJ_3000X_FW': str(tmp_path)})


@pytest.mark.parametrize('head, at, kind', [
    (b'LUKS\xba\xbe', 0, 'LUKS container'),
    (b'CD001', 0x8001, 'ISO image'),
    (b'\x53\xef', 0x438, 'ext4 image'),
])
def test_a_package_it_cannot_open_yet_is_named_by_what_it_is(tmp_path, head, at, kind):
    package = tmp_path / 'package'
    package.write_bytes(b'\0' * at + head + b'\0' * 64)
    with pytest.raises(SystemExit, match=kind):
        keysource().load({'CDJ_3000X_FW': str(package)})
