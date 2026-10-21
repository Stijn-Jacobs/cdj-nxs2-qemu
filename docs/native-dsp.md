# Reproducible NXS native DSP coverage

`CDJ_MODEL=cdj2000nxs scripts/build/build_dsp_module.sh` builds the curated
module from your own extracted firmware and DSP profile, then makes a second
pass over that same profile with `MODEL_DSP_SUPPLEMENT_ARGS` in
`models/cdj2000nxs.conf`:

```
--roots 400 --min 100000 --nodes 400 --kernels 16 --cold 2000 --idle-head 0xc004cb8c --jobs 6
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

On 2026-10-13, a local checkout regenerated the
supplement with 400 roots and all other options unchanged, using the same
private trial profile at `/tmp/cdj-dsp-matrix/mac-mp3-320-v2/profile` and its
`ram_*.bin` images. These inputs and the recording were checked against the
prior private trial manifests. The output is private under
`runs/native-dsp-roots400`; nothing was installed into `~/c14gen`.

Sequential offline replays in that session measured:

| Module chain | Replay seconds | Supplemental `m.so` bytes |
| --- | --- | --- |
| Curated + regenerated roots 100 | 58.41, 58.80 | 2,289,560 |
| Curated + regenerated roots 400 | 48.96, 53.53 | 6,941,912 |
| Curated baseline alone | 98.40 | — |
| Installed curated + existing supplement | 103.89 | 2,289,560 (`m.extra.so`) |

The curated `m.so` is 23,189,480 bytes. The roots-400 generation and compilation
together took 26 s with six jobs (the core library was already up to date),
producing 400 regions, 406 clean entries and 16 kernels. Compared with the fresh
roots-100 samples from `runs/native-dsp-trial-regenerated`, the two-sample mean
fell from 58.605 s to 51.245 s: 12.6% less replay time for a 3.03x larger
supplement. These are timing samples, not a broader performance qualification.

Both roots-400 exact-replay gates passed all 48,115,851 records with identical
final state, write and RAM hashes against their interpreter runs (148.68 s and
159.66 s), the fresh curated baseline, both fresh roots-100 runs, the installed
native chain and the prior trial receipt. Private receipts are under
`runs/native-dsp-roots400-gate-1` and `runs/native-dsp-roots400-gate-2`; comparison
receipts are under `runs/native-dsp-measure-*`. No deck or QEMU guest runs were
used for this regeneration or measurement.

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
