# scripts/debug

## gui_dump.py: readable GUI runtime state

Python 3 stdlib only. Needs `bfin-elf-nm` on PATH (or `--nm` / `$BFIN_NM`). All
addresses come from symbol names in the ELF you pass; symbols the ELF lacks are
reported as "not in this ELF".

    gui_dump.py mods.elf --gdb /path/to.sock          # interrupt, dump, continue
    gui_dump.py mods.elf --gdb host:port --stay       # leave it halted
    gui_dump.py mods.elf --gdb SOCK --watch cdj_gui_rt::time::TICKS --max 5
    gui_dump.py mods.elf --offline DIR                # sdram.bin, l1a.bin, l1b.bin[, l1s.bin]

`--gdb` is whatever the machine was started with in `CDJ_BF531_GDB` (unix path,
`host:port` or port). `--watch` sets a write watchpoint (Z2) per symbol, prints
a dump on each hit (server stop reply shown verbatim) and continues; `--max N`
hits then it removes the watchpoints and detaches. Offline mode reads
`CDJ_BF531_DUMP` output; it has no registers, so stacks are scanned from their base.

Dump: registers (PC/RETS/RETI symbolized), executors, task pools, kernel words,
timer queues (deadline relative to TICKS), message rings, watchdog counters, and
a return-address scan of each `_*task_stack` from SP to its `_end`.

Tests: `python3 scripts/debug/test_gui_dump.py` (builds the standalone
`hw/cdj/bfin/tests/gdb_server`, assembles a tiny ELF; no emulator).
