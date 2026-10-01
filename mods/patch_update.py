#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Patch a real Pioneer CDJ-2000NXS2 firmware update (.UPD) with a mod.

    usage: patch_update.py <in.UPD> <out.UPD> [<mod> ...]
           patch_update.py --list

With no mod named, every section is copied through untouched -- a repack
that reproduces the input byte for byte, useful for proving the container
handling itself is not what changed.

Every mod here also has a --list entry in patch_gui.py or patch_main.py,
since that is the tool that actually knows how to splice it into its image;
this is the container-level wrapper around them. Only mods that patch real
firmware code are offered -- an emulator-only knob such as high_fps or
live_clock (emulator/mods/mods.conf) has nothing to write into a .UPD and
does not appear here.

    1. Split the update into its sections (the same manifest split_update.py
       reads: CRLF-decimal lengths, then the sections concatenated).
    2. Decode the target section to the raw image patch_gui.py/patch_main.py
       works on: an LZSS stream for the GUI section, Motorola S-records
       wrapping the same LZSS codec for the MAIN section.
    3. Apply every requested mod for that image in one patch() call, so they
       share one placement and cannot collide.
    4. Re-encode (lzss_encode.py) and recompute the section's own length
       field and checksum(s) -- gui_encode.py, main_encode.py.
    5. Rewrite the manifest (only the patched sections' sizes change) and
       concatenate the sections back into a new .UPD.

Every mod declares the firmware version(s) of its target image it was
verified against (MODS[name].fw_versions). patch_update.py reads the
update's own version string for that image and refuses a mismatch -- pass
--force-version to patch anyway, at your own risk.

THIS IS UNTESTED ON REAL HARDWARE. The container repacks correctly and the
patched image has been proven identical to patch_gui.py's/patch_main.py's own
output (see emulator/tests/), but nobody has flashed one of these into a real
deck. Flashing a modified firmware update is entirely at your own risk: keep
the original .UPD to restore from, and expect that a mistake here can require
a service-mode recovery or worse.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, '..', 'scripts', 'firmware')
sys.path.insert(0, HERE)
sys.path.insert(0, FIRMWARE)

import patch_gui
import patch_main
import gui_decode
import gui_encode
import main_decode
import main_encode

# Which section of the v1.87 container (models/cdj2000nxs2.conf's
# MODEL_GUI_SECTION / MODEL_MAIN_SECTION) each target lives in. The
# CDJ-2000NXS2's .UPD is the only container shape this tool reads; a model
# whose update ships as several files (the CDJ-2000) is out of scope.
SECTION_OF = {'gui': 1, 'main': 3}

# target -> (decode(section) -> (header, declared, avail, image),
#            encode(header_or_section, image) -> section, module owning MODS/patch())
#
# gui's encode() only needs the section's 32-byte header (it is a flat LZSS
# blob with nothing else to preserve); main's needs the whole original
# section, since its own encode() carries the bootloader/emergency-updater
# S-records before the image forward unchanged -- see main() below.
TARGETS = {
    'gui': (gui_decode.decode, gui_encode.encode, patch_gui),
    'main': (main_decode.decode, main_encode.encode, patch_main),
}


def registry():
    """{mod name: (target, module)} across every target module."""
    reg = {}
    for target, (_, _, mod) in TARGETS.items():
        for name, m in mod.MODS.items():
            reg[name] = (target, mod)
    return reg


def parse_manifest(data):
    """(section sizes, header length) -- see split_update.py."""
    sizes, pos = [], 0
    while True:
        m = re.match(rb'(\d+)\r\n', data[pos:pos + 32])
        if not m:
            break
        sizes.append(int(m.group(1)))
        pos += m.end()
    return sizes, pos


def section_version(header):
    m = re.search(r'Ver(\d+\.\d+)', header)
    if not m:
        raise SystemExit('patch_update: no version string in section header %r' % header)
    return m.group(1)


def die(msg):
    raise SystemExit('patch_update: %s' % msg)


def main():
    args = sys.argv[1:]
    force_version = '--force-version' in args
    args = [a for a in args if a != '--force-version']

    if args == ['--list']:
        for name, (target, mod) in registry().items():
            m = mod.MODS[name]
            print('%-8s [%s Ver%s] %s' % (name, target, '/'.join(m.fw_versions), m.what))
        return

    if len(args) < 2:
        raise SystemExit(__doc__)
    in_path, out_path, names = args[0], args[1], args[2:]

    reg = registry()
    unknown = [n for n in names if n not in reg]
    if unknown:
        die('unknown mod(s) %s -- see --list' % ', '.join(unknown))

    by_target = {}
    for name in names:
        target, mod = reg[name]
        by_target.setdefault(target, []).append(name)

    with open(in_path, 'rb') as f:
        data = f.read()
    sizes, hdrlen = parse_manifest(data)
    off = hdrlen
    sections = []
    for size in sizes:
        sections.append(data[off:off + size])
        off += size

    for target, target_names in by_target.items():
        decode, encode, mod = TARGETS[target]
        idx = SECTION_OF[target] - 1
        if idx >= len(sections):
            die('%s targets section %d, but this update only has %d'
                % (target, idx + 1, len(sections)))
        section = sections[idx]
        header, declared, avail, image = decode(section)
        version = section_version(header)
        for name in target_names:
            wanted = mod.MODS[name].fw_versions
            if version not in wanted and not force_version:
                die('%s is verified only for %s Ver%s; this update carries Ver%s '
                    '-- pass --force-version to patch anyway (untested)'
                    % (name, target, '/'.join(wanted), version))
        patched = mod.patch(image, target_names)
        # gui's section has nothing but the header and the LZSS blob; main's
        # carries the bootloader and emergency-updater S-records too, so its
        # encode() needs the whole original section to reproduce them.
        sections[idx] = encode(section, patched) if target == 'main' else encode(section[:32], patched)
        print('patch_update: section %d (%s, Ver%s) %d -> %d bytes'
              % (idx + 1, target, version, len(section), len(sections[idx])))

    manifest = b''.join(b'%d\r\n' % len(s) for s in sections)
    with open(out_path, 'wb') as f:
        f.write(manifest + b''.join(sections))
    print('patch_update: wrote %s (%d bytes)' % (out_path, hdrlen + sum(len(s) for s in sections)))
    print('patch_update: UNTESTED ON REAL HARDWARE -- flash at your own risk')


if __name__ == '__main__':
    main()
