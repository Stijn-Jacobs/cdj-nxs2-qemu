#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a full NOR flash image: the MAIN section and the settings sector.

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

The MAIN section of the official update (a 32-byte label, then S-records) is
laid in the way the firmware's own updater does it: every record at its
address, which is a flash offset, in an image that is 0xFF elsewhere. That puts
Pioneer's bootloader at 0, an emergency updater image at 0x10000 and the MAIN
image at 0x50000, and the board then boots through the bootloader. Without the
section the image holds settings only and the board needs -kernel.

usage:  make_flash.py <out.bin> [settings.bin] [main-section.bin]
"""
import sys, os

FLASH_SIZE = 0x800000        # 8 MB, per the firmware's geometry tables
SETTINGS_OFF = 0x7F6000      # 8 KB parameter sector
SETTINGS_SIZE = 0x2000
MAIN_END = 0x7E0000          # the updater never programs the section past this
LABEL_SIZE = 32
ERASED = 0xFF


def srecords(section):
    """(address, data) of every S1/S2/S3 record, checksums verified."""
    for line in section[LABEL_SIZE:].split(b'\r\n'):
        if line[:1] != b'S' or line[1:2] not in (b'1', b'2', b'3'):
            continue
        rec = bytes.fromhex(line[2:].decode('ascii'))
        if sum(rec) & 0xFF != 0xFF:
            raise ValueError(f"bad S-record checksum: {line[:24]!r}")
        abytes = int(line[1:2]) + 1
        yield int.from_bytes(rec[1:1 + abytes], 'big'), rec[1 + abytes:-1]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    out = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else 'extract/settings.bin'
    main_sec = sys.argv[3] if len(sys.argv) > 3 else None

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
    if main_sec:
        section = open(main_sec, 'rb').read()
        if b'MAIN' not in section[:LABEL_SIZE]:
            print(f"{main_sec}: not a MAIN section ({section[:LABEL_SIZE]!r})",
                  file=sys.stderr)
            return 1
        top = 0
        try:
            for addr, data in srecords(section):
                if addr + len(data) > MAIN_END:
                    print(f"{main_sec}: record at 0x{addr:06X} runs past 0x{MAIN_END:06X}",
                          file=sys.stderr)
                    return 1
                img[addr:addr + len(data)] = data
                top = max(top, addr + len(data))
        except ValueError as e:
            print(f"{main_sec}: {e}", file=sys.stderr)
            return 1
    img[SETTINGS_OFF:SETTINGS_OFF + SETTINGS_SIZE] = sector

    with open(out, 'wb') as f:
        f.write(img)

    print(f"wrote {out}: {FLASH_SIZE:,} bytes "
          f"({FLASH_SIZE // 0x100000} MB), erased 0xFF")
    if main_sec:
        print(f"  MAIN section from {main_sec} at 0x000000-0x{top - 1:06X}")
    print(f"  settings sector from {src} at 0x{SETTINGS_OFF:06X}"
          f"-0x{SETTINGS_OFF + SETTINGS_SIZE - 1:06X}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
