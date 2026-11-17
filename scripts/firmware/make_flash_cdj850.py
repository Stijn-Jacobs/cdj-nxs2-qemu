#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble the CDJ-850's 4 MB NOR flash image from the prepared firmware.

The updater's own offsets (notes/cdj850-layout.md): boot.bin at flash 0, the
MAIN region at 0x60000, erased (0xFF) elsewhere. The updater never programs
past 0x200000, so the top half stays erased. The board boots through the
bootloader at flash 0 (MAIN_BOOT=flash).

usage: make_flash_cdj850.py [out.bin] [extract-dir]
  out.bin      default extract/cdj850/flash_cdj850.bin (repo-relative)
  extract-dir  default extract/cdj850 (must hold boot.bin + main_flat.bin)
"""
import os
import sys

FLASH_SIZE = 0x400000        # 4 MB: DYW1796 32 Mbit
MAIN_OFF = 0x60000           # MAIN region flash offset (= label offset)
BOOT_SIZE = 0x40000          # boot region size
MAIN_SIZE = 0x1A0000         # MAIN region size (0x060000-0x200000)
ERASED = 0xFF


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    here = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        here, "extract", "cdj850", "flash_cdj850.bin")
    ext = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        here, "extract", "cdj850")

    boot_path = os.path.join(ext, "boot.bin")
    main_path = os.path.join(ext, "main_flat.bin")
    for path, want in ((boot_path, BOOT_SIZE), (main_path, MAIN_SIZE)):
        if not os.path.exists(path):
            print(f"no {path} -- run scripts/firmware/prepare_firmware.sh "
                  f"--model cdj850 --install <C850MAIN.UPD> first",
                  file=sys.stderr)
            return 1
        got = os.path.getsize(path)
        if got != want:
            print(f"{path}: expected {want} bytes, got {got}",
                  file=sys.stderr)
            return 1

    img = bytearray([ERASED]) * FLASH_SIZE
    with open(boot_path, "rb") as f:
        img[0:BOOT_SIZE] = f.read()
    with open(main_path, "rb") as f:
        img[MAIN_OFF:MAIN_OFF + MAIN_SIZE] = f.read()
    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out} ({len(img)} bytes: boot at 0x0, main at 0x{MAIN_OFF:X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
