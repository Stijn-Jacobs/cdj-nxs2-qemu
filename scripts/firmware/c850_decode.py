# SPDX-License-Identifier: GPL-2.0-or-later
"""Carve Pioneer's single-file CDJ-850 update (C850MAIN.UPD).
Container: flat big-endian binary flash image, no manifest, no S-records.
32-byte label at 0x60000: b'CDJ-850 MAIN' + b'1.13' (cf. main_decode.LABEL_SIZE).
"""
import hashlib
C850_SIZE = 2156300
C850_SHA256 = "12ed34539d844e4c3d9230eebccbc31f0b6ff2ac4d813455084987c53beac3b7"
C850_LABEL_OFF = 0x60000
def read_upd(path):
    data = open(path, "rb").read()
    if len(data) != C850_SIZE:
        raise ValueError("unexpected size %d" % len(data))
    if hashlib.sha256(data).hexdigest() != C850_SHA256:
        raise ValueError("SHA-256 mismatch")
    return data
def find_main_label(data):
    off = data.find(b"CDJ-850 MAIN")
    if off != C850_LABEL_OFF:
        raise ValueError("label at 0x%x, expected 0x%x" % (off, C850_LABEL_OFF))
    return off
def carve_regions(data):
    return {
        "boot": data[0x000000:0x040000],
        "erased": data[0x040000:0x060000],
        "main": data[0x060000:0x200000],
        "tail": data[0x200000:],
    }
