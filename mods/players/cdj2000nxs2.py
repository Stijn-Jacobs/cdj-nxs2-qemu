# SPDX-License-Identifier: GPL-2.0-or-later
"""What the CDJ-2000NXS2's mods share: the shape of its two images, the
firmware sites more than one mod hooks, the firmware addresses more than one
mod imports, and the places the launcher pokes while it runs the player.
Everything only one mod needs is in that mod's own folder
(mods/<mod>/cdj2000nxs2/patch.py).

An import is (stock value, Anchor): the address the routine holds, the one its
firmware has (MAIN 1.87, display 1.81), and the Anchor that finds the same
thing in whatever image is being patched. An anchor is a stretch of the
firmware's own instructions around a place that loads the address
(literal=True: the value of that mov.l @(disp,pc) literal) or that is the
address (literal=False), kept unique in the image and clear of every span a mod
replaces. Data that the firmware only reaches through a nearby base is the
base's anchor plus a delta.

The words one mod's routines hand each other sit in the last bytes of the
erased run (FreeEnd), out of reach of the placed code. Each mod declares its
own; no two mods may take the same bytes.

SoC registers (0xFFD90018 TCNT4, 0xA4D90040 USB1 INTSTS0, 0xFFE7FFFF fpscr
mask) are the chip's, not the firmware's, and stay in the routines.
"""
from dsppatch import DspTable
from sigpatch import Anchor, ImageProfile

# MAIN runs in place from 0x08000000. The routines go into the largest run of
# erased flash in the image (on the v1.87 image the 938,112 bytes at
# 0x0811AF80-0x08200000, which no pc-relative load in the image reads from),
# found by sigpatch.place_inplace() rather than named here.
MAIN_PROFILE = ImageProfile(
    flash_base=0x08000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='<')

# The C6655's program, the boot table MAIN streams to the DSP's first stage at
# boot (0x8DD54 bytes at 0x080034E0 on v1.87, entry 0x00800200 and the first
# section's header). MAIN sends it as 35 windows of 16 KB and a last one of
# 0x1D54 bytes, counts its own code holds as immediates, so the table cannot
# grow. The cave is six PCM converters, 0x80065320-0x80065854, that no branch,
# call, constant pair or data word anywhere in the program reaches.
DSP_PROFILE = DspTable(
    head=(0x00800200, 0x0000BFC0, 0x00802000, 0x07BE09C2, 0x0201416E, 0x04001E2A, 0x04105FEA, 0x04A0805A),
    length=0x8DD54,
    cave=(0x80065320, 0x80065854, '44fcd723a4018ecf920f45e2b17ba6e454f961da952e93b3880d2501729fc869'))

GUI_PROFILE = ImageProfile(
    flash_base=0x30000, loadtab_off=0x10, loadtab_n=20,
    ram_base=0x1C000000, ram_end=0x1C280000)

# -- MAIN sites hooked by more than one mod -----------------------------------

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

