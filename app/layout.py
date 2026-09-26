# SPDX-License-Identifier: GPL-2.0-or-later
"""Where everything sits on the deck face, and what each control does.

Pure data. The face is laid out in UNITS of a 972 x 1252 grid, the NXS2's top
panel seen from straight above (its proportions and the arrangement of its
controls follow the operating manual's "Control panel" drawing and product
photos). Everything is drawn from scratch by art.py; no photo or artwork is
used, and the manufacturer's logos, model name and disc marks are left off.

Each control names an action from midi/cdj_actions.py, so the report bits live
in one table. `lamp` is a role from midi/leds.py's LAMPS: the NXS2 lamp that
lights it. How a control behaves follows its action's evidence status (see
controls.py): confirmed, partial and decoded ones work, the rest are drawn but
inert. The status bar names the control under the mouse and its key.
"""

from dataclasses import dataclass

W, H = 972, 1252

LCD = (246, 110, 716, 392)              # 470 x 282: the panel's 5:3
TOP_BLOCK = (137, 0, 835, 402)          # the gloss-black raised display panel
LEFT_COL = (0, 52, 137, H)
RIGHT_COL = (835, 52, W, H)
JOG = (483, 815, 327)                   # centre x, centre y, outer radius
JOG_RIM = 257                           # the rubber rim ends here
JOG_TOP = 238                           # the touch-sensitive platter top
JOG_DISPLAY = 120                       # the centre display's window
SELECT = (783, 198, 42)                 # the rotary selector
SELECT_PANEL = (738, 100, 828, 300)
SLIDER = (900, 850, 1144)               # x, top and bottom of the cap's travel
SLIDER_FRAME = (868, 809, 932, 1186)

LIME = (160, 255, 60)
BLUE = (60, 130, 255)
GREEN = (40, 235, 90)
ORANGE = (255, 120, 30)
AMBER = (255, 160, 40)
RED = (255, 30, 30)
CYAN = (70, 220, 255)
YELLOW = (240, 235, 40)
WHITE = (235, 240, 255)


@dataclass
class Key:
    """One pressable control.

    kind picks the look (art.py): src, top, pad, dome (black), cdome (chrome),
    ring (black in a chrome ring), pale (black in a pale ring), chrome, plate
    (chrome with a label plate), loop, call, big, arc, lever, rect. A key has a box (x0, y0, x1, y1) or a circle
    (cx, cy, r).
    """
    name: str
    kind: str
    action: str | None
    box: tuple = None
    circle: tuple = None
    label: str = ""                     # printed on the key
    lamp: str | None = None             # LAMPS role that lights it
    color: tuple = LIME                 # its light
    dot: tuple | None = None            # a small LED in the key's centre
    symbol: str = ""                    # arrow glyph drawn on the key
    font: int = 11
    keycap: str = ""                    # the host key that presses it too

    def bounds(self):
        if self.box:
            return self.box
        cx, cy, r = self.circle
        return (cx - r, cy - r, cx + r, cy + r)

    def hit(self, x, y):
        if self.circle:
            cx, cy, r = self.circle
            return (x - cx) ** 2 + (y - cy) ** 2 <= (r + 3) ** 2
        x0, y0, x1, y1 = self.box
        return x0 - 2 <= x <= x1 + 2 and y0 - 2 <= y <= y1 + 2


@dataclass
class Text:
    """Printed on the panel: (x, y, text, size, anchor)."""
    x: float
    y: float
    text: str
    size: int = 10
    anchor: str = "mm"
    bold: bool = False
    color: str = "print"                # print, dim, or a key colour name


def _pad(letter, y, color):
    return Key(f"hot_cue_{letter}", "pad", None, box=(37, y, 104, y + 36),
               color=color)


