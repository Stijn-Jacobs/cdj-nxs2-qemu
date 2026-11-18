# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 JOG FL fields in the MAIN-to-panel SCIF1 status frame."""

from dataclasses import dataclass

JOG_FRAMES = 135       # MAIN 0x0c0ec350 sends (frame % 135) + 1
CUE_SEGMENTS = 85      # MAIN 0x0c0ec224 sends round(cue phase * 84 / 134) + 1


@dataclass(frozen=True)
class JogDisplay:
    position: int | None
    cue: int | None
    vinyl: bool
    touch: bool
    audio_memory: bool

    @classmethod
    def from_frame(cls, frame):
        # MAIN 0x0c065d9e copies the jog fields into bytes 1..3 of status page 1.
        # Only accept that page with its 8-bit end-around-carry checksum.
        if not frame or len(frame) != 16 or frame[0] != 1:
            return None
        total = sum(frame[:15])
        if (total - 1) % 255 + 1 != frame[15]:
            return None

        cue, flags, position = frame[1:4]
        # Zero and the display's special values (e.g. 0x88) have no phase.
        position = position - 1 if 1 <= position <= JOG_FRAMES else None
        cue = cue - 1 if position is not None and 1 <= cue <= CUE_SEGMENTS else None
        # MAIN 0x0c064d10 supplies VINYL (including the jog/brake indication).
        # MAIN 0x0c064118 drives the audio-memory blink in the status itself.
        vinyl = bool(flags & 0x08)
        return cls(position, cue, vinyl, vinyl and bool(flags & 0x20),
                   position is not None and bool(flags & 0x40))
