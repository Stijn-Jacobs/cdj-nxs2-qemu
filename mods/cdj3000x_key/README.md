# cdj3000x_key

**Experimental.** For the CDJ-3000X identity (`cdj3000x`): shows the key of the
loaded track. Serato sends a CDJ-3000X the key as its own message type, which the
NXS2 ignores. The deck does take a key as the musical key message of its report
framework, so this mod rewrites Serato's message into that one and hands it over,
from a hook on the entry of the report router, after the handshake's.

| | |
|---|---|
| knobs | `CDJ_MAIN_X3000KEY` |
| on / off | `1` / `0` |
| default | off |

Serato writes the key as Camelot text ("1A", "11A") or an empty string. The
firmware's parser turns a name into a key number: it takes the Camelot numbers 1
to 9 and the note names, but not 10 to 12, so those six become the note names the
same keys have on the Camelot wheel (10A Bm, 10B D, 11A F#m, 11B A, 12A C#m, 12B
E). Text that is not ASCII leaves the key empty. The colour index of the message
is left 0, Serato sends none.

**What is verified.** The routine runs on the interpreter against the keys in two
recorded Serato sessions, and the firmware's own report framework and parser run
on its output: every key Serato sent gives the key number of that Camelot code.
The update validator passes with it applied. It has not been tried on a deck:
whether the key field of the track info shows the key is not yet measured.

Back to the [mods](../../README.md#mods).