KEYS = [
    # Left column.
    Key("usb_stop", "cdome", None, circle=(101, 141, 13)),
    _pad("a", 316, RED), _pad("b", 390, CYAN), _pad("c", 464, YELLOW),
    _pad("d", 540, BLUE),
    # BANK and the hot cue CALL/DELETE: no report bits are known for them.
    Key("bank_rev", "ring", None, circle=(30, 634, 11), dot=BLUE),
    Key("hot_cue_call", "rect", None, box=(60, 623, 80, 645)),
    Key("bank_fwd", "ring", None, circle=(110, 634, 11), dot=BLUE),
    Key("reverse", "lever", "direction_rev", box=(28, 700, 78, 752), keycap="Z"),
    Key("track_rev", "chrome", "track_rev", circle=(39, 835, 22),
        symbol="track_rev", keycap=","),
    Key("track_fwd", "chrome", "track_fwd", circle=(100, 835, 22),
        symbol="track_fwd", keycap="."),
    Key("scan_rev", "chrome", "scan_rev", circle=(39, 916, 22),
        symbol="scan_rev", keycap="["),
    Key("scan_fwd", "chrome", "scan_fwd", circle=(100, 916, 22),
        symbol="scan_fwd", keycap="]"),
    Key("cue", "big", "cue", circle=(68, 1037, 52), label="CUE", lamp="cue",
        color=ORANGE, font=15, keycap="C"),
    Key("play", "big", "play_pause", circle=(68, 1175, 52), symbol="play",
        lamp="play", color=GREEN, keycap="Space"),

    # The display panel: sources, browse keys, time mode and quantize.
    Key("pc", "src", "dev_rekordbox", box=(168, 55, 208, 74), label="PC",
        color=WHITE, font=9, keycap="R"),
    Key("link", "src", "dev_link", box=(168, 99, 208, 120), label="LINK",
        color=BLUE, keycap="L"),
    Key("usb", "src", "dev_usb", box=(168, 144, 208, 165), label="USB",
        keycap="U"),
    Key("sd", "src", "dev_sd", box=(168, 189, 208, 210), label="SD"),
    Key("disc", "src", "dev_disc", box=(168, 234, 208, 255), label="DISC",
        keycap="D"),
    Key("time_mode", "cdome", "time_a_cue", circle=(188, 302, 12)),
    Key("quantize", "dome", None, circle=(188, 357, 12), dot=RED),
    Key("browse", "top", "browse", box=(280, 55, 368, 74), label="BROWSE",
        keycap="B"),
    Key("tag_list", "top", "taglist", box=(388, 55, 476, 74), label="TAG LIST",
        keycap="T"),
    Key("info", "top", "information", box=(497, 55, 584, 74), label="INFO",
        keycap="I"),
    Key("menu", "top", "menu_utility", box=(604, 55, 692, 74), label="MENU",
        keycap="M"),
    Key("back", "arc", "back", box=(742, 104, 782, 156), label="nw",
        keycap="Esc"),
    Key("tag_track", "arc", "tagtrack", box=(784, 104, 824, 156), label="ne"),
    Key("track_filter", "arc", None, box=(742, 240, 782, 292), label="sw"),
    Key("short_cut", "arc", None, box=(784, 240, 824, 292), label="se"),

    # Right column.
    Key("eject", "chrome", "eject", circle=(900, 163, 27), symbol="eject",
        color=GREEN),
    Key("jog_mode", "rect", "jog_mode", box=(904, 512, 948, 545),
        label="JOG\nMODE", font=9, color=BLUE, keycap="J"),
    Key("sync", "plate", "sync", circle=(870, 628, 22), label="SYNC",
        color=WHITE, font=8, keycap="S"),
    Key("master", "plate", "master", circle=(930, 628, 22), label="MASTER",
        color=ORANGE, font=7, keycap="A"),
    Key("tempo_range", "cdome", "tempo_range", circle=(900, 700, 12),
        keycap="P"),
    Key("master_tempo", "cdome", "master_tempo", circle=(900, 773, 12),
        dot=RED, lamp="master_tempo", keycap="K"),
    Key("tempo_reset", "dome", "tempo_reset", circle=(803, 1000, 15)),

    # Under the screen: the loop section, cue/loop call, delete, memory.
    Key("loop_in", "loop", "loop_in", circle=(180, 468, 25), color=ORANGE,
        keycap="Q"),
    Key("loop_out", "loop", "loop_out", circle=(256, 468, 25), color=ORANGE,
        keycap="W"),
    Key("reloop", "ring", "reloop_exit", circle=(350, 468, 20), dot=ORANGE,
        keycap="E"),
    Key("four_beat", "pale", "four_beat_loop", circle=(178, 543, 15)),
    Key("slip", "ring", "slip_mode", circle=(178, 603, 13), dot=RED,
        lamp="slip", keycap="V"),
    Key("call_rev", "call", "call_rev", circle=(628, 468, 12), symbol="left"),
    Key("call_fwd", "call", "call_fwd", circle=(686, 468, 12), symbol="right"),
    Key("delete", "ring", "delete", circle=(748, 468, 20)),
    Key("memory", "ring", "memory", circle=(803, 468, 13)),
]