# The colour-preview publisher's copy of the 600 six-byte overview records
# into the link slot: the copy's call with its delay slot, then the fpscr read
# and the load after it, which make the span long enough for the jump to the
# trampoline. The hooks run after the copy, with the slot's lock still held.
OVERVIEW_PUBLISH = dict(
    sig=[0x65B3, 0xEB00, 0x2E12, 0xD14E, 0x410B, 0x1E21, 0x026A, 0xD547, 0xD644],
    start=3,
    end=8,
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

# The pad pass's mode switch 0x08442D96, at its entry: the pushes of r8-r14
# and pr. The pad pass 0x08443050 calls it on every Player pass, after the
# queue of pad edges is built. The span has to take the pr push with it: the
# trampoline returns through pr, so a push left after the span saves the
# trampoline's way back instead of the caller's, and the switch's rts then
# loops on itself.
PAD_MODES = dict(
    sig=[0x2F86, 0x2F96, 0x2FA6, 0x2FB6, 0x2FC6, 0x2FD6, 0x2FE6, 0x4F22, 0x4F12, 0xD60C,
         0x6062, 0x8802, 0x892B, 0x8801, 0x895E],
    start=0,
    end=8,
)

# -- MAIN addresses imported by more than one mod -----------------------------

# code
UDP_WRAP = (0x085113EC, Anchor(sig=[0xE01F, 0x0464, 0xE020, 0xD66B, 0x04C4, 0xEC3C], index=3, literal=True))     # UDP send wrapper
TICK_GETTER = (0x08221D9E, Anchor(sig=[0x7F08, 0xDD32, 0x076A, 0xD234, 0x65F3, 0x64E3], index=3, literal=True))  # reads the tick word
UDP_SND_DAT = (0x08233CF6, Anchor(sig=[0xE500, 0x2F56, 0x096A, 0xD150, 0x65F3, 0x56A2], index=3, literal=True))  # udp_snd_dat
MALLOC = (0x08344A9C, Anchor(sig=[0x22A9, 0x8D1A, 0x426A, 0xD557, 0xE101, 0xE404], index=3, literal=True))
FREE = (0x08344B42, Anchor(sig=[0x6073, 0x8D08, 0x81FF, 0xD52F, 0x450B, 0x0009], index=3, literal=True))
MEMSET = (0x08533D50, Anchor(sig=[0x4F26, 0x2FE6, 0x4F22, 0x7FDC, 0xD17C, 0x64F3], index=4, literal=True))
TAG_FETCH = (0x082545F8, Anchor(sig=[0x372C, 0x2F76, 0x7004, 0xD145, 0xD746, 0x64E3], index=3, literal=True))    # generic ANLZ tag fetch

# data
DEVICE = (0x0AB84CFA, Anchor(sig=[0x4D0B, 0x446A, 0x056A, 0xD453, 0x25B9, 0x456A], index=3, literal=True))       # the device record
TICK = (0x0B0C5258, Anchor(sig=[0x2FD6, 0x4F22, 0x7FFC, 0xD18A, 0xEDCE, 0x4D18], index=3, literal=True))         # 1 ms tick word
PLAYER_RECORDS = (0x0A371D3C, Anchor(sig=[0xE749, 0x016A, 0x4708, 0xD977, 0x07E7, 0x041A], index=3, literal=True))

# The pad words of the deck's own panel in the Player key bank, one per slot
# from slot 0 (source 0 of 0x0B056C90, at +0x24C), as the pad test 0x08442CDA
# loads them.
PADS = (0x0B056EDC, Anchor(sig=[0xD644, 0xE504, 0x7604, 0x6262, 0x2228, 0x8B07, 0x5261], index=0,
                           literal=True))

# data the firmware reaches only through a base a little before it
OVERVIEW_SLOT = (0x0B569528, Anchor(sig=[0x056A, 0x61E2, 0x52E1, 0xE60E, 0xDE54, 0x4618], index=4, literal=True, delta=0x8))

# -- MAIN sites and addresses of the HID report path, used by more than one mod --

# The entry of the host-report router 0x084F9504: its five register pushes and
# its frame, which the hook trampoline runs before the hooks look at the
# report. The router hands the report to the handshake handler 0x084FD262
# while the deck is not authenticated, to the report dispatchers once it is.
# The handler's own entry is in the boot-critical set; this is not.
REPORT_ROUTER_ENTRY = dict(
    sig=[0x2FB6, 0x2FC6, 0x2FD6, 0x2FE6, 0x4F22, 0x7FA4, 0xD646, 0x6C53, 0x6262, 0x2228, 0x8F08, 0x6E43],
    start=0,
    end=6,
)

# The HID OUT task's read of a report, after the count it returns is checked.
HIDO_READ_DONE = dict(
    sig=[0x4E0B, 0x66D2, 0x016A, 0x88FB, 0xDE3D, 0x6503, 0x21E9, 0x8D2F, 0x416A],
    start=2,
    end=7,
)

# The HID OUT task's 128-byte report buffer, as its start-up code loads it.
HIDO_BUFFER = (0x0BD50FDC, Anchor(sig=[0xD21F, 0xE001, 0xD722, 0xE440, 0xD11F, 0xD521, 0x2B42, 0xE404],
                                  index=4, literal=True))
# The HID OUT task's state word, 2 while it hands reports to the router.
HIDO_STATE = (0x0BD50E90, Anchor(sig=[0x2CE9, 0x4C6A, 0x7F10, 0xDB5E, 0x64B2, 0x6043, 0x8801, 0x8B0D],
                                 index=3, literal=True))
# The handshake's authentication word.
HS_AUTH = (0x0BD4B144, Anchor(sig=[0xD215, 0x420B, 0x6462, 0xD614, 0xE201, 0xA07B], index=3, literal=True))
# The report framework (r4 = report, r5 = length): per-type records, appended at
# each record's running count, completed at the last fragment.
REPORT_FRAMEWORK = (0x084F9DB2, Anchor(sig=[0x2F86, 0x2F96, 0x2FA6, 0x2FB6, 0x2FC6, 0x2FD6, 0x2FE6, 0x4F22,
                                            0x4F12, 0x7FE0, 0x8441, 0xE125], index=0, literal=False))
# The stock beat grid record (count at +16), as the grid completion loads it.
BEAT_GRID = (0x0BF5BFFC, Anchor(sig=[0xDC7C, 0xED00, 0xE014, 0x58C3, 0x28D1, 0x5AC3], index=0, literal=True))

# -- what the launcher pokes while it runs the player (launcher/sites.py) ------

LAUNCHER = dict(
    load_poke_pc=Anchor(sig=[0xAFDD, 0x426A, 0xE094, 0x600C, 0x02EE, 0x2228], index=4, literal=False),       # 0x082F04D8 the loader's wait branch
    set_flag=Anchor(sig=[0x60D3, 0xDE69, 0xE501, 0xD773, 0x2219, 0x426A], index=3, literal=True),             # 0x08516448 sets an event flag
    load_poke_addr=Anchor(sig=[0x6EF6, 0x000B, 0x6DF6, 0xDE38, 0xE1AC], index=3, literal=True, delta=0x94),   # 0x0994CC18 the loader's state word
    own_mac=Anchor(sig=[0x000B, 0x006E, 0xD26D, 0x63F2, 0x51F1], index=2, literal=True),                      # 0x0A35F754 the player's own MAC
    deck_ready=Anchor(sig=[0x410B, 0x0009, 0x056A, 0xD63E, 0x2008], index=3, literal=True),                   # 0x0B5125B8 the deck-state word
    colour_capability=Anchor(sig=[0x8B00, 0xE600, 0xEC04, 0xD53D, 0x4C18], index=3, literal=True, delta=0x6),  # 0x0A35FAAA
    sr_save_slot=Anchor(sig=[0x0002, 0x6212, 0xD115, 0x420E, 0x000B], index=2, literal=True),                 # 0x0AB87D8C where the RTOS keeps SR
    # The C library routines the machine runs as host code. The head of each is the anchor; memcmp's reaches into
    # the next routine because a second routine opens with the same 22 halfwords.
    native_memcmp=Anchor(sig=[0x2668, 0x6763, 0x8D10, 0x6153, 0xA005, 0xE500, 0x6644, 0x6214, 0x3620, 0x8B02,
                              0x7501, 0x3572, 0x8BF8, 0xE0FF, 0x054C, 0x071C, 0x605C, 0x677C, 0x000B, 0x3078,
                              0x000B, 0xE000, 0xE218, 0x3622], index=0, literal=False),                       # 0x085336B0
    native_strlen=Anchor(sig=[0xA001, 0xE500, 0x7501, 0x6643, 0x6260, 0x2228, 0x8FFA, 0x7401, 0x000B, 0x6053],
                         index=0, literal=False),                                                             # 0x085341C0
    native_memcpy=Anchor(sig=[0xE218, 0x3622, 0x8F10, 0x6743, 0xA004, 0x0009, 0x6254, 0x76FF, 0x2420, 0x7401,
                              0x6053, 0xC803], index=0, literal=False),                                       # 0x085336DC
    native_memset=Anchor(sig=[0x2FE6, 0x6143, 0x346C, 0x6243, 0xE41F, 0x3642, 0x8F1F, 0x6E53, 0x6023, 0xC90F,
                              0x3608, 0xA002], index=0, literal=False),                                       # 0x08533D50
    native_movmem=Anchor(sig=[0x2F36, 0xE340, 0x3302, 0x8B4B, 0xD301, 0x033E, 0x432B, 0x0009],
                         index=0, literal=False),                                                             # 0x08531F88 block copy
)

# usb_midi raises the same event flag.
set_flag = LAUNCHER['set_flag']
