# The DSP module

A deck keeps up with real time only with a native module of the DSP program's
hot code. None is shipped, because it would be derived from Pioneer's code;
setup builds it from your own firmware ([step 5](setup.md#the-setup-steps)) and installs it into
`~/c14gen/`. A headless deck plays a track for 30 seconds, the DSP's hot code is
compiled to a native module from what it ran, and a second 30-second play with
that module collects the profile-guided counts for the final build. It uses 6
compile jobs (less on an idle machine). With the module, playback holds real
time from the first seconds after a load.

The module is installed with a stamp naming what it was built from: the
generator version and revision, the DSP core's interface, the firmware and the player.
`./start.sh` compares the stamp with the current code. After a `git pull` that
changed the generator or the core's interface it does not load the module and
shows a banner that it needs building again (`./setup.sh` rebuilds it); the
deck runs far below real time until then. If you skipped step 5, the banner
says how to build the module.

A generator change that only makes the module faster bumps a separate revision
in the stamp instead. The older module still loads, and at a terminal
`./start.sh` offers once per revision to build the faster one (about 30
minutes); answering no is remembered beside the stamp. Starts without a
terminal never ask.

Mods do not affect it: they patch the MAIN and display firmware, never the DSP
program the module is built from, so turning a mod on or off later needs no
rebuild. To compare without the module, start with `MODULE=none ./start.sh`
(the DSP's hot code is then compiled as it plays).

For the CDJ-2000NXS, `DSP_SUPPLEMENT=1 scripts/build/build_dsp_module.sh` also
builds a second, smaller module (`m.extra.so`, `MODEL_DSP_SUPPLEMENT_ARGS`)
that is loaded after the first under the same stamp. It is off by default: its
gain (about 1.3x on a complete MP3 and MASTER TEMPO replay) was measured on one
broad private profile, not on the short play setup uses.
`scripts/test/dsp_native_replay.py --record <dsp.c6rec>` replays a recording
with the interpreter and with the installed modules, and passes only when every
record is EXACT and the final hashes are equal.