TEXTS = [
    # Left column.
    Text(48, 138, "5V = 2.1A", 7), Text(104, 106, "USB", 9),
    Text(104, 117, "STOP", 9), Text(76, 262, "SD", 9),
    Text(70, 305, "HOT  CUE", 10),
    *[Text(26, y + 18, a, 9) for a, y in zip("ABCD", (316, 390, 464, 540))],
    *[Text(115, y + 18, a, 9) for a, y in zip("EFGH", (316, 390, 464, 540))],
    Text(44, 606, "•CALL /", 8),
    Text(70, 662, "BANK", 9), Text(70, 686, "DIRECTION", 9),
    Text(110, 698, "SLIP", 8), Text(110, 708, "REV", 8), Text(110, 723, "FWD", 8),
    Text(70, 801, "TRACK  SEARCH", 9), Text(70, 882, "SEARCH", 9),
    Text(70, 1112, "PLAY/PAUSE", 9),

    # The display panel.
    Text(152, 296, "TIME", 8), Text(152, 306, "MODE", 8),
    Text(222, 296, "AUTO", 8), Text(222, 306, "CUE", 8),
    Text(152, 286, "•", 8), Text(222, 286, "—", 8),
    Text(188, 379, "QUANTIZE", 8),
    Text(324, 41, "— SEARCH", 9), Text(540, 41, "— LINK INFO", 9),
    Text(648, 41, "— UTILITY", 9),
    Text(757, 86, "BACK", 8), Text(804, 76, "TAG TRACK", 8),
    Text(804, 86, "/ REMOVE", 8),
    Text(762, 304, "•TRACK FILTER", 7), Text(762, 313, "— EDIT", 7),
    Text(810, 304, "SHORT", 7), Text(810, 313, "CUT", 7),
    Text(784, 368, "MP3/AAC/WAV", 8), Text(784, 380, "AIFF/FLAC/ALAC", 8),

    # Right column.
    Text(900, 90, "STANDBY", 7), Text(900, 120, "DISC EJECT", 9),
    Text(900, 300, "VINYL", 9), Text(900, 311, "SPEED ADJUST", 9),
    Text(900, 329, "TOUCH/BRAKE", 8), Text(900, 412, "RELEASE/START", 8),
    Text(903, 578, "BEAT SYNC", 9), Text(903, 593, "— INST. DOUBLES", 8),
    Text(900, 665, "TEMPO", 9), Text(900, 677, "±6 / ±10 / ±16 / WIDE", 7),
    Text(900, 740, "MASTER", 9), Text(900, 751, "TEMPO", 9),
    Text(900, 1171, "TEMPO", 9),
    Text(803, 1022, "TEMPO", 7), Text(803, 1031, "RESET", 7),
    Text(858, 845, "–", 11), Text(858, 997, "0", 9), Text(858, 1150, "+", 11),

    # The loop section and its neighbours.
    Text(179, 435, "IN/CUE", 9), Text(256, 435, "OUT", 9),
    Text(305, 455, "LOOP", 9), Text(349, 435, "RELOOP/EXIT", 9),
    Text(178, 522, "• 4 / — 8BEAT", 8), Text(178, 583, "SLIP", 9),
    Text(657, 433, "CUE/LOOP", 9), Text(657, 452, "CALL", 9),
    Text(628, 489, "1/2X", 7), Text(686, 489, "2X", 7),
    Text(748, 433, "DELETE", 9), Text(803, 433, "MEMORY", 9),
    Text(762, 512, "JOG ADJUST", 9), Text(737, 578, "LIGHT", 6),
    Text(793, 578, "HEAVY", 6),
    Text(243, 1089, "REV", 8), Text(243, 1078, "–", 9),
    Text(727, 1089, "FWD", 8), Text(727, 1078, "+", 9),
]

