# SPDX-License-Identifier: GPL-2.0-or-later
"""usb_midi on the CDJ-2000NXS2's MAIN 1.87: the sending half in the USB-MIDI
task, the timing half on the 1 ms timer."""
from players.cdj2000nxs2 import DEVICE, PLAYER_RECORDS, set_flag
from sigpatch import Anchor, Mod

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

USB_MIDI_FILL = (0x084F511E, Anchor(sig=[0x2521, 0xE200, 0x2420, 0xD16F, 0xDC70, 0x410B], index=3, literal=True))
USB_MIDI_COMMIT = (0x084F5148, Anchor(sig=[0x2008, 0x8D04, 0x6403, 0xD216, 0x420B, 0x4408], index=3, literal=True))
MIDI_BUFIDX = (0x0BD4B0F0, Anchor(sig=[0x000B, 0x0005, 0xD15A, 0xD95B, 0x410B, 0xE401], index=3, literal=True))
MIDI_LENS = (0x0BD4B0F4, Anchor(sig=[0x096A, 0x66F2, 0xE401, 0xD255, 0x4008, 0xD555], index=3, literal=True))
MIDI_TASKSTATE = (0x0BD50E8C, Anchor(sig=[0xDD22, 0x056A, 0xE100, 0xDE22, 0xE614, 0xD422], index=3, literal=True))
MIDI_CLASSFLAGS = (0x0BD4B0C8, Anchor(sig=[0x4008, 0x470B, 0x02E6, 0xD71E, 0x0E6A, 0xD621], index=3, literal=True))
PLAYER1_RECORD = (0x0A371E60, PLAYER_RECORDS[1]._replace(delta=0x124))
SET_FLAG = (0x08516448, set_flag)

MODS = {
    'usbmidi': Mod(
        what='USB-MIDI start/stop/clock, sending',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(USB_MIDI_FILL, USB_MIDI_COMMIT, MIDI_BUFIDX, MIDI_LENS),
        hook=True,
        **MIDI_TASK,
    ),
    'usbmiditick': Mod(
        what='USB-MIDI start/stop/clock, 1 ms timing',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(DEVICE, PLAYER1_RECORD, MIDI_LENS, MIDI_TASKSTATE, MIDI_CLASSFLAGS, SET_FLAG),
        hook=True,
        **TIMER_TICK,
    ),
}
