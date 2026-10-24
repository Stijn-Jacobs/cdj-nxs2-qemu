# SPDX-License-Identifier: GPL-2.0-or-later
"""Where the Serato SysEx key of a CDJ-3000X comes from: the person's own
CDJ-3000X firmware, read when the mod is applied.

The key is 128 bits, the same in every unit of a model, and the 3000X's own
loader reads it as 32 hex characters. It is never part of this repository or of
a published build; it goes from the person's firmware file into the update they
patch for their own deck and nowhere else, which makes that update theirs alone
to keep. Nothing here prints, logs or puts the key in an error message.

CDJ_3000X_FW names where it is read from. Today that is the folder the 3000X's
cabinet partition was unpacked into (the key is encryption/key.dat in it). The
update file itself is a LUKS container around an ISO whose cabinet image is
another one; opening those is not done yet.
"""
import os

KEY_FILE = os.path.join('encryption', 'key.dat')
KEY_HEX_DIGITS = 32

LUKS_MAGIC = b'LUKS\xba\xbe'
ISO_MAGIC = (0x8001, b'CD001')
EXT4_MAGIC = (0x438, b'\x53\xef')


def parse_key(text):
    """The 16 key bytes of a file's 32 hex characters, as the 3000X's loader reads it."""
    digits = text.strip()
    if len(digits) != KEY_HEX_DIGITS:
        raise SystemExit('the key file does not hold %d hex characters' % KEY_HEX_DIGITS)
    try:
        return bytes.fromhex(digits)
    except ValueError:
        raise SystemExit('the key file does not hold hex characters') from None


def container(path):
    """What an unreadable-for-us CDJ-3000X file is, for the message that says
    so; None for anything else."""
    with open(path, 'rb') as f:
        head = f.read(8)
        if head.startswith(LUKS_MAGIC):
            return 'LUKS container'
        for name, (at, magic) in (('ISO image', ISO_MAGIC), ('ext4 image', EXT4_MAGIC)):
            f.seek(at)
            if f.read(len(magic)) == magic:
                return name
    return None


def load(env):
    """The 16 key bytes from the 3000X firmware that CDJ_3000X_FW names."""
    source = env.get('CDJ_3000X_FW', '')
    if not source:
        raise SystemExit('CDJ_3000X_FW must name your own CDJ-3000X firmware: the Serato key is read from it, '
                         'and this mod does not build without it')
    if os.path.isdir(source):
        path = os.path.join(source, KEY_FILE)
        if not os.path.isfile(path):
            raise SystemExit('%s holds no %s' % (source, KEY_FILE))
        with open(path, encoding='ascii', errors='replace') as f:
            return parse_key(f.read())
    if not os.path.isfile(source):
        raise SystemExit('CDJ_3000X_FW names %s, which does not exist' % source)
    kind = container(source)
    raise SystemExit('%s is %s: reading the key out of it is not done yet, point CDJ_3000X_FW at the '
                     'unpacked cabinet folder' % (source, 'a ' + kind if kind else 'not a folder'))