# Printed boxes: the white pill labels and the coloured badges.
PILLS = [
    (179, 504, "IN ADJUST"), (256, 504, "OUT ADJUST"),
    (178, 569, "LOOP CUTTER"), (657, 493, "LOOP"),
]
BADGES = [
    (868, 519, "VINYL", BLUE), (868, 540, "CDJ", GREEN), (110, 745, "REV", RED),
]
# Decorative knobs: (cx, cy, r) -- vinyl speed adjust and jog adjust are not
# modelled on this emulator.
# The wells key groups sit in, full stadiums: (the keys, the margin round
# them). A well is its keys' outline plus the margin on every side, so it is
# centred on them by construction.
RECESSES = [(("bank_rev", "hot_cue_call", "bank_fwd"), 5),
            (("track_rev", "track_fwd"), 4),
            (("scan_rev", "scan_fwd"), 4),
            (("sync", "master"), 4)]


def recess_box(names, margin):
    """The well round a group of keys, in face units."""
    boxes = [k.bounds() for k in KEYS if k.name in names]
    return (min(b[0] for b in boxes) - margin, min(b[1] for b in boxes) - margin,
            max(b[2] for b in boxes) + margin, max(b[3] for b in boxes) + margin)

# The light the face is drawn under: one soft source above its left edge, as
# the NXS2's top panel is seen. (x, y) it falls off from, and how far it
# reaches across (x) and down (y), in face units.
LIGHT = (140, 404, 300, 190)

# Outlined boxes: printed frames around a word, as DELETE beside CALL.
FRAMES = [(88, 606, "DELETE")]
# Decorative knobs: (style, cx, cy, r) -- vinyl speed adjust and jog adjust
# are not modelled on this emulator. A vinyl knob has tick lines round it, the
# jog adjust dots, its two end marks and a silver cap.
KNOBS = [("vinyl", 901, 361, 21), ("vinyl", 901, 443, 21),
         ("jog_adjust", 762, 548, 20)]

# Printed lines, as polylines in face units, measured off the NXS2's top
# panel: the rules joining keys, the brackets, and the vinyl speed curves.
PRINTED_LINES = [
    # The loop row: IN/CUE to OUT, OUT to RELOOP/EXIT under "LOOP", and the
    # cue/loop call row on to DELETE and MEMORY.
    [(210, 468), (224, 468)], [(287, 468), (324, 468)],
    [(643, 468), (671, 468)], [(701, 468), (724, 468)], [(772, 468), (787, 468)],
    # "INST. DOUBLES" hangs off a bracket running down to SYNC.
    [(852, 601), (852, 592), (858, 592)],
    # BANK sits in a bracket with its ends turned up.
    [(24, 655), (24, 660), (52, 660)], [(88, 660), (116, 660), (116, 655)],
    # TOUCH/BRAKE: how the platter stops, sharp at one end of the knob's
    # travel and gradual at the other; RELEASE/START: how it starts.
    [(870, 389), (878, 389), (878, 401), (886, 401)],
    [(913, 389), (919, 389), (931, 401), (937, 401)],
    [(867, 483), (877, 483), (877, 472), (885, 472)],
    [(912, 483), (918, 483), (929, 472), (935, 472)],
]

# The face's own name. Generic on purpose: nothing of the manufacturer's is
# shipped, and that includes its marks.
TITLE = "NXS2 VIRTUAL DECK"

# The dock (F2): each deck's screen large beside the faces, in a bezel of the
# face's materials. In face units, drawn at the face's scale so its print
# matches the face's.
DOCK_PAD = 16                           # the panel's edge to a bezel
DOCK_BEZEL = 12                         # the gloss surround around the glass
DOCK_HEADER = 26                        # the deck's name above its bezel
