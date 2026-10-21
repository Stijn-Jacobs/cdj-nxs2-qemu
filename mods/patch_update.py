#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Patch a real Pioneer firmware update (.UPD) with a mod.

    usage: patch_update.py [--model <id>] <in.UPD> <out.UPD> [<mod> ...]
           patch_update.py [--model <id>] --list

--model picks the player the update is for (default cdj2000nxs2); its profile
(models/<id>.conf) says which section holds MAIN and where the packed
application starts. Every player's one-file update has the container read
here and the same two checksums; a mod is offered only for a player that has
a patch for it. The CDJ-2000 and CDJ-900 ship one file per section: name their
MAIN file (C2KMAIN.UPD, C900MAIN.UPD), which is one section with no manifest
before it. The other three files are never patched: they are checked against
the shipped ones and copied next to the output.

A mod is named by its folder, mods/<mod>/ (three_band, osc, ableton_link,
...), and brings every patch its mod.conf switches on; --list shows them. A
single patch (patch_gui.py --list, patch_main.py --list) can still be named on
its own.
With no mod named, every section is copied through untouched -- a repack
that reproduces the input byte for byte, useful for proving the container
handling itself is not what changed.

The update is taken as it comes, already modded or not. A mod already in it
is left alone (patching a patched update changes nothing more); a mod whose
site was changed by someone else, or whose version does not match, is left out
and reported while the others are applied; the section lengths and checksums
are rewritten for what is written, and a checksum that did not match on the
way in is no reason to refuse it.

Every mod here also has a --list entry in patch_gui.py or patch_main.py,
since that is the tool that actually knows how to splice it into its image;
this is the container-level wrapper around them. Only mods that patch real
firmware code are offered -- an emulator-only knob such as high_fps or
live_clock (emulator/mods/<mod>/mod.conf) has nothing to write into a .UPD and
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
update's own version string for that image and leaves out a mod it was not
verified for -- pass --force-version to patch anyway, at your own risk.

