# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj3000x on the CDJ-2000NXS2's MAIN 1.87: the USB identity of a CDJ-3000X.

The device descriptor, its string descriptors and the function that stamps the
firmware version into bcdDevice are data and code the USB function driver reads
from, so they are rewritten in place. The USB bring-up copies the descriptor
table at 0x0856F01C to RAM 0x10DDFBA0 and serves GET_DESCRIPTOR from the copy,
taking each string's length from its first byte. A string can therefore get
shorter without moving the table around it.
"""
from players.cdj2000nxs2 import (BEAT_GRID, HIDO_BUFFER, HIDO_READ_DONE, HIDO_STATE, HS_AUTH, REPORT_FRAMEWORK,
                                 REPORT_ROUTER_ENTRY, TICK)
from sigpatch import Anchor, DataPatch, Edit, FreeEnd, Mod

VENDOR_ID = b'\x73\x2b'
NXS2_PID = b'\x05\x00'

# The 3000X's own fallback when its bootloader has no product_id. The shipped
# one is one of 0x004E-0x0051 and is not in any firmware image; CDJ_3000X_PID
# (hex) overrides it for whoever has read the real one off a deck.
DEFAULT_PID = 0x004E


def string_descriptor(text, size):
    """bLength, type 3, the UTF-16 text, padded with zeros to the `size` bytes
    the string had; bLength alone says how much of it is sent."""
    body = text.encode('utf-16-le')
    return (bytes([2 + len(body), 3]) + body).ljust(size, b'\0')


def product_id(env):
    text = env.get('CDJ_3000X_PID', '')
    if not text:
        return DEFAULT_PID
    try:
        pid = int(text, 16)
    except ValueError:
        pid = -1
    if not 0 <= pid <= 0xFFFF:
        raise SystemExit('CDJ_3000X_PID must be a 16-bit hex value, not %r' % text)
    return pid


def identity(env):
    pid = product_id(env).to_bytes(2, 'little')
    return (
        Edit(VENDOR_ID + NXS2_PID, VENDOR_ID + pid),
        Edit(string_descriptor('Pioneer DJ Corporation', 46), string_descriptor('AlphaTheta Corporation', 46)),
        Edit(string_descriptor('CDJ-2000NXS2', 26), string_descriptor('CDJ-3000X', 26)),
        Edit(string_descriptor('CDJ-2000NXS2 MIDI', 36), string_descriptor('CDJ-3000X MIDI', 36)),
        Edit(string_descriptor('CDJ-2000NXS2 HID', 34), string_descriptor('CDJ-3000X HID', 34)),
    )


# The device qualifier and the start of the full-speed configuration header
# that follows the device descriptor of the Windows/Serato configuration; the
# device descriptor sits 18 bytes in front. The second device descriptor, 0xE88
# further on, belongs to the vendor-class configuration and keeps its identity.
# This anchor stops before the configuration's wTotalLength byte, which the
# MIDI-OUT repack below rewrites, so the two patches do not depend on order.
DEVICE_TABLE = bytes.fromhex('0a0600020000004001000902')

# The routine that writes bcdDevice (descriptor bytes 12 and 13) from the
# firmware version string: low byte from the digits after the point, high byte
# from the digit before it. Replaced by the constant 0x0140, which is what the
# 3000X's "1.40" gives. Its first instruction is the load of the descriptor
# address, which stays.
BCD_ROUTINE_TAIL = bytes.fromhex('178420c8290001ca0b000900')
BCD_STOCK = bytes.fromhex('4284 06d5 0fc9 0840 0840 0366 4384 0fc9 6b20 5c80 4060 0fc9 0b00 5d80'.replace(' ', ''))
BCD_3000X = bytes.fromhex('06d5 40e0 5c80 01e0 0b00 5d80'.replace(' ', '')) + bytes.fromhex('0900') * 8

# The MIDI-OUT repack. Serato's authentication SysEx arrives on a bulk-OUT MIDI
# endpoint, which the stock USB-MIDI function (one bulk IN endpoint, no OUT)
# does not have. The two configuration descriptors -- full-speed at 0x0856F038,
# high-speed at 0x0856F132, 250 bytes each -- gain an embedded MIDI IN jack, an
# external MIDI OUT jack sourced from it, a bulk-OUT endpoint 0x04 and its
# class-specific endpoint descriptor inside MIDIStreaming interface 3, and that
# interface's bNumEndpoints goes 1 -> 2. The 24-bit audio-streaming alternate
# (interface 1 alternate 2, 49 bytes) is dropped to make room: Serato takes PCM
# over the 16-bit alternate and never selects the 24-bit one. Net 29 bytes in,
# 49 out, so each configuration shrinks from 250 to 230 bytes; the freed 20
# bytes are left as zero padding at the tail of each descriptor's slot, so every
# byte after it -- including the string descriptors the serve-from-RAM copy
# points at with baked addresses -- keeps its offset and no pointer moves. The
# MIDIStreaming header wTotalLength follows this firmware's own convention of
# spanning the endpoints too (its stock value 0x24 counts the bulk IN endpoint),
# so it gains the full 29, not only the two jacks.
CONFIG_FS_OLD = bytes.fromhex(
    '0902fa00050100c00009040000000101000209240100011e0001010c24020101'
    '0100020300000009240302010300010009040100000102000009040101010102'
    '000007240101000100112402010202100344ac0080bb00007701090501018401'
    '0100000725010100000009040102010102000007240101000100112402010203'
    '180344ac0080bb00007701090501014602010000072501010000000904020000'
    '0101000309240100010900010309040300010103000007240100012400092403'
    '0101010201000624020202000905850240000000000525010101090404000203'
    '0000040921100100012234000705060340000107058703400001')
CONFIG_FS_NEW = bytes.fromhex(
    '0902e600050100c00009040000000101000209240100011e0001010c24020101'
    '0100020300000009240302010300010009040100000102000009040101010102'
    '000007240101001e00112402010202100344ac0080bb00007701090501018401'
    '0100000725010100000009040200000101000309240100010900010309040300'
    '0201030000072401000141000924030101010201000624020202000905850240'
    '0000000005250101010624020103000924030204010301000905040240000000'
    '0005250101030904040002030000040921100100012234000705060340000107'
    '0587034000010000000000000000000000000000000000000000')
CONFIG_HS_OLD = bytes.fromhex(
    '0902fa00050100c00009040000000101000209240100011e0001010c24020101'
    '0100020300000009240302010300010009040100000102000009040101010102'
    '000007240101000100112402010202100344ac0080bb00007701090501018401'
    '0400000725010100000009040102010102000007240101000100112402010203'
    '180344ac0080bb00007701090501014602040000072501010000000904020000'
    '0101000309240100010900010309040300010103000007240100012400092403'
    '0101010201000624020202000905850200020000000525010101090404000203'
    '0000040921110100012234000705060340000207058703400002')
CONFIG_HS_NEW = bytes.fromhex(
    '0902e600050100c00009040000000101000209240100011e0001010c24020101'
    '0100020300000009240302010300010009040100000102000009040101010102'
    '000007240101001e00112402010202100344ac0080bb00007701090501018401'
    '0400000725010100000009040200000101000309240100010900010309040300'
    '0201030000072401000141000924030101010201000624020202000905850200'
    '0200000005250101010624020103000924030204010301000905040200020000'
    '0005250101030904040002030000040921110100012234000705060340000207'
    '0587034000020000000000000000000000000000000000000000')

# SET_CONFIGURATION programs pipes from the endpoint's ordinal, not its
# address: the walker at 0x08425108 stores, per endpoint number, the count of
# endpoint descriptors in every other interface and alternate before it (the
# slot table at 0x0AC9853C), and 0x08424CE2 programs the pipe from that row of
# the pipe table at 0x0856F49C (RAM 0x10DE0020): pipe number and direction,
# PIPECFG, PIPEBUF, PIPEMAXP. Dropping the 24-bit alternate moves 0x85 from row
# 2 to row 1 and puts 0x04 on row 2, so the rows follow: the 24-bit row goes,
# MIDI IN keeps pipe 5, and MIDI OUT gets bulk pipe 4 with a 512-byte buffer
# after pipe 5's. Rows 3 and 4 (HID) keep their ordinals and are untouched.
PIPE_TABLE_OLD = bytes.fromhex(
    '0120 01c0 0818 c001 0000' '0120 01c0 0824 8002 0000'
    '0510 1540 501c 0002 0000'.replace(' ', ''))
PIPE_TABLE_NEW = bytes.fromhex(
    '0120 01c0 0818 c001 0000' '0510 1540 501c 0002 0000'
    '0420 0440 581c 0002 0000'.replace(' ', ''))

# What the stage-5 arm of the handshake (0x084FE9BC) uses once the host's
# answer is verified: the report it sends, the lock and the two state words.
HS_STATE = (0x0BD4B148, Anchor(sig=[0x4C6A, 0x1F6E, 0xE206, 0xDB17, 0x58FE, 0x2888], index=3, literal=True))
MEMSET = (0x08533D50, Anchor(sig=[0x60A3, 0x8801, 0x895F, 0xD826, 0xE640, 0xD426], index=3, literal=True))
REPORT_SEND = (0x084F4ECA, Anchor(sig=[0xD425, 0x0B6A, 0xE0F0, 0xD224, 0xE540, 0xD824], index=3, literal=True))
MAY_WAIT = (0x08518068, Anchor(sig=[0xE0F0, 0xD224, 0xE540, 0xD824, 0x2BE9, 0x8041], index=3, literal=True))
LOCK = (0x0851A580, Anchor(sig=[0x426A, 0xD21A, 0xE5FF, 0xDB1A, 0x4B0B, 0x6422], index=3, literal=True))
UNLOCK = (0x0851A7C4, Anchor(sig=[0x8F04, 0x2B22, 0xD612, 0xD215, 0x420B, 0x6462], index=3, literal=True))
LOCK_ID = (0x10DE1BE4, Anchor(sig=[0x22E9, 0x8F09, 0x426A, 0xD21A, 0xE5FF, 0xDB1A], index=3, literal=True))
# The RAM copy of the descriptor table the deck serves from, device descriptor
# first, as SET_CONFIGURATION loads it.
DEVICE = (0x10DDFBA0, Anchor(sig=[0xE6A5, 0xE120, 0x4618, 0xD2AA, 0x76D9, 0xE05C], index=3, literal=True))

# The HID report descriptor: a 64-byte Input and a 64-byte Output report of
# vendor usages. Serato sends a 3000X its detail waveform only when the Output
# report is 1024 bytes. The descriptor cannot grow (a pointer follows it), so
# its length stays 52 and every wDescriptorLength stays 0x34: the Output
# block's Physical Minimum 0 is written as the zero-length item 0x34, which
# makes room for a two-byte Report Count of 1024.
REPORT_DESCRIPTOR_INPUT = bytes.fromhex(
    '06a0ff 0901 a101 0902 a100 06a1ff 0903 0904 1580 257f 3500 45ff 7508 9540 8102'.replace(' ', ''))
OUTPUT_64 = bytes.fromhex('0905 0906 1580 257f 3500 45ff 7508 9540 9102 c0 c0'.replace(' ', ''))
OUTPUT_1024 = bytes.fromhex('0905 0906 1580 257f 34 45ff 7508 960004 9102 c0 c0'.replace(' ', ''))

# The HID OUT task's read call 0x0850074C(handle, buffer, length): the loads
# in front of it (r13 = the length word, r14 = the read, r5 = the buffer, r4 =
# the handle), and the count it returns, compared with -5 before the bt/s. The
# store after the loads stays at the site: the pool in front of the task's
# code holds data that decodes as a load from that halfword.
HIDO_READ = dict(
    sig=[0x460B, 0xE540, 0x076A, 0x27E9, 0x476A, 0xDD4C, 0xE014, 0xDE4C, 0xEC00, 0xD542, 0x54FA, 0x0FC4],
    start=5,
    end=11,
)
# The 1024-byte receive buffer, in the erased run, clear of the hand-over words,
# and below it the list x3000rxview rewrites in the NXS2 form for the framework.
RX_BUFFER = (0x081FFBE0, FreeEnd(-0x420))
LIST_BUFFER = (0x081FEFE0, FreeEnd(-0x1020))
# The detail points x3000rxview has taken since it last asked for a publish.
PENDING_POINTS = (0x081FEF98, FreeEnd(-0x1068))
# The highest point a window has reached, and what it was at the last publish.
HIGHEST_POINT = (0x081FEF94, FreeEnd(-0x106C))
PUBLISHED_POINT = (0x081FEF90, FreeEnd(-0x1070))
# The first point written since the last detail train, the train x3000rxview
# owes (0 none, 1 owed, 2 once the publisher has taken the framework's request)
# and the header's length word while that request reads it as 0.
LOW_POINT = (0x081FEF8C, FreeEnd(-0x1074))
TRAIN_OWED = (0x081FEF88, FreeEnd(-0x1078))
SAVED_LENGTH = (0x081FDF98, FreeEnd(-0x2068))
# The fragments of a list, put together before x3000rxview rewrites it, and the
# fragment it expects next (0 once a list is complete).
LIST_STAGE = (0x081FDFA0, FreeEnd(-0x2060))
NEXT_FRAGMENT = (0x081FDF9C, FreeEnd(-0x2064))
# The local cursor (x3000browse with x3000rxview). x3000browse sets the first
# word to 1 once it runs; until then x3000rxview takes lists as before. The
# guard byte; the steps answered (written by x3000rxview) and counted
# (x3000browse) and the ring of the last 32, 1 down and -1 up; 1 while the
# answer is a library list; where x3000rxview's show is; the answer's bytes,
# cursor, last row and the rows above its row 1 (negative while not known);
# the bytes of the list last handed to the framework; the bytes of a list
# owed while the guard was taken. Then x3000browse's own: the last encoder
# count, the time of the last step, of the last list it showed and the steps
# that list showed.
BROWSE_ON = (0x081FB540, FreeEnd(-0x4AC0))
GUARD = (0x081FB544, FreeEnd(-0x4ABC))
ANSWERED = (0x081FB548, FreeEnd(-0x4AB8))
STEPS = (0x081FB54C, FreeEnd(-0x4AB4))
STEP_RING = (0x081FB520, FreeEnd(-0x4AE0))
LIVE = (0x081FB550, FreeEnd(-0x4AB0))
SHOW = (0x081FB554, FreeEnd(-0x4AAC))
ANSWER_LENGTH = (0x081FB558, FreeEnd(-0x4AA8))
CURSOR = (0x081FB55C, FreeEnd(-0x4AA4))
LAST_ROW = (0x081FB560, FreeEnd(-0x4AA0))
TOP_ROWS = (0x081FB564, FreeEnd(-0x4A9C))
SENT_LENGTH = (0x081FB568, FreeEnd(-0x4A98))
LIST_OWED = (0x081FB56C, FreeEnd(-0x4A94))
ENCODER = (0x081FB570, FreeEnd(-0x4A90))
STEP_TIME = (0x081FB574, FreeEnd(-0x4A8C))
SHOWN_TIME = (0x081FB578, FreeEnd(-0x4A88))
SHOWN_STEPS = (0x081FB57C, FreeEnd(-0x4A84))
# Serato's last answer as it came, the prediction rewritten from it, and the
# list last handed to the framework.
ANSWER = (0x081FCF80, FreeEnd(-0x3080))
PREDICTION = (0x081FBF00, FreeEnd(-0x4100))
SENT_LIST = (0x081FB580, FreeEnd(-0x4A80))

# The HID IN task's send of the next report from the ring the report sender
# fills: the index scaled, the channel, the slot length, then the class send
# 0x08500B7A loads the slot pointer in its delay slot. The report sender and
# everything before the ring are boot-critical; this task is not.
HID_IN_SEND = dict(
    sig=[0xD22C, 0xD721, 0x4008, 0x54F8, 0x2ED9, 0x4E6A, 0x062E, 0x4B0B, 0x057E],
    start=2,
    end=7,
)
# The stock records the overview, grid and detail go to. The overview's
# message (flag byte first), as its copy arm loads it; the grid record
# (count at +16), as the grid completion loads it; the detail header (N at +0,
# the point array at +16), as the detail completion loads it.
OVERVIEW = (0x0BE53428, Anchor(sig=[0xE511, 0x51F3, 0x4518, 0x7590, 0x5662, 0x6263, 0x321C, 0x3256, 0x890A,
                                     0x65F2, 0xD412], index=10, literal=True))
DETAIL_HEADER = (0x0BE54510, Anchor(sig=[0xD480, 0xE60C, 0x6242, 0x3266, 0x8B01, 0xA14D], index=0, literal=True))
# The SPI tick's detail send (0x08390148): its send state (armed byte at +0,
# source +4, length +8, progress +12, remaining +16, chunks +20, index +24),
# the train byte (1 detail, 2 grid, 0 idle) with the grid's own byte after it,
# the grid's send state, the record the publisher fills for the armer, and
# the armer, as the publisher's arm path loads them.
SEND_STATE = (0x09FF85EC, Anchor(sig=[0xD493, 0xD293, 0x6640, 0x2668, 0x8B70, 0x420B], index=0, literal=True))
ARM_PATH = [0xD18E, 0xE401, 0xD28E, 0xE500, 0xD68E, 0x2140, 0xD18E, 0x2250, 0x2650, 0x5713, 0x2778, 0x8B04,
            0xD38C, 0x430B, 0x0009, 0xA003, 0x0009, 0xD38A]
TRAIN_STATE = (0x10DC1544, Anchor(sig=ARM_PATH, index=0, literal=True))
GRID_SEND = (0x09FF8608, Anchor(sig=ARM_PATH, index=4, literal=True))
TRAIN_RECORD = (0x0B55059C, Anchor(sig=ARM_PATH, index=6, literal=True))
DETAIL_ARMER = (0x08391D7C, Anchor(sig=ARM_PATH, index=17, literal=True))
# The grid's armer, as the tick's grid send after the detail loads it.
GRID_ARMER = (0x08391F2E, Anchor(sig=[0xD4A5, 0xD5A6, 0x6040, 0x8802, 0x8B57, 0x6250, 0x2228, 0x8B08, 0xD5A3,
                                      0xE101, 0xD3A3, 0x430B, 0x2510], index=10, literal=True))
# The publisher's request flag, a field of the block the setter stores into.
REQUEST_FLAG = (0x0A35F404, Anchor(sig=[0x8801, 0x0229, 0xD663, 0xE015, 0x4018, 0x70D4, 0x000B, 0x0626],
                                   index=2, literal=True, delta=0x14D4))

MODS = {
    'x3000ident': DataPatch(
        what='USB identity of a CDJ-3000X',
        target='main',
        fw_versions=('1.87',),
        table=DEVICE_TABLE,
        before=18,
        reach=0x2F0,
        edits=identity,
    ),
    'x3000bcd': DataPatch(
        what='USB bcdDevice of a CDJ-3000X',
        target='main',
        fw_versions=('1.87',),
        table=BCD_ROUTINE_TAIL,
        before=0,
        reach=len(BCD_ROUTINE_TAIL) + len(BCD_STOCK),
        edits=(Edit(BCD_STOCK, BCD_3000X),),
    ),
    'x3000midi': DataPatch(
        what='USB bulk-OUT MIDI endpoint for Serato authentication',
        target='main',
        fw_versions=('1.87',),
        table=DEVICE_TABLE,
        before=0,
        reach=0x4B0,
        edits=(Edit(CONFIG_FS_OLD, CONFIG_FS_NEW), Edit(CONFIG_HS_OLD, CONFIG_HS_NEW),
               Edit(PIPE_TABLE_OLD, PIPE_TABLE_NEW)),
        kind='data',
    ),
    'x3000hs': Mod(
        what='CDJ-3000X answer to Serato\'s HID handshake',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(HS_STATE, HS_AUTH, MEMSET, REPORT_SEND, MAY_WAIT, LOCK, UNLOCK, LOCK_ID, DEVICE),
        hook=True,
        **REPORT_ROUTER_ENTRY,
    ),
    'x3000in': Mod(
        what='deck state report in the CDJ-3000X layout',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(),
        hook=True,
        **HID_IN_SEND,
    ),
    'x3000out': DataPatch(
        what='USB HID Output report of 1024 bytes, as a CDJ-3000X declares it',
        target='main',
        fw_versions=('1.87',),
        table=REPORT_DESCRIPTOR_INPUT,
        before=0,
        reach=len(REPORT_DESCRIPTOR_INPUT) + len(OUTPUT_64),
        edits=(Edit(OUTPUT_64, OUTPUT_1024),),
    ),
    'x3000rxbuf': Mod(
        what='HID OUT read of whole 1024-byte reports',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(RX_BUFFER,),
        hook=True,
        **HIDO_READ,
    ),
    'x3000rxview': Mod(
        what='first 64 bytes of each HID OUT report for the stock handlers',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(RX_BUFFER, HIDO_BUFFER, LIST_BUFFER, HIDO_STATE, HS_AUTH, REPORT_FRAMEWORK,
                 OVERVIEW, BEAT_GRID, DETAIL_HEADER, MEMSET, PENDING_POINTS, HIGHEST_POINT,
                 PUBLISHED_POINT, LIST_STAGE, NEXT_FRAGMENT, LOW_POINT, TRAIN_OWED, SAVED_LENGTH, SEND_STATE,
                 TRAIN_STATE, GRID_SEND, TRAIN_RECORD, DETAIL_ARMER, GRID_ARMER, REQUEST_FLAG, BROWSE_ON,
                 GUARD, ANSWERED, STEPS, STEP_RING, LIVE, SHOW, ANSWER_LENGTH, CURSOR, LAST_ROW, TOP_ROWS,
                 SENT_LENGTH, LIST_OWED, ANSWER, PREDICTION, SENT_LIST),
        hook=True,
        **HIDO_READ_DONE,
    ),
    # Switched by its own row, cdj3000x_browse; it shares x3000rxview's words,
    # so it is declared here. Runs after x3000in, which moves the encoder to
    # where it reads it.
    'x3000browse': Mod(
        what='browse list moved on the deck ahead of Serato\'s answer',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(BROWSE_ON, GUARD, LIST_OWED, HIDO_STATE, HS_AUTH, TICK, ENCODER, LIVE, STEPS, ANSWERED,
                 STEP_RING, STEP_TIME, SHOWN_STEPS, SHOWN_TIME, SHOW),
        hook=True,
        after=('x3000in',),
        **HID_IN_SEND,
    ),
}
