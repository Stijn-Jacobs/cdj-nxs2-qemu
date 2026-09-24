#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a full NOR flash image with the settings sector provisioned.

make_settings.py produces only the 8 KB parameter sector. QEMU's pflash wants a
backing file the size of the whole device, so the sector has to be placed at its
real offset inside an otherwise-erased image.

Why this matters: with a completely erased device the firmware's settings scan
finds no valid record in any sector and walks straight off the end of the flash
-- observable as a linear sweep of 16-bit reads starting at the first address
past the device. Provisioning one record gives the scan something to stop on.

Geometry is the firmware's own, from its sector tables (0x080B0978 addresses,
0x080B0B94 sizes): 127 x 64 KB + 8 x 8 KB = 8 MB, top-boot. The settings sector
is the 8 KB one at 0x7F6000.

usage:  make_flash.py <out.bin> [settings.bin]
"""
import sys, os

FLASH_SIZE = 0x800000        # 8 MB, per the firmware's geometry tables
SETTINGS_OFF = 0x7F6000      # 8 KB parameter sector
SETTINGS_SIZE = 0x2000
ERASED = 0xFF


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    out = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else 'extract/settings.bin'

    if not os.path.exists(src):
        print(f"no settings sector at {src} -- run make_settings.py first",
              file=sys.stderr)
        return 1

    sector = open(src, 'rb').read()
    if len(sector) != SETTINGS_SIZE:
        print(f"{src}: expected {SETTINGS_SIZE} bytes, got {len(sector)}",
              file=sys.stderr)
        return 1

    img = bytearray([ERASED]) * FLASH_SIZE
    img[SETTINGS_OFF:SETTINGS_OFF + SETTINGS_SIZE] = sector

    with open(out, 'wb') as f:
        f.write(img)

    print(f"wrote {out}: {FLASH_SIZE:,} bytes "
          f"({FLASH_SIZE // 0x100000} MB), erased 0xFF")
    print(f"  settings sector from {src} at 0x{SETTINGS_OFF:06X}"
          f"-0x{SETTINGS_OFF + SETTINGS_SIZE - 1:06X}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
