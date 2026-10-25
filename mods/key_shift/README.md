# key_shift

Key shift, as on the CDJ-3000: the key of the playing track moves in
semitones while the tempo stays where the tempo fader puts it. Hold MASTER
TEMPO and press hot cue pad A to move the key a semitone down, pad B a
semitone up, pad C to reset it; the shift goes to plus or minus 12 and
resets on every load. While a shift is set, the key in the title bar shows
the shifted key and the shift: an Em track shifted up two reads F#m+2.

Stock firmware 1.87 has no key shift: with MASTER TEMPO on the key holds, and
without it the key follows the tempo.

| | |
|---|---|
| knobs | `CDJ_MAIN_KEYSHIFT`, `CDJ_MAIN_KEYSHIFTPAD`, `CDJ_MAIN_KEYSHIFTLOAD`, `CDJ_MAIN_KEYSHIFTDSP`, `CDJ_MAIN_KEYSHIFTKEY` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf`; `./setup.sh --reconfigure` asks again. For one run, set the five
knobs to `1` in the environment. In the virtual deck app (`--app`), hold the
MASTER TEMPO key and click the hot cue pads; on a controller, use the keys
mapped to MASTER TEMPO and pads A to C.

**What it changes.** Four MAIN hooks and a DSP program change. `keyshift`
sits where MAIN builds the block it sends the DSP each frame and writes the
shift into the block's second word, in semitones. `keyshiftpad` sits at the
pad mode switch and turns pads A to C into the key controls while MASTER TEMPO
is held; the pads are not passed on as hot cue presses then. `keyshiftload`
clears the shift when a track loads. `keyshiftkey`, on the pad pass after
`keyshiftpad`, rewrites the key field of the display model the title bar is
drawn from: it keeps the track's own key, spells the shifted one (classic names
such as C#m, Ebm and F# from one table, or Camelot names such as 8A when the
export uses them) with the shift after it, and asks the display task to send
the field; with no shift the track's own key goes back. On the DSP,
`keyshiftdsp` (in `cdj2000nxs2/dsp_keyshiftdsp.s`) runs in code the program never reaches,
within the boot table's own length: it keeps MASTER TEMPO's time-stretch engine
running whenever the shift is not zero, also with MASTER TEMPO off, and
resamples its output by 2^(n/12) so the pitch moves and the tempo does not.
`python mods/patch_update.py C2KNXS2.UPD out.UPD key_shift` writes all five
into a real update (firmware 1.87).

**Status: works in the emulator (firmware 1.87), not tried on a deck.** Measured
on a test track holding a steady 440 Hz tone over a 120 BPM click grid, two runs
per case against a run without the mod: with MASTER TEMPO on, a shift of +2
plays the tone at +2.000 semitones and -3 at -3.002 and -3.013, with the tempo
at 0.9992 to 0.9998 of the track's; with the shift set and MASTER TEMPO then
switched off, the tone stays at +2.000 semitones and the tempo at 0.9996. A
shift costs the DSP what MASTER TEMPO costs, so a host that cannot hold MASTER
TEMPO in real time cannot hold a shift either. The title bar's key follows the
pads: on a track in Em it read Em, then F#m+2 after two presses of pad B, then
Fm+1 after one of pad A. The title bar has room for five characters after the
key's symbol, so a shift of 10 or more after a three-character key (G#m) shows
the shifted key alone.
