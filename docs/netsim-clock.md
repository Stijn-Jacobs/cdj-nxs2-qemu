# Bounded wall clock for a local Link segment

The default `CDJ_NETSIM` mode still requires `-icount shift=1,sleep=off`
(or another explicitly selected icount configuration). Opt into the new clock
with these environment variables on every deck on the segment:

```sh
CDJ_NETSIM=unix:/path/to/segment.sock
CDJ_NETSIM_CLOCK=wall
CDJ_NETSIM_CLOCK_FILE=/path/to/fresh-segment-epoch
CDJ_NETSIM_QUANTUM_US=1000
```

Omit `-icount` in wall mode. The hub still runs with `--sync --decks 2` and
uses the unchanged CDJC protocol. Both QEMUs must be on the same host, use the
same quantum and map the same fresh epoch file. Create a new private segment
directory for each run; remove it only after all its processes have exited.
The epoch is an eight-byte shared monotonic timestamp, not an observer file.
Keep each deck's `CDJ_VCLOCK_FILE` separate: those files remain ASCII guest ns,
updated roughly every 50 guest ms. They cannot measure sub-ms clock skew.

Each deck's clock is `min(host_monotonic - epoch, next_boundary)`. A boundary
is at most the hub's peer promise plus the segment quantum, and no later than
the earliest queued packet. Timer dispatch can be arbitrarily late without
letting the clock cross that boundary. The timer drains received messages,
delivers due packets in stamp order (arrival order for ties), reports its
current time, waits for a safe grant and installs the next boundary. TCG runs
without instruction counting inside the interval.

Wall mode always promises its current time, including when idle. If `Pi` is
deck i's last promise, `Pi <= Ti`, so its peer's clock satisfies
`Tj <= Pi + Q <= Ti + Q`. Together with the reverse inequality, clock skew is
bounded by Q. A frame sent at t is delivered at t+Q; the receiver cannot pass
that time before learning about it. The existing sorted packet queue remains
unchanged. VM pause freezes the local clock and backpressures peers; this mode
resumes against the live host epoch and can catch up after a pause. Debugger
sessions must be excluded from performance admission. This mode does not
provide instruction-count determinism, migration or replay support.
It fails closed if the hub is lost.

`CDJ_NETSIM_CENSUS=5` reports barrier waits, raw icount (zero in wall mode),
delivery lateness and sends before a declared promise every five guest seconds.
`CDJ_NETSIM_TRACE=/path/to/trace.jsonl` records send/receive stamps, delivery
deadlines and diagnostic frame hashes. The hashes include Ethernet minimum
padding so peer transmission and receipt records can be matched.

## Session integration

In mods' `tools/emulator/stijn/session.py`, add an explicit clock option rather
than changing existing qualification defaults. When that option selects wall:

1. The two-deck coordinator creates one fresh segment epoch pathname and
   supplies it to both sessions.
2. `build_command()` forwards `CDJ_NETSIM_CLOCK=wall` and
   `CDJ_NETSIM_CLOCK_FILE=<that pathname>` when `link_sync` is set.
3. It omits the current `argv += ['-icount', 'shift=1,sleep=off']` in that branch.
4. It records the selected mode, epoch pathname, QEMU hash and quantum in the
   run receipt. The observer `CDJ_VCLOCK_FILE` contract stays per deck.
5. Admission tooling must recognize the wall mode explicitly and still check
   real playback speed, real beat-report consumption and packet timing.

No mods files are changed by this fork. `scripts/test/netspeed.py` implements
the coordinator for local experiments. Its DHCP hub only supplies normal
address configuration; all beat and status packets come from firmware.

## Regression checks

```sh
python3 -m pytest -q tests/test_netsim_clock.py tests/test_net.py tests/test_link_probe.py
python3 scripts/test/test_sh_tmu.py
```

The clock test compiles production netsim with QEMU plumbing stubbed. It
checks shared epoch initialization across processes, delayed dispatch and
pause, conservative idle promises, out-of-order frames, equal-time FIFO,
exact delivery deadlines, barrier updates and invalid clock files.

## Capability discovery

The NXS machine description in `qemu-system-sh4 -M help` advertises
`[netsim-wall-v1]`. This is the versioned launcher capability contract for
the bounded local clock above; probing it neither boots firmware nor creates
a segment. Binaries without it must not receive wall-mode launch arguments.
