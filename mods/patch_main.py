#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply code mods to the unpacked MAIN firmware image.

    usage: patch_main.py <main_unpacked.bin> <patched.bin> <mod> [<mod>...]
           patch_main.py --flash <flash.bin> <patched-flash.bin> <mod> [<mod>...]
           patch_main.py --assemble     rebuild every mod's .bin from its .s
           patch_main.py --list

Same shape as patch_gui.py (see that file for the general mechanism), on the
MAIN image instead of the display one. Two differences follow from MAIN being
executed in place rather than scattered to RAM by a load table:

  1. There is no scatter-load record to add: a mod's routine is written
     directly into a run of erased flash within MAIN's own address space
     (see sigpatch.place_inplace()), which is both its storage and its
     execution address.
  2. --flash patches the MAIN application's packed image directly inside a
     flat NOR/flash buffer (extract/flash.bin, or a deck's own copy), at the
     flash offset make_flash.py places it -- for booting a patched MAIN
     through Pioneer's own bootloader (MAIN_BOOT=flash) rather than via
     -kernel. The window each update is allowed to write is erased first, the
     same way the real updater erases before it writes, so a shorter or
     longer re-encoded stream never leaves stale bytes past its own end.

MAIN is a little-endian SH-4 (the GUI's SH7269 is big-endian), so every mod's
sig/args/literal is still given as the instruction's logical value -- see
sigpatch.ImageProfile's byteorder field, which does the translation.

Most MAIN mods are hooks (Mod.hook): several of them can sit on one firmware
site, which is defined once below and shared by every mod on it. The patch
engine moves the site's span into one trampoline that runs it once and then
calls each selected mod's routine in registry order -- see
sigpatch.trampoline() for the code and the contract a hook routine follows.
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, '..', 'scripts', 'firmware')
sys.path.insert(0, HERE)
sys.path.insert(0, FIRMWARE)
import sigpatch
from sigpatch import Mod
import main_decode
import main_encode
from make_flash import MAIN_END as FLASH_WINDOW_END

# The 938,112-byte run of erased flash at file offset 0x11AF80 (address
# 0x0811AF80-0x08200000), found by the same erased-flash scan
# that found the GUI board's cave. Confirmed here, independently of that
# scan, by decoding every mov.l/mov.w @(disp,PC) instruction in the whole
# image (the same check sigpatch.literal_pool_reads() runs before every
# patch) and finding none of their targets inside this range.
MAIN_PROFILE = sigpatch.ImageProfile(
    flash_base=0x08000000, loadtab_off=0, loadtab_n=0,
    ram_base=0x0811AF80, ram_end=0x08200000, byteorder='<')

# The beat loop's call to the UDP send wrapper: the wrapper's literal, the
# port store into the send descriptor at r15+0x150, the descriptor pointer
# into r5 and the call itself with its delay slot. The beat packet the call
# sends stays reachable after it at *(r15+0x218), r12 still holds the
# wrapper and r10 the fpscr mask the firmware applies before every call.
BEAT_SEND = dict(
    sig=[0x0FC4, 0x7004, 0xDC2E, 0x0F26, 0xE254, 0x4208, 0x352C, 0x026A,
         0x22A9, 0x4C0B, 0x426A, 0x026A, 0xD62A],
    start=2,
    end=11,
)

# The USB-MIDI task's event loop, at its top: the task-state read, the load of
# that state's event mask into r14 and the push of the infinite timeout, all
# before the wait call. Every pass of the loop branches back to the start of
# the span; nothing after it reads r0 or r1 before the wait returns.
MIDI_TASK = dict(
    sig=[0xDAAE, 0xD1AE, 0x6012, 0xE1FF, 0x4008, 0x0E9E, 0x2F16, 0xE601,
         0x67C3, 0x55F3, 0x6452, 0x4A0B, 0xE5FF],
    start=1,
    end=7,
)

# The 1 ms timer interrupt's body (TMU unit 1 channel 1, the tick word
# 0x0B0C5258), at the first of its down-counters: the load of its base, the
# decrement unless already zero and the store. pr is saved before it, r1 is
# not read before the body next writes it and T is set again before it is
# tested.
TIMER_TICK = dict(
    sig=[0xD67E, 0x6262, 0x2228, 0x8B01, 0xA22C, 0x0009, 0xD67C, 0x5261,
         0x2228, 0x8901, 0x72FF, 0x1621, 0xD47A, 0xE5FF],
    start=6,
    end=12,
)

# The Pro DJ Link receive task, right after udp_rcv_dat has filled its 128-byte
# buffer (r10) and the call that takes a memory block for the datagram: the
# block call with its delay slot and the fpscr read that follows it. r0 still
# holds the block call's result, r14 the datagram length, *(r15) the block and
# r15+12 the sender's address; the stock test of r0 after the span loops back
# to the receive when it is non-zero, so a hook that sets r0 swallows the
# datagram.
LINK_RECEIVE = dict(
    sig=[0x26C9, 0xAFE4, 0x466A, 0xD25C, 0x65F3, 0xD75C, 0xE6FF, 0x470B,
         0x6422, 0x016A, 0x2008, 0x21C9, 0x8FD9, 0x416A, 0xD758, 0xE500,
         0x64F2, 0x470B],
    start=3,
    end=10,
)

# The Sentinel task's one call to the status serialiser: the argument offset,
# the serialiser's literal, the call with its delay slot and the two
# instructions after it (the fpscr read and the next literal load, which make
# the span long enough for the jump to the trampoline), between context
# halfwords that make the signature unique. The hooks run after the span; r11
# still holds the deck's own status record and r12 the fpscr mask the firmware
# applies around every call.
STATUS_SEND = dict(
    sig=[0x018C, 0x0B14, 0xE07E, 0x4008, 0xD33E, 0x430B, 0x04FE, 0x046A, 0xD131],
    start=2,
    end=9,
)

# The DJcont output task's loop (the task at 0x082D86AC), after the call that
# recomputes the play position word 0x09947488 from the DSP's frame: the call
# of its DSP-output step with its delay slot and the fpscr reads around it.
# The loop branches back to its wait right after the span, and nothing
# branches into it. r13 holds the DJcont record, r14 the fpscr mask; r6, the
# mask the loop applies on the way back, comes from the block.
DJCONT_PASS = dict(
    sig=[0xB015, 0x64D3, 0x056A, 0x64D3, 0x25E9, 0xD38F, 0x430B, 0x456A,
         0x066A, 0x26E9, 0xAFD2, 0x466A],
    start=2,
    end=10,
)

# The colour-preview publisher's copy of the 600 six-byte overview records
# into the link slot: the copy's call with its delay slot, then the fpscr read
# and the load after it, which make the span long enough for the jump to the
# trampoline. The hooks run after the copy, with the slot's lock still held.
OVERVIEW_PUBLISH = dict(
    sig=[0x65B3, 0xEB00, 0x2E12, 0xD14E, 0x410B, 0x1E21, 0x026A, 0xD547, 0xD644],
    start=3,
    end=8,
)

# The waveform requester's success path after the firmware has re-encoded the
# builder's PWV5 record: the free of the builder's record, then the stores of
# the new record and its size into the shared context (0x0B531EB8/0x0B531EBC).
# The hooks run after the stores and before the record is published. The only
# branch into the span is the `bf` just before it, which lands on its first
# halfword.
DETAIL_CONVERTED = dict(
    sig=[0x8B08, 0xD67E, 0x4A0B, 0x2F66, 0x096A, 0x57F7, 0x2979, 0x496A, 0xA00C,
         0x7F04, 0xD178, 0xDA7A, 0x4A0B, 0x5417, 0xD276, 0x0B6A, 0x5CF4, 0x5DF3,
         0x2BE9, 0x12C7, 0x4B6A, 0x12D8, 0x65F2, 0xD675, 0x460B],
    start=10,
    end=22,
)

# The colour-preview reader's call to the PWV4 tag reader, from the push of
# the tag's entry count to the argument setup before the call: the span has no
# branch and nothing branches into it. r13 holds the open ANLZ handle and the
# saved r5 the track's hash, both the reader's own arguments; the hooks run
# before the reader, which is before the preview is published.
PREVIEW_READ = dict(
    sig=[0x60E3, 0x8801, 0x8F0F, 0x426A, 0xE805, 0x4818, 0x78B0, 0x2F86,
         0x67F3, 0x66F3, 0x55F2, 0x7714, 0x7618, 0xB25E, 0x64D3, 0x0C6A,
         0x2CA9, 0x4C6A, 0x7F04, 0xDE7E],
    start=4,
    end=13,
)

MODS = {
    # The detail-waveform builder's own PWV5 fetch, right after it returns:
    # r15+8/+12 still hold the &out_ptr/&out_size the fetch wrote into (read
    # again a few instructions later, once this call site resumes, to fill
    # the shared ctx's own RGB fields), r10 still holds the fetch's hash
    # argument, and the ANLZ handle this call opened is at r15+16 -- all of
    # it live only in this one span, which is why the hook sits exactly
    # here rather than earlier or later in the same function. r6/r7/r10 and
    # the bsr are reproduced first, so the RGB path is untouched whether or
    # not this mod's own fetches below succeed.
    'wave3data': Mod(
        what='3-band detail waveform data fetch (PWV7)',
        target='main',
        fw_versions=('1.87',),
        sig=[0x67F3, 0x66F3, 0x7708, 0x65A3, 0x760C, 0xDA18, 0xBC7B, 0x1FE5],
        start=0,
        end=8,
        args=[],
        literal=False,
    ),
    # Hooks DETAIL_CONVERTED: writes the words the wave3data mod packed into the
    # re-encoded waveform record, reading its buffer through two words at fixed
    # addresses (see main_wave3data.s).
    'wave3detail': Mod(
        what='3-band detail waveform data (PWV7 in the waveform record)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **DETAIL_CONVERTED,
    ),
    # Hooks OVERVIEW_PUBLISH: writes the PWV6 bands the wave3ovfetch mod kept
    # into the overview payload, reading that mod's buffer through a word at a
    # fixed address (see main_wave3ovfetch.s).
    'wave3ovdata': Mod(
        what='3-band overview data (PWV6 in the overview payload)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **OVERVIEW_PUBLISH,
    ),
    # Hooks PREVIEW_READ: reads and packs the track's PWV6 for wave3ovdata.
    'wave3ovfetch': Mod(
        what='3-band overview data fetch (PWV6)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **PREVIEW_READ,
    ),
    # Hooks PREVIEW_READ: reads the track's PSSI and beat grid and reduces them
    # to the 600-column phrase table phrasedata writes.
    'phrasefetch': Mod(
        what='phrase analysis (PSSI) read and reduced to overview columns',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **PREVIEW_READ,
    ),
    # Hooks OVERVIEW_PUBLISH: writes the phrase table phrasefetch kept into the
    # overview records' spare bits (see main_phrasedata.s).
    'phrasedata': Mod(
        what='phrase colours in the overview payload',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **OVERVIEW_PUBLISH,
    ),
    'abletonlink': Mod(
        what='Ableton Link announcements (UDP 20808)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **BEAT_SEND,
    ),
    'abletonlinkpong': Mod(
        what='Ableton Link measurement replies (UDP 50000)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **LINK_RECEIVE,
    ),
    'osc': Mod(
        what='OSC deck state and load/play/stop/cue/loop events (UDP 50010)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **STATUS_SEND,
    ),
    'oscbeat': Mod(
        what='OSC per-beat output (UDP 50010)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **BEAT_SEND,
    ),
    'tcnet': Mod(
        what='TCNet Opt-IN and Status, once a second (UDP 60000)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **STATUS_SEND,
    ),
    'tcnettime': Mod(
        what='TCNet Time packets, every 20 ms (UDP 60001)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **DJCONT_PASS,
    ),
    'tcnetdata': Mod(
        what='TCNet listener (UDP 65023): Metrics and MetaData to requesting nodes',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **DJCONT_PASS,
    ),
    'usbmidi': Mod(
        what='USB-MIDI start/stop/clock, sending',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **MIDI_TASK,
    ),
    'usbmiditick': Mod(
        what='USB-MIDI start/stop/clock, 1 ms timing',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **TIMER_TICK,
    ),
}


def source(name):
    """Each routine sits in its own mod's folder, mods/<mod>/."""
    return glob.glob(os.path.join(HERE, '*', 'main_%s.s' % name))[0]


def blob_path(name):
    return source(name)[:-2] + '.bin'


def patch(data, names):
    return sigpatch.patch(MAIN_PROFILE, blob_path, data, MODS, names, tag='patch_main')


def patch_flash(flash, names):
    """Patch the packed MAIN application directly inside a flat flash image
    (flash.bin's own bytes, not the unpacked one -kernel loads)."""
    _, image = main_decode.decode_image(flash)
    patched = patch(image, names)
    packed = main_encode.encode_image(patched)
    if len(packed) > FLASH_WINDOW_END - main_decode.MAIN_IMG_OFF:
        raise SystemExit('patch_main: patched image (%d bytes) does not fit '
                         'the flash window' % len(packed))
    out = bytearray(flash)
    lo, hi = main_decode.MAIN_IMG_OFF, FLASH_WINDOW_END
    out[lo:hi] = b'\xff' * (hi - lo)      # erase the window, as the real updater does
    out[lo:lo + len(packed)] = packed
    return bytes(out)


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        for name in MODS:
            obj = os.path.join(tmp, name + '.o')
            subprocess.run(['sh4-linux-gnu-as', '--isa=sh4', '-little',
                            '-I', os.path.dirname(source(name)),
                            '-o', obj, source(name)], check=True)
            subprocess.run(['sh4-linux-gnu-objcopy', '-O', 'binary',
                            obj, blob_path(name)], check=True)
            print('patch_main: wrote %s (%d bytes)'
                  % (blob_path(name), os.path.getsize(blob_path(name))))


def main():
    args = sys.argv[1:]
    if args == ['--assemble']:
        assemble()
        return
    if args == ['--list']:
        for name, mod in MODS.items():
            print('%-8s %s' % (name, mod.what))
        return
    if args[:1] == ['--flash']:
        args = args[1:]
        flasher = True
    else:
        flasher = False
    if len(args) < 3 or any(n not in MODS for n in args[2:]):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        data = f.read()
    out = patch_flash(data, args[2:]) if flasher else patch(data, args[2:])
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