THIS IS UNTESTED ON REAL HARDWARE. The container repacks correctly and the
patched image has been proven identical to patch_gui.py's/patch_main.py's own
output (see emulator/tests/), but nobody has flashed one of these into a real
deck. Flashing a modified firmware update is entirely at your own risk: keep
the original .UPD to restore from, and expect that a mistake here can require
a service-mode recovery or worse.
"""
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, '..', 'scripts', 'firmware')
sys.path.insert(0, HERE)
sys.path.insert(0, FIRMWARE)

import modelfw
import patch_gui
import patch_main
import gui_decode
import gui_encode
import main_decode
import main_encode
import validate
from launcher import layout, mods as mod_rows

# target -> (decode(section, fw) -> (header, declared, avail, image),
#            encode(section, image, fw) -> section, module owning mods_for()/patch())
#
# gui's section is a flat LZSS blob with nothing to keep but its 32-byte
# header; main's carries the bootloader and emergency-updater S-records before
# the image, which its encode() copies forward from the whole original section.
TARGETS = {
    'gui': (lambda section, fw: gui_decode.decode(section),
            lambda section, image, fw: gui_encode.encode(section[:32], image), patch_gui),
    'main': (lambda section, fw: main_decode.decode(section, fw.main_off, verify=False),
             lambda section, image, fw: main_encode.encode(section, image, fw.main_off), patch_main),
}


def registry(model):
    """{mod name: (target, module)} across every target module."""
    reg = {}
    for target, (_, _, mod) in TARGETS.items():
        for name in mod.mods_for(model):
            reg[name] = (target, mod)
    return reg


def conf_rows():
    """{mod: the registry names of its knobs}. A knob is named CDJ_GUI_<NAME>
    or CDJ_MAIN_<NAME> after its patch_gui/patch_main entry, so one mod
    (three_band) stands for every patch it switches on."""
    return {row.key: [re.sub(r'^CDJ_(GUI|MAIN)_', '', knob).lower() for knob in row.env.split('+')]
            for row in mod_rows.load(layout.Layout())}


def section_version(header):
    m = re.search(r'Ver(\d+\.\d+)', header)
    if not m:
        raise SystemExit('patch_update: no version string in section header %r' % header)
    return m.group(1)


def die(msg):
    raise SystemExit('patch_update: %s' % msg)


def main():
    model, args = modelfw.model_id(sys.argv[1:])
    force_version = '--force-version' in args
    args = [a for a in args if a != '--force-version']

    reg = registry(model)
    rows = {key: [n for n in knobs if n in reg] for key, knobs in conf_rows().items()}

    if args == ['--list']:
        for key, patches in rows.items():
            if patches:
                versions = sorted({'%s Ver%s' % (reg[n][0], '/'.join(reg[n][1].mods_for(model)[n].fw_versions))
                                   for n in patches})
                print('%-12s [%s] %s' % (key, ', '.join(versions), ' '.join(patches)))
        in_rows = {n for patches in rows.values() for n in patches}
        for name, (target, mod) in reg.items():
            if name not in in_rows:
                m = mod.mods_for(model)[name]
                print('%-12s [%s Ver%s] %s' % (name, target, '/'.join(m.fw_versions), m.what))
        return

    if len(args) < 2:
        raise SystemExit(__doc__)
    in_path, out_path = args[0], args[1]

    # A row's name wins over a patch of the same name: `osc` is the whole osc
    # row (osc + oscbeat), not its first patch alone.
    names = []
    for arg in args[2:]:
        if rows.get(arg):
            expanded = rows[arg]
        elif arg in rows:
            die('%s has nothing to write into a %s .UPD' % (arg, model))
        elif arg in reg:
            expanded = [arg]
        else:
            die('unknown mod %s -- see --list' % arg)
        names += [n for n in expanded if n not in names]

    by_target = {}
    for name in names:
        target, mod = reg[name]
        by_target.setdefault(target, []).append(name)

    with open(in_path, 'rb') as f:
        data = f.read()
    fw = modelfw.load(model)
    sizes, hdrlen = modelfw.parse_manifest(data)
    if fw.bare:
        if sizes:
            die('%s keeps each section in a file of its own: give the MAIN one, not a container' % model)
        sizes = [len(data)]
    off = hdrlen
    sections = []
    for size in sizes:
        sections.append(data[off:off + size])
        off += size
    trailer = data[off:]

    report, spans, profiles = {}, {}, {}
    for target, target_names in by_target.items():
        decode, encode, mod = TARGETS[target]
        if target not in fw.sections:
            die('%s has no %s section' % (model, target))
        idx = fw.sections[target] - 1
        if idx >= len(sections):
            die('%s targets section %d, but this update only has %d'
                % (target, idx + 1, len(sections)))
        section = sections[idx]
        header, declared, avail, image = decode(section, fw)
        version = section_version(header)
        mods = mod.mods_for(model)
        usable = []
        for name in target_names:
            wanted = mods[name].fw_versions
            if version in wanted or force_version:
                usable.append(name)
            else:
                report[name] = ('skipped', 'verified only for %s Ver%s; this update carries Ver%s '
                                '-- pass --force-version to patch anyway (untested)'
                                % (target, '/'.join(wanted), version))
                print('patch_update: %s not applied: %s' % (name, report[name][1]))
        patched = mod.patch(image, usable, model, report, spans.setdefault(target, {})) if usable else image
        if patched == image:
            continue
        profiles[target] = mod.PATCH_SETS[model].profile
        sections[idx] = encode(section, patched, fw)
        print('patch_update: section %d (%s, Ver%s) %d -> %d bytes'
              % (idx + 1, target, version, len(section), len(sections[idx])))

    if names and not any(state != 'skipped' for state, _ in report.values()):
        die('nothing to patch: %s' % '; '.join('%s: %s' % (n, why) for n, (_, why) in report.items()))

    manifest = b'' if fw.bare else b''.join(b'%d\r\n' % len(s) for s in sections)
    output = manifest + b''.join(sections) + trailer
    checked = validate.verify(data, output, model, set(by_target), spans, profiles)
    others = validate.check_other_files(os.path.dirname(os.path.abspath(in_path)), model, checked) if fw.bare else []
    print(checked.text())
    if checked.failed:
        die('%s fails validation; nothing written' % out_path)
    with open(out_path, 'wb') as f:
        f.write(output)
    for name, path in others:
        copy = os.path.join(os.path.dirname(os.path.abspath(out_path)), name)
        if os.path.abspath(path) != copy:
            shutil.copyfile(path, copy)
    print('patch_update: wrote %s (%d bytes)' % (out_path, len(output)))
    print('patch_update: UNTESTED ON REAL HARDWARE -- flash at your own risk')


if __name__ == '__main__':
    main()
