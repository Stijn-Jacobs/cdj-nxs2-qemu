# Reproducible NXS native DSP coverage

`CDJ_MODEL=cdj2000nxs scripts/build/build_dsp_module.sh` builds the curated
module from your own extracted firmware and DSP profile, then makes a second
pass over that same profile with `MODEL_DSP_SUPPLEMENT_ARGS` in
`models/cdj2000nxs.conf`:

```
--roots 100 --min 100000 --nodes 400 --kernels 16 --cold 2000 --idle-head 0xc004cb8c --jobs 6
```

The supplemental pass uses the generator defaults for everything else (no
wide-memory/return-prediction options from the curated pass). It installs
`m.extra.so` beside `~/c14gen/curated-cdj2000nxs/m.so`; the launcher loads the
curated module first and the supplement second using the core's existing
`C66X_JIT` module chain. Both are covered by `m.stamp`. Generated C, mapped DSP
RAM, compiled modules, recordings and replay receipts remain private and
untracked. No firmware-derived files or additional firmware hashes belong in
the repository.

The Mac arm64 coverage trial (mods repository `fadfefef`,
`analysis/data/linux-port-native-coverage-trial.json`) replayed the complete
generated MP3-320 workload, including seven tempo/MASTER TEMPO phases: 75.69 s
with the curated module, 58.20 s with the supplement (1.30x throughput, 23.1%
less time). All 48,115,851 records were EXACT with identical final state, write
and RAM hashes against both the interpreter and the curated module. This was
one timing sample using diagnostic media; Linux and real-library performance
remain unmeasured.

Coverage depends on the supplied profile. The trial used the complete recorded
MP3/MASTER TEMPO workload's profile with the existing curated module, rather
than the ordinary short-play build profile. Set `DSP_SUPPLEMENT_PROFILE` when
running `build_dsp_module.sh` to reuse such a broader private profile; otherwise
both generator passes use the build's own short-play profile. The options alone
do not guarantee the measured speedup on a different profile/workload.

To regenerate only the extra regions without a deck/QEMU guest run, use a
previously recorded private profile (including its `ram_*.bin` DSP images):

```sh
CDJ_MODEL=cdj2000nxs C66X_JIT_LIBDIR="$PWD/build/c6x" \
  scripts/build/build_dsp_supplement.sh /path/to/private/profile runs/nxs-extra
```

The output directory must be new. Six jobs is the default; `JOBS` overrides
it. The helper builds the current core library, generates C, then compiles it
with at most six compiler processes and prints elapsed seconds. The normal
module build calls this same helper after its curated PGO build. The normal
build profiles/trains decks; the helper and gate below use only offline tools.

The exact-replay gate reuses the trial's `tools/linux/compare_dsp_replay.py`
workflow from the mods repository, with explicit record-count comparison and
a clean skip when private inputs/tools are absent. `dsp_transcript_diff.py`
compares MAIN host-port event sequences; it is not the DSP core hash oracle.
Build the offline runner and replay the whole recording with no cycle cap:

```sh
make -C hw/cdj/c6x -j6 O="$PWD/build/c6x" "$PWD/build/c6x/c6xreplay"
python3 scripts/test/dsp_native_replay.py \
  --record /path/to/private/dsp.c6rec \
  --module "$HOME/c14gen/curated-cdj2000nxs/m.so" \
  --module "$PWD/runs/nxs-extra/m.so" --out runs/nxs-exact-replay
```

`--out` must be new. Omit `--module` to check the installed `m.so` and
`m.extra.so`; `--runner` can select another host runner. The gate clears
inherited JIT/replay knobs, sets the NXS idle skip/ISR options, runs interpreter
then native, and fails on divergence, incomplete replay, module load errors,
unequal record counts or unequal final hashes. Logs/receipts stay under `--out`.
