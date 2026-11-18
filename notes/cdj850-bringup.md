# CDJ-850 bring-up log

UPSTREAM_BASE=9042f71c674fc45f62b284764a21ba1d9451bb67

Upstream: https://github.com/Stijn-Jacobs/cdj-nxs2-qemu.git
Branch: cdj850-bringup (off main at UPSTREAM_BASE, 2026-10-12)
Goal: add Pioneer CDJ-850 firmware support to the emulator.
Constraint: never commit C850MAIN.UPD or anything derived from it.
New code license: GPL-2.0-or-later.

## Corrections (2026-11-16) — read before trusting older entries

The entries below are history and are left as written. Several
attributions in them were later shown to be wrong (commits cde25d1,
011ec7d; evidence is the service manual 10.2 MAIN ASSY schematic and the
MAIN image). Where an older entry rests on one of them it carries a
`SUPERSEDED` marker under its heading. Current code comments in
hw/cdj/boards/cdj850/xc/ (intc.c, dsp_shi.c, port.c, scif.c,
panel_peer.c) are authoritative.

- **SSU0 (0xFFFE7000) is the audio DSP link, not the panel link.** It is
  the SHI of the DSP56371 (IC301): IC103 PF0/SSCK0 = AUDIO_SCK, PF1/SSI0 =
  AUDIO_SI, PF2/SSO0 = AUDIO_SO, PF3/SCS0 = DSPCS. The DSP is strapped to
  boot mode 5 (SHI slave, SPI). The 1140-byte "panel boot upload" is DSP
  bootstrap framing: 24-bit words, a word count, a load address, then the
  program words. MAIN holds four such segments at 0x0C155B28-0x0C1593A1
  (9, 82, 896 and 3824 words; the first three end in JMP $FF0000, i.e.
  back into the bootstrap ROM). The table at 0x0C1C1AE8 lists the DSPDREQ
  level MAIN expects after each segment: 0, 1, 0, 1. The earlier
  conclusion that no DSP program exists in the MAIN image is wrong. The
  6-byte `c2 00 00 08 00 00` exchange on SSU0 (wrapper 0x0C077570) is a
  DSP exchange, not a panel exchange. xc/panel_link.c and xc/ser.c are
  misnamed (they model the DSP side of SSU0; names kept for now).
- **The panel CPU is on SCIF1 (0xFFFE8800)**, clock-synchronous slave
  (MAIN) with the panel as clock master. Pins: PE3/SCK1 = TSCK, PE5/TXD1 =
  TSO, PE4/RXD1 = TSI, PE9/IRQ5 = PNL_XBUSY (panel to MAIN), PC14 =
  MAIN_XBUSY (MAIN to panel). One transaction per PNL_XBUSY rising edge,
  two or three 16-byte full-duplex frames. Type 1: byte 0 = 0x01, byte 15 =
  checksum of bytes 0..14. Type 2: byte 0 = 0x02, byte 12 = checksum of
  bytes 0..11. Checksum = 8-bit end-around-carry sum (routine 0x0C0F9EE8).
  ISRs: IRQ5 0x0C0FA05E, TXI1 (vector 199) 0x0C0FA13C. SCIF0 is the ASCII
  debug console. Evidence: MAIN driver (init 0x0C0F9F7A) plus schematic
  nets.
- **Interrupts.** The firmware registers 31 ISR descriptors; before
  cde25d1 only CMT0 (vector 142) was ever delivered. External IRQ routing:
  IRQ1 = PB1 SRVSBSY, IRQ2 = PB2 SPDLFG, IRQ4 = PB4 ZERO, IRQ5 = PE9
  PNL_XBUSY, IRQ7 = PE11 DSPDREQ (ICR1 = 0x4924, IPR01 = 0x0770, IPR02 =
  0x7907 as the firmware programs them). The "outer caller 0x0C105004"
  with zero hits in the dispatch-arming analysis is the IRQ1 handler
  (vector 65); 0x0C107484 is the IRQ2 handler (vector 66), 0x0C107232 the
  vector-146 handler. Nothing in those analyses could fire them because no
  external IRQ was modelled.
- **Port E pins.** DSPDREQ is PE11, not PE12. PE7 = SRVRST and PE6 =
  SRVCCE (Task 8m's report had PE7 = SRVCCE and PE6 = spare); PE8 =
  SRVBCLK, PE12 = SRVBUS0. PE6/PE8/PE12 are therefore not DSP READY lines.
  The Port-E bit-bang stream (bring-up 0x0C0FD7CC, transfer 0x0C0FC9DC,
  validator 0x0C0FC71C-87E) is the CD servo bus; Task 8m's conclusion on
  that stands. The 519-byte "DSP init image" is a servo image.
- **Board input pins that read 0 and should not:** PB3 = 5MON (5 V
  monitor, high on a running unit; the power task 0x0C0FF7DA counts lows as
  a supply fault); PF25/PF26 = LPS1/LPS2 loader switches, each pulled up by
  10 k (R140/R141); PF24 = INSIDE, no pull-up on the sheet, stays 0.
- **"E90" on the debug UART.** In every run, including the old baseline,
  the firmware prints `E90\r` on SCIF0 about 11-15 s after boot: error
  code 0x90 from the CD loader state machine at 0x0C106194-0x0C10622E
  (reported via 0x0C102F70; it waits for LPS2 high and times out). It went
  unnoticed because SCIF TX is only captured under CDJ850_TRACE.
- **Status (2026-11-16).** With CDJ850_DSP_BOOT_HLE=1 MAIN uploads all four
  DSP segments. The panel link exchanges frames (vectors 69 and 199 taken,
  frames accepted). The CD loader model (xc/loader.c + xc/mtu2.c: PWM on
  PD10/TIOC3C, enable PD9, switches LPS1/LPS2) lets the no-disc homing
  finish: loader state 0x0C7F6D98 = 0, no "E90", and the display pages
  MAIN sends read "EJEC" then "NO DISC". The unit therefore reaches its
  idle no-disc state. Open: execution of the
  uploaded DSP program, CD servo (only needed with a disc).

- **USB host (2026-11-16, reviewed and verified).** The 0xFFFC1C00 block is the
  SH7263 USB module, but its register layout is NOT the SH7724 one in
  hw/cdj/common/usb_r8a66597.h (the "offsets match exactly" claim in the
  Task 8 entry below is wrong): SYSSTS +0x02, DVSTCTR +0x04, CFIFO/D0FIFO/
  D1FIFO +0x10/+0x14/+0x18, CFIFOSEL +0x1E, PIPECFG +0x66, DEVSEL in MAXP
  bits 15:14, transaction counter in DnFIFOTRN (manual Table 25.2, and the
  firmware's own accesses). xc/usb.c translates onto the shared model;
  xc/dmac.c models the DMAC channel the driver uses for sector data (ch7,
  DMARS3 = USB_DMA0, DEI7 vector 136). USBI is vector 140, IPR08[15:12]
  (firmware sets 6). PE10 = USB_CDETFLG (service manual 10.15: H = normal,
  L = over-current); it used to read 0, and the firmware (poll 0x0C10DCEA,
  flag 0x0C1E1271) reported a USB over-current all the time. PB10 =
  USB_ABSEL (H = A/host port), PB11 = USB_VBUSON. With
  `MAIN_ARGS="-drive if=none,id=usbstk,format=raw,file=<img> -device
  usb-storage,drive=usbstk,port=1"` the firmware enumerates the stick,
  runs BOT, reads the FAT16 tables and root directory, and creates
  /PIONEER/{CDJ,MPJ,USBANLZ} and /PIONEER/USBANLZ/USBMNG.DAT on it (use
  snapshot=on to keep the image unchanged). Display text stays "NO DISC".
  Review fixes preserve the programmed DnFIFOTRN preset when TRCLR clears
  the current counter (manual 25.4.4(e)), hold DEIn while TE and IE remain
  set (100 us re-evaluation also picks up a later INTC priority enable),
  and keep reserved DMAC offsets 0x80..0xFF from indexing nonexistent
  channels 8..15. Five QTest regressions in `tests/test_cdj850_usb.py`
  exercise both FIFO presets, DMA addressing, reserved storage, delayed
  priority enable and an uncleared completion interrupt. They run the
  built big-endian QEMU with a synthetic ROM, without Pioneer firmware.
  All five pass after rebuilding. Full suite: 687 passed, 38 skipped,
  1 xfailed; the existing baken download-preview test now explicitly
  simulates a missing tool. Firmware boots with and without USB reach
  idle without E90 or queue overflow. USB trace: 144 READ(10), 40 WRITE(10),
  184 DMA completions. A disposable FAT16 copy contains USBMNG.DAT
  (400058 bytes, PMNG header), with matching FAT copies. Original update,
  flash and USB image checksums are unchanged. Audio playback is unverified;
  DSP program execution remains unimplemented.
  Known limits: DnFIFOTRN readback currently returns its programmed preset
  even with TRENB set; hardware then returns the current ascending count.
  USB frontend state (local registers, cached pipe directions, interrupt
  level/poll timer) is not migrated, so save/restore with USB attached is
  not supported. `snapshot=on` above is block-image write isolation, not
  a VM save/restore guarantee. Other peripherals also have incomplete
  migration support; these review fixes do not establish it.

Environment knobs added by these commits (all read once at start-up):

| Variable | Default | Effect |
|---|---|---|
| CDJ850_IRQ_PULSE=`n@ms[,n@ms...]` | unset | Diagnostic: invert IRQn pin for 1 ms at virtual time `ms`. Invents an event; nothing may depend on it. |
| CDJ850_IRQ_IDLE=`0xNN` | 0xC0 | Idle level of IRQ0-7 pins, bit n = IRQn (default: IRQ7/DSPDREQ and IRQ6/USB_CDETFLG high; 0x80 = the old level, which reads as a USB over-current). |
| ~~CDJ850_DSP_BOOT_HLE=1~~ | removed | Obsolete (2026-11-17): the DSP56300 core (dsp56300.c) runs the uploaded program, which drives DSPDREQ itself. Formerly: toggle DSPDREQ after each DSP SHI segment (idle high: 0,1,0,1). Stand-in for the DSP program. |
| CDJ850_DSP_DEBUG=1 | off | DSP core diagnostics: DSPDREQ edges, SHI RX transitions, SHI TX (first 40), unimplemented ops, ESAI RX/TX counters and TX peak per 500 ms, ESAI vectors/registers once at tick 3000. |
| CDJ850_DSP_IPS=`n` | 256 | DSP instructions per ESAI slot (88.2 kHz). Real-time audio needs about 750 (~66.2 MIPS); default 256 (~22.6 MIPS) keeps live non-`icount` responsive while higher values depend on host CPU headroom. Without `-icount`, DSP execution runs on a dedicated worker thread (`dsp_worker_thread`, paced against `QEMU_CLOCK_VIRTUAL`); with `-icount`, `dsp_audio_tick` runs synchronously in lockstep—use `CDJ850_DSP_IPS=2048` for exact deterministic audio. |
| CDJ_DSP_AUDIO=1 | off | Open the DSP's ESAI TX0 as a host audio voice (`cdj850-dsp-esai0`); needs `-audio driver=…`. |
| CDJ850_PF16_HIGH=1 | off | OBSOLETE (2026-11-17): PF16 is a MAIN output (PFIORH = 0x0001) that MAIN drives itself; PFPRH now reads it back from PFDRH (e47bad7). Forcing it high only hides the real level. |
| CDJ850_DSP_PLAY_PF=`n` | unset = MAIN PF16 | Overrides the DSP PF10 (PLAY) source with MAIN PFDRL bit `n`. Default since e47bad7 is MAIN PF16 (PFDRH bit 0), per service manual 10.2/10.4. Leave unset. |
| CDJ850_5MON=0 | 5MON reads 1 | `0` makes PB3 read low (old behaviour). |
| CDJ850_LOADER_SW=`0xNNNN` | 0x0600 | Overrides PFDRH bits 8-10 (INSIDE, LPS1, LPS2); default LPS1/LPS2 high, INSIDE low. |
| CDJ850_PANEL=0 | enabled | `0` disables the emulated panel peer (xc/panel_peer.c). |
| CDJ850_PANEL_PERIOD_MS=`n` | 10 | Idle gap between panel transactions; 0 or unparsable gives the default. The real rate is unknown. |
| CDJ850_PANEL_SCRIPT=`list` | unset | Comma-separated scripted SCIF1 panel events (`<ms>:<btn>[:<hold_ms>]`, `<ms>:+<btn>`, `<ms>:-<btn>`, `<ms>:rot+/-N`). |
| CDJ850_LOADER=`0` | model on | Disables the loader mechanism model; LPS1/LPS2 read the constant 0x0600. |

## Task 1 baseline (2026-10-12)

- Step 1: cloned upstream, created branch `cdj850-bringup`. Base short hash: 9042f71.
- Step 2: `split_update.py` is a script (argv parsing at top level), so a bare
  `import split_update` raises IndexError by design; `py_compile` passes and
  the importable tooling module `lzss_decode` imports cleanly.
- Step 3: all six "Adding a model" prerequisite paths exist:
  models/cdj900.conf, hw/cdj/boards/cdj900/board.c, hw/cdj/common/cdj_common.h,
  scripts/run/boot_main.sh, scripts/firmware/prepare_firmware.sh,
  launcher/firmware.py, launcher/model.py.
- Step 4: this file force-added (`git add -f`) because upstream .gitignore
  ignores `notes/` (local-data dir); committed as the branch's first commit.

## Task 5 skeleton + first boot (2026-10-12)

Machine `cdj850` (hw/cdj/boards/cdj850/board.c + meson.build, Kconfig
`CONFIG_CDJ850` selecting `CDJ_COMMON`, `default_cpu_type = -cpu sh2a`;
runs in the sh4eb target: ~/qemu-build-eb/qemu-system-sh4eb).

- fw_entry 0x00000100, init_sp 0xFFF90000: SH-2A power-on reset loads PC
  from the vector at 0 and SP from the vector at 4 (SH-2A Software Manual
  §3; SH7263 hardware manual §5.2.3); the boot region holds PC = 0x100,
  SP = 0xFFF90000 (manual-reset copies at 8/12). init_sr = 0 keeps the
  CPU reset value; the first three instructions re-mask anyway
  (`stc sr,r0; or #-16,r0; ldc r0,sr`, seen in `-d in_asm`).
- DRAM 32 MB at CS3 0x0C000000 (256 Mbit K4S561632J / 8 = 32 MB).
- Flash 4 MB at CS0 0x00000000 (DYW1796 32 Mbit / 8 = 4 MB;
  63 x 64 KiB + 8 x 8 KiB = 4096 KiB; big-endian bus, new `flash_be`
  descriptor flag; flash IDs unverified).
- Clocks: bus CLK_66M 66 MHz (service-manual block diagram; feeds flash +
  SDRAM); X101 33 MHz crystal (CPU/bus PLL on EXTAL), X102 48 MHz (USB).
- PVR/PRR/CVR stay 0: the SH7263 has no SH-4-style memory-mapped PVR and
  the boot code holds no 0xFF00xxxx literals; QEMU never reads the IDs on
  translate/helper paths.
- On-chip RAM 256 KB at 0xFFF80000; `sh7263.internal` unimp 0xFFFC0000 up
  (BSC/CPG/INTC/DMAC/ports); `sh7263.cache` unimp 0xF0000000 (16 MB) +
  0xE0000000 (4 KB); open-bus CS0-rest/CS1/CS2/CS3-rest.

Boot (firmware installed locally, untracked):
  scripts/firmware/prepare_firmware.sh --model cdj850 --install C850MAIN.UPD
  (+ boot.bin copied from a plain prepare outdir: --install only copies
  MODEL_EXPECTED images)
  python3 scripts/firmware/make_flash_cdj850.py  # extract/cdj850/flash_cdj850.bin
  CDJ_MODEL=cdj850 MAIN_QEMU=~/qemu-build-eb/qemu-system-sh4eb \
    MAIN_BOOT=flash FLASH=extract/cdj850/flash_cdj850.bin \
    bash scripts/run/boot_main.sh bringup0 30

First boot (30 s, EB binary, no resets, 654 log lines): entry 0x100 runs
real code; the first access is an SDRAM mode-register write
(`sh7263.internal: unimplemented device write (size 4, offset 0x01000,
value 0x00000000)` = 0xFFFC1000), then CPG (0xFFFE0002/04/10/18), ports
(0xFFFE3890+), BSC (0xFFFC0004/10/28, RTCOR 0xA55A keys), cache-array
writes (0xF0800228+ stride 0x10, value 0x0DDEF800), and a descending
zero-fill at 0xFFFC3FCA4+. Totals: 591 internal writes, 32 cache writes,
29 internal reads (MRS/CPG/port readbacks). The 8 s and 30 s runs log
identical counts: after init the firmware parks (likely sleeping for an
unmodeled INTC/CMT tick). Next: model the internal block (Task 6).

## Task 6 increment 1: INTC + CMT (2026-10-12)

Park analysis (monitor `info registers` on a live boot): pc=0x0C0B8DBE
(SDRAM MAIN code), sr=0x400000e0 (IMASK=14), sp=0xFFF8FFF8 (still the
reset stack), r0=0xFFFFFFFB (-5), r22=0xFFFEC000 (CMT base). Capstone-BE
disassembly of guest bytes shows the init sequence calls its first
subroutine (target from pool 0x0C0B8F78 = 0x0C0CDDCA, arg r4=1), gets -5,
and takes `bf 0x0C0B8DBE` into a `bra self` halt loop. (QEMU monitor
`x/i` mis-decodes the branch target as 0x0C0B8DBA -- trust Capstone, not
`x/i`, for SH-2A code.) The subroutine is TOPPERS-style kernel-object
init (error immediates -25/-18/-17/-34/-41/-5 = E_CTX/E_ID/E_PAR/
E_NOID/E_OBJ/E_SYS); nearby pool holds "freeWorkComm: pointer is
NULL!!!" strings. Boot-log order shows the firmware wrote CMT CMSTR=1
(CMCOR 0x1045 = 1 ms tick at Pphi/8) and later READ CMSTR back; the
unmodelled read returned 0, so the firmware concludes the timer never
started (E_SYS) and halts. Fix: a real ticking CMT with readback.

Modeled this increment (commit `feat(cdj850): model INTC+CMT ...`):
- CMT ch0 0xFFFEC000/0x10 (new hw/cdj/boards/cdj850/xc/cmt.c): CMSTR/
  CMCSR/CMCNT/CMCOR with readback, CMF write-0-clear, 1 ms QEMU tick,
  SH-2A irq queue delivering CMI0 vector 188 level 8 (family value from
  the SH7269 board; SH7263 manual table unchecked). Ch1 storage only.
- INTC 0xFFFE0800/0x800: plain storage for ICR/IPR RMW (delivery does
  not consult it).
- CCR 0xFFFC1000: u32 storage (boot cache-init sequence reads back).
- Cache windows as RAM sinks (0xF0000000/16 MB, 0xE0000000/4 KB): the
  boot tag pattern must read back.
- On-chip-hi RAM 0xFFFF0000/64 KB: the descending zero-fill
  0xFFFFFCA4-0xFFFFFEA3 must stick (later peripherals here carve out
  with higher-priority overlaps).
- CCN audit shadow 0xFF000000/0x30 (Task 5 concern): identical semantics
  to the mapped SH-4 CCN model (EXPEVT/INTEVT mirror env, rest zero) but
  reports the first accesses with guest PC, so a MAIN touch at
  0xFF00xxxx becomes visible. Candidates 0xFF000000/08/24 exist as
  single literals in main_flat.bin (contexts look code-ish, verdict
  open); UBC 0xFF200000 was already a logging unimp.
- Left unmodelled (writes succeed, no reads seen yet): BSC, CPG, ports.

Correction to the Task 5 paragraph above: 0xFFFC1000 is modelled here as
the cache-control word (CCR), not an SDRAM mode register -- the access
pattern (writes 0, 0x808, 0x101, two readbacks) is the boot block's cache
init, and the SDRAM mode setup lives in the BSC writes at 0xFFFC0000+.

Port gap (same increment): the remaining 25 reads are all port
readbacks (0xFFFE3890-96, 0xFFFE3910-16, 0xFFFE3990-96, 0xFFFE3A10-16,
0xFFFE3A86-96 at 1222-1224 ms, pcs 0x0C0B9090-0x0C0B91CE), each preceded
by a write to the same offset with the same value (CDJ850_TRACE=1).
Modelled as plain storage 0xFFFE3800/0x800 (no strapped pins needed).
After this increment the unimp log is 18 writes, 0 reads (BSC
0xFFFC0004/10/28/34/4C/50/54/58 incl. RTCOR 0xA55A keys, CPG
0xFFFE0002/04/10/18, PFC 0xFFFE0408/0C/10/14/18, 0xFFFC5040) -- down
from 591+32+29. CCN audit shadow saw zero touches in the full boot, so
MAIN does not use 0xFF000000-0xFF00002F (the three single literals
0xFF000000/08/24 in main_flat.bin are data, not executed MMIO).

Debug method note: QEMU monitor `x/i` mis-decodes SH-2A branch targets
(`bf 0x0C0B8DBE` shown as `bf 0x0C0B8DBA`) and 32-bit insns; use
Capstone-BE on `xp`-dumped bytes plus `--gdb tcp:...` single-stepping
(`s`/`g`/`m`/`M` packets, scripted; do NOT send QStartNoAckMode
repeatedly, use one session per boot) for ground truth. QEMU watchpoints
+ single-stepping 32-bit code can trip a `record_save` translator
assertion (debug path only; normal boots unaffected).

Pool-2 wall (park persists at 0x0C0B8DBE, r0=-5=E_SYS): single-step
traces prove the failing call is the RTOS object-create 0x0C0CDDCA
whose second small-block alloc (pool 2, 0x28 B via 0x0C0D0256, and an
earlier 0x44 B one via 0x0C0C72A2) returns NULL because pool 2's arena
base [0x0CE4A330] (static [0x0C18BF68]) is 0 and no executed code ever
writes it nonzero (2 BE32 refs in the whole SDRAM image, both reads).
Pool 1 (15 MB arena at 0x0CEEFA20) works. No device reads happen
between pool init and the halt (traced), no CS/external touches at all,
flash ID mode is never entered (no 0xAAA/0x554 sequences), and 0xFF
power-on fill of OCRAM/hi-RAM changes nothing -- so this is not a
missing-peripheral gap but an RTOS-init precondition. WHY the base is 0
on real HW too (same image/code) is unexplained; candidates exhausted:
not ports/CCR/CMT/INTC readbacks (all consistent), not CCN, not flash
IDs, not RAM power-on.
  Local diagnostic bridge (NOT committed, runtime-only): halt at -S,
  `Mce4a330,8:0DDEFA20 00100000` (pool-2 base = free SDRAM hole
  0x0DDEFA20, size 1 MB), cont. Boot then proceeds through RTOS init
  into driver init, next gap: tight size-1 read poll of 0xFFFE7004
  (pc 0x0C077C3C, r0=0xFFFE7004). Continue the per-gap loop from there
  with the bridge applied; each new peripheral still gets its own
  commit below.

## Task 6 increment 2: SCIF/serial 0xFFFE7000 (2026-10-13)

Adopted the uncommitted `cdj850_ser_*` stub (+106 lines in board.c):
kept, with one fix. The stub is sound: setup writes +3/+0/+1/+2/+4/+3
(0x00/0x80/0x40/0xC5/0x04/0x80 -- SCIF SCSMR/SCBRR/SCSCR-style init),
1140 TX bytes through +6 with a `read +4, and #4, loop while 0'
TX-ready poll (driver 0x0C077B06/0x0C077C3C), teardown read +3 / write
+3=0. Bridged boot (`CDJ_RPOKE="0x0c0d0122:0:0x0c400000:72:209715200,
0x0c0b8db2:0:0,0x0c0ff6c6:0:0,0x0c00570a:0:0"`, see below) sends the
full packet and the driver completes (port write +0x282, port read
+0x21E follow). Fix: busy reads reported 0x0C (bits 2+3), so the
`and #4' poll saw ready at once and the fake transfer never waited;
busy now reports 0x08 (bit 3 shifting, TX-ready clear), ready 0x04.
The packet is binary, not text, so this channel is a protocol link
(panel/DSP candidate), not the DBG_TxD console -- the stub stays a
TRACE logger (CDJ_FWLOG equivalent). Packet archived untracked for
Tasks 7/8: `extract/cdj850/ser_tx_bringup2.bin` (1140 B, sha256
395475ef...f91196; first 00000900000030001304d03f07b49e0, last 45c8...
0af080ff0000).

Bridge note (correction to the paragraph above): the GDB `Mce4a330'
poke applied at reset is cleared mid-boot ([0x0CE4A330] reads back 0
at the E_SYS halt; the 0x0DDEFA20 hole holds image bytes then, not
free space). The working bridge is the committed `CDJ_RPOKE'
translator facility: RPOKE[0] (allocator ramp at 0x0C0D0122 into
0x0C400000+72) is load-bearing; RPOKE[1..3] (r0=0 at 0x0C0B8DB2/
0x0C0FF6C6/0x0C00570A) are no-ops (r0 already 0 at first hit) kept as
guards. MAIN is not verbatim in flash (main region entropy 7.69 =
compressed; strings like `freeWorkComm' exist only in SDRAM), so all
firmware forensics below are against live `xp`/memsave bytes, with
Capstone-BE (QEMU `x/i` mis-decodes SH-2A; Capstone 5.0.7 itself
misses some SH-2A 32-bit forms -- trust `in_asm`, not Capstone,
for what executed).

## Task 6 increment 3: mailbox/host-port 0xFFFFC000 (2026-10-13)

Gap: bridged boot with increment 2 parks spinning at 0x0C075EA2
(`mov.l @(r0,r1),r2; bld #0,r2; bf self` = wait bit0 of 0xFFFFC004).
Driver 0x0C075E40 (called from 0x0C075E36, pr=0x0C075E3A): write
command to +0x000, clear +0x004, poll +0x004 bit0, read response from
+0x000; second identical channel pair at +0x800/+0x804 (driver
0x0C076006, same sequence). Status word also carries bit24 (second
driver 0x0C075EF6 polls it: `tst 0x01000000; bt wait'). Modeled
(board.c `cdj850_mbox_*`, window 0xFFFFC000/0x810, priority-2 carve
out of the on-chip-hi RAM): data words echo last write (RAM-identical),
status words (+0x004/+0x804) OR 0x01000001 (always-ready; the driver
has bit0-clear init paths at 0x0C075F34 that proceed under it too).
Boot evidence (bringup2, 20 s, bridge+TRACE): all 14 mbox accesses
clean -- +0x004/+0x804 read 0x01000001, commands 0x00000000 then
0x00090102 written to both +0x000/+0x800; the
0x0C0CF4C4(0x0C155AAC,0) validation returns 0 (no E-6002-style halt
at 0x0C075ECA). Unimp 19w+1r -> 30w+9r (strict progress): new touches
CPG STBCR2 0xFFFE0018 (R, size1), DMAC DAR_1/CHCR_1 0xFFFE1014/0x101C
(W 0xFFFFC008, W 0/0x1810, R), DMARS0 0xFFFE1300 (RMW 0x2300), same
trio for channel 2 (0xFFFE1024/0x102C/0xFFFE1304=0x0027), BSC(?) read
0xFFFC750C (size1), then port polls. No DMAC stub: 0x1810 leaves DE=0
(mode only, no transfer; SAR/DMATCR never programmed), so storage
would only risk a busy-spin regression -- recorded for later. DMAC
map (from image pools at 0x0C1C14E0+: SAR/DAR/DMATCR/CHCR at
0xFFFE1000+0x10n, DMAOR 0xFFFE1200, DMARS0/1 0xFFFE1300/0x1304,
STBCR2 0xFFFE0018/MSTP8) matches the SH DMAC family (app-note
cross-check).

Park (NOT the milestone): after the port polls the firmware `rts's
into data at 0x0C0C78D8 (pr=0x0C000008, stale on ALL port entries;
sp pristine 0xFFF90000), garbage-executes to an illegal insn, and
lands in the flash halt field at 0x000002F8 (`bra self`, pid-visible
for the rest of the run; CMT vector 188 pends, IMASK=14). This is not
a peripheral gap: the bootloader jmp @[0x49C]=0x0DDEFB7C entry is
legitimate (flash vector), control is register-clean through the
mailbox validation (pr verified at 0x0C075E5C=E3A, 0x0C07607E,
0x0C0CF4C4, orchestrator-ret 0x0C00575C), and zero-filling the RPOKE
scratch (GDB `M0c400000,1000:00..', verified zeros) reproduces the
identical halt. Prime suspect: pool-2 objects consumed unconstructed
(RPOKE bypasses RTOS init) or DMA-absent buffer state. Next step:
fresh-code forensics on the orchestrator(0x0C00575C-ret)->port-entry
path (DMA driver text before pool 0x0C1C14E0; stale dumps unreliable
there -- use live `xp'), watching for the first `lds pr'/indirect
call off the uninitialized structs.

For Task 7 (panel): ser packet above + PFC 0xFFFE0414 RMW + CPG
0xFFFE0018/STBCR2 patterns. For Task 8 (DSP): mailbox channels
0xFFFFC000/4 + 0xFFFFC800/4, command words 0x01000000/0x90102, SDRAM
mirrors 0x0CEE33D0/0x0CEE33D4/0x0CEE3160/0x0CEE34C1, validators
0x0C075E40/0x0C076006/0x0C0CF4C4/0x0C0C688A/0x0C0C6894, data
0x0C155AAC, DMA DAR_1/2 hooked to mbox+8 (+0xFFFFC008/+0xFFFFC808).

## Task 6c: genuine pool-2 init — stale SR.RB strands the pool-2 base (2026-10-13)

Wall (no bridge) reproduced exactly: pc=0x0C0B8DBE, r0=-5, sr IMASK=14,
18 unimp writes / 0 reads. Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-6c-report.md. Forensics
(GDB RSP + `-d in_asm`, in_asm authoritative; scripts in /tmp t6c_*.py):

- The failing call is RTOS object-create 0x0C0CDDCA (from 0x0C0B8D96,
  id=1): its pool-A alloc (via 0x0C0D0230, free list at [0x0CE4A32C] =
  0x0CEEFF30) succeeds, but the 0x28 B pool-2 alloc (via 0x0C0D0256)
  NULLs (head var [0x0CE4A330] = 0) -> E_SYS (-5) -> `bf` to the
  `bra self` halt. The earlier 0x44 B pool-2 alloc (via 0x0C0C72A2 from
  0x0C0895AC) fails the same way; its caller tolerates -5 (stores -1).
- Pool-2 can never fill through MAIN code: exhaustive BE32 scan of a
  wall-time pmemsave (32 MB SDRAM) shows [0x0CE4A330] has exactly 3
  refs (1 writer: the pool-register store at 0x0C0C6BBE; 2 alloc
  readers) and the flag [0x0C18BF68] has exactly 2 refs (both reads).
  GDB write-watch from reset proves both stay 0: BSS zero-fill, an
  init-table clear (0x0C0B92BA), and the pool-register itself writing 0.
- The pool-2 arena base is decoded image data, not peer data:
  [[0x0C0C64E0]] = [0x0C14F700] = 0xFFF86638 (on-chip RAM), written by
  the bootloader LZSS decoder before init runs (byte-wise watch hits),
  with valid .data neighbors. So this is NOT a Task 7/8 peer-data
  dependency (Exit B out): the base is present, but never arrives.
- Root cause (GDB single-step 0x0C0C64B8->0x0C0C64CE): QEMU's sh4
  target applies SH-4 register banking to `-cpu sh2a`, but the SH-2A
  SR has no MD/RB/BL bits (SH-2A Software Manual §2.2.2; reset sets
  only IMASK, reserved bits read 0) and its register banks work
  differently (interrupt auto-save + RESBANK, no active-bank bit).
  QEMU resets SR to MD|RB|BL|IMASK (0x700000F0, verified live at
  0x0C0C64B0 entry: sr=0x700000F1), so the whole boot runs on bank 1
  self-consistently until 0x0C0C64B0 loads the pool-2 base into r4 and
  executes `ldc` of a from-zero SR (0xF0, RB=0): the tail-call then
  reads stale bank-0 r4=0/r5=4 and registers pool-2 base 0. On silicon
  there is no bank switch, so the sequence is harmless and pool-2
  registers 0xFFF86638. The RPOKE ramp is retired (known-bad, Task 6b).
- Fix (board.c `cdj850_reset`, same pattern as the SH7269 GUI board's
  `env->sr = (1u << SR_MD)`): clear the SH-4-only RB/BL bits at reset,
  keeping MD (translator privilege) + IMASK (manual). No translator
  change, no SH-4 path touched.

Boot evidence (no bridge, EB rebuild): pool-2 head [0x0CE4A330] =
0xFFF866CC (live OCRAM free list); the old wall is gone; unimp goes
18w+0r -> 30w+8r, all legitimate driver-init touches (DMAC DAR/CHCR,
DMARS0/1, CPG STBCR2, CPG/PFC writes). The 9th bridged read
(0xFFFC750C) is confirmed bridge-degenerate (table-bytes-as-code in
6b) and does not recur. Boot proceeds through RTOS init, both mailbox
channels (0x90102), INTC IPR programming, and a CMT restart at 1560 ms.

Next park (NOT this task; brief files are board.c/xc only): first task
entry faults on the unimplemented SH-2A word 0x0068 at 0x0C08EFCA
(`extu.b/tst/mov.l` dispatch in 0x0C08EFC4, called from 0x0C08F294):
EXPEVT=0x180 illegal-insn -> firmware default vectors -> flash halt
`bra self` at 0x000002F0 (task stack, IMASK=0, RB=0). 0x0068 is absent
from notes/cdj850-sh-insns.md; identifying/implementing it is
translator-patch work (same class as commit 444dd14). USB enum not
reached (no USB activity yet).

Follow-ups: (a) `do_interrupt` still sets SR.RB on delivered
interrupts (helper.c:293) — two CMT ticks delivered harmlessly so far
(ISR spills/restores, rte returns to RB=0), but a translator-level
SH-2A banking disable (reset + ldc mask + gbank, gated on
SH_FEATURE_SH2A like the existing MD-force) would remove the hazard
class; (b) in_asm prints `mov r15,r5` for bytes 0x65F3 at 0x0C0C64C0
(standard decode: mov r3,r5) — display-only nit, non-load-bearing
(r5=4 comes from stale bank-0 +4 either way).

## Task 6b: wild-RTS forensics — Exit B, NOT a wild RTS (2026-10-13)

Reproduced identically (bridge+RPOKE, 20 s): 30w+9r, pc=0x000002F8
(`bra self` + nop), CMT vector 188 pending, IMASK=14. Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-6b-report.md. Headline
corrections to the park paragraph above (signed off via live-xp GDB +
`-d exec,in_asm`, in_asm authoritative, scripts in /tmp t6b_*.py):

- The `rts` at 0x0C0B91D6 fires TWICE. First (boot, pr=0x0C000008,
  sp=0xFFF90000) returns CORRECTLY into the MAIN init-call chain at
  0x0C000000 (`mov.l @PC,Rn; jsr/n @Rn` trampolines, SH-2A JSR/N=0x4n4B;
  literals 0x0C0B92AC/8FF4/951C; ends `bra self` at 0x0C00000C) and runs
  the 0x0C0B951C->0x0C0C64B0 cascade. The port function itself
  (0x0C0B91C0-D8, port polls + RAM stores, delay-slot store) never
  writes pr: it is clean in both passes.
- The fault is the SECOND pass: the 0x0C0C68xx driver epilogue
  (FPU restore 0x0C0C6806, GBR/MACH/MACL restore + `rte` 0x0C0C682C)
  resumes a task context with pc=0x0C0B931C (boot-time code area, by
  then overwritten with runtime descriptor tables referencing
  RPOKE-scratch 0x0C400AAF/AB0/1B0 + pool arena 0x0CE4A30C),
  pr=0x0C0C78D8 (more table-data), sp=0x0C400A53 (inside RPOKE
  scratch). Pre-history in the same exec flow: allocator 0x0C0D01xx
  (brackets the RPOKE site 0x0C0D0122, 3 alloc iterations at 0x0C0D01C4).
- The resumed thread runs descriptor bytes: a 2-insn oscillation at
  0x0C0B931C/1E (~86k TB execs, ZERO stub/MMIO accesses in the span —
  same-run exec+TRACE correlation) with `sts.l pr,@-r15` descending
  from scratch, then falls into the port driver (works: r6=0x1000 or
  0x0 across runs, same outcome), `rts` to 0x0C0C78D8, executes table
  bytes (`mov #-93,r4`; BSC read 0xFFFC750C=0; `shlr r0` -> 0x7FFF1D43;
  `bsr 0x0C0C78FE`, pr=0x0C0C78E6) and dies on the delay-slot word
  0x0C68 (an SH-2A form both disassemblers print as `.word`; no handler
  TB executes) -> flash halt 0x000002F8. [VBR+0x100]=0x0C0C68FC (never
  runs at fault), [VBR+0xA00]=0.
- Stub-independence (why no board.c change): (a) zero stub MMIO in the
  86k span; (b) the faulting indexed access (base 0x7FFF1D43 + 16-bit
  index) can never reach a valid mapping for ANY index value;
  (c) r6 varies run to run (0x0/0x1000), outcome identical;
  (d) mailbox-not-ready would park EARLIER (validation 0x0C075ECA).
  The DMA-driver-text hypothesis is out too (CHCR=0x1810 DE=0, path
  never touches 0x0C1C14E0).
- Verdict: Exit B. The resumed context is a creature of the RPOKE
  bridge (built by allocator flow through the ramp site, stack inside
  ramped scratch) and/or of peer-starved tables (mailbox echo consumed
  as DSP data). Owned elsewhere: genuine pool-2 init to replace the
  ramp (Task 6 remainder — the [0x0CE4A330] WHY is still open), and/or
  Task 8's DSP/panel upload populating these tables. Do NOT paper over
  with stub fabrications. No commit (Step 4: no code changed); suite
  untouched (baseline).

## Task 6e increment 1: panel-handshake ready pin 0xFFFE391E + xc split (2026-10-13)

Step 1 correction first: Task 6d's "park polling repeated 0x01c40 reads"
was imprecise. The 0x01c40 reads are USB INTSTS0 (offset 0x40 of the
0xFFFC1C00 block), BOUNDED (12x, then boot moves on to the 0x2801x block).
The true infinite park is a SILENT port poll at 0x0C08E8F2, invisible in
the unimp log because the port block is modelled storage.

- Full device address: r0 = 0xFFFE391E (movi20 #0xE391E sign-extends),
  r11 = 0; `mov.w @(r0,r11),r6` reads the word, waits for bit 2
  (extu.w; shar; shar; bld #0,r2; bf loop). Live QMP: pc stable at
  0x0C08E8F2/0x0C08E8F4 over 3 samples/6 s, r0 = 0xFFFE391E, r11 = 0.
  CDJ850_TRACE: `port read +0x11e size 2 = 0x0 at pc=0x0c08e8f2` from
  1587 ms on; +0x11E never written (only +0x110..+0x116 pin-function
  neighbours), so stored zero never releases it.
- Expected pattern: bit 2 = 1 (peer ready/idle). Driver 0x0C08E8AE
  (device id 1; sibling 0x0C08E842 for id 0 never runs this boot):
  int-disable, 0xAA/0x55 probe patterns to void 0x20000xxx scratch,
  SR restore, delay, ACK-wait, outcome to SDRAM 0x0CEE34AA + flag.
  Called from the mode-2 branch of the boot dispatcher (state
  [[0x0C08EC28]] == 2).
- Hardware: panel-handshake GPIO. The 0xFFFC1C00 block is the on-chip
  R8A66597-family USB (offsets match hw/cdj/common/usb_r8a66597.h
  exactly: SYSCFG 0x00/SYSSTS0 0x04/DVSTCTR0 0x08/INTENB0 0x30/INTENB1
  0x32/BRDYENB 0x36/NRDYENB 0x38/BEMPENB 0x3A/INTSTS0 0x40/PIPESEL
  0x64/PIPECFG 0x68/PIPEBUF 0x6A/PIPEMAXP 0x6C/PIPECTR5 0x78; INTENB1
  0x4030 arms BCHG + control-transfer interrupts; firmware strings
  usbh_msc/usbTask/USB-A-host + USB-B-function confirm both stacks).
  The service manual's signal block diagram wires MAIN (IC103) to the
  panel CPU (IC1007) over TSI/TSO/TSCK serial plus PNL_XBUSY/MAIN_XBUSY
  handshake GPIOs (connector CN2003) — the ACK-wait shape fits, with
  USB_CDETFLG/VBUSON and DSP-READY as noted alternates. Exact SH7263 pin
  name for +0x11E unchecked (no hardware manual on file).
- Model (xc/port.c): reads of +0x11F (low byte of the +0x11E word;
  self-caught off-by-one — strapping +0x11E reads back 0x0400, which the
  shar-shar-bld sequence still sees as 0) OR in bit 2, storage otherwise.
  The level is what the driver's own comparison dictates (same always-
  ready rationale as the mbox stub); no data bytes invented; the panel
  protocol stays Task 7's.
- File-split rule applied: port/ser/mbox move to xc/ (xc/port.c,
  xc/ser.c, xc/mbox.c + headers, shared xc/trace.h); board.c keeps
  intc/ccr/ccn-audit; meson updated. Verbatim moves except the pin bit.
- Evidence: rebuild warning-free (only the pre-existing ld notice);
  boot passes the handshake (`port read +0x11e = 0x4` exit at 1482 ms),
  runs ser-upload (1143 TX bytes via 0xFFFE7000), mbox 0x90102 rounds,
  INTC IPR programming, CMT restart — unimp counts unchanged 82w+46r
  (no new unmodelled touches). Suite 659 passed / 38 skipped / 1 xfailed.

Next wall (NOT a fault, NOT a translator gap — a silent peer-event
park, this task BLOCKED here): outer loop 0x0C0B8EAE spins while the
scan 0x0C0B8ED8 returns mask 0 over 17 pool-2 object slots (bytes
0x0CE49CBC = 00 00 03..0E 00 00 00, words 0x01010101.., ptrs to OCRAM
TCBs; release needs [*ptr+20] to differ from word for a claimed entry).
Hard park (60 s run never leaves; pc histogram only scan + CMT-tick
ISR 0x0C0C68FC). -d exec proves the RTOS is alive: usbTask creator
(0x0C10B34A), usbh_msc (0x0C01DAFC+), servoTask entry (0x0C0FF8C8, name
confirmed in SDRAM strings), ser TX driver, tick ISR all execute, then
everyone blocks on peer/device events that never arrive (no panel/DSP/
USB peers; our ser/mbox stubs raise no RX/IRQ). What unblocks it needs
a Task 7/8 call: panel-serial RX bytes? DSP/mbox reply IRQ? servo
"no disc"? SH7263 manual (INTC IPR +0x40E source at 1477 ms, USB
vectors) required before any IRQ wiring — do not guess vectors.

## Task 8a: DSP/servo signaler hunt — BLOCKED (stop rule; 2026-10-13)

> SUPERSEDED 2026-11-16: see Corrections at top (DSPDREQ is PE11; Port E is servo).

Wall reproduced exactly (no bridge, EB rebuild of 07df07e): outer
0x0C0B8EAE spins, scan 0x0C0B8ED8 returns mask 0 over 17 slots
(claim bytes 0x0CE49CBC = 00 02..0E 00 00, words 0x0CE49C74 =
01010101..11111111, ptrs 0x0CE49F58: slots 0/1/16 null, 2-15 OCRAM
TCBs). Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8-report.md.

- Scan protocol decoded (authoritative `-d in_asm`; the 32-bit forms
  0x32E1_8000 = MOVU.B @(0,r14),r2 = claim-byte load and 0x8728-class
  BLD display as `.word`, semantics execute correctly): per idx,
  r3 = ptr[idx+1], r2 = [r3+20] (signal ptr), r4 = [r2] (signal
  value), claim[idx] must equal idx+1, and a mask bit sets only when
  [[ptr+20]] DIFFERS from word[idx]. Release needs ANY single bit
  (outer `tst r0,r0; bt.s re-scan`); post-release stores mask to
  [0x0CE49CD0] and calls the dispatcher (0x0C108xxx), else re-scans.
- `[[ptr+20]]` writer watch (CDJ_MWATCH, all 14 signal cells across 4
  boots + mask cell): the SOLE writer is the RTOS creator at
  0x0C0C7490/pr 0x0C0C7438 (init-fill 0x0X0X0X0X); NO in-guest task
  or ISR ever signals. The 13 claimed objects are a task-init
  barrier gated purely on peer events.
- "mailbox" 0xFFFFC000/0xFFFFC800 identified as SH7263 SSI0/SSI1
  audio (hardware manual Table 18.2: SSICR_0 0xFFFFC000,
  SSISR_0 0xFFFFC004, SSITDR_0 0xFFFFC008, SSIRDR_0 0xFFFFC00C; ch1
  at 0xFFFFC800): init writes 0/0x90102 complete under the stub,
  SSI0 IRQ pri 7 via IPR14 (0x0C075EE0), SSI1/3 + ROM-DEC IRQs via
  IPR15; NO post-init SSI status/data/IRQ wait exists (14 accesses
  at 1474-1475 ms, then silence incl. SSITDR DMA DAR with DE=0).
  NOT the signaler. (Model rename to ssi.c deferred; behavior is
  SSI-plausible today.)
- E-6002 anchor INVALID: the string (and "6002") is absent from
  C850MAIN.UPD, main_flat.bin, and live 32 MB SDRAM (exhaustive
  search); error display uses "SYSTEM ERROR : %s %d" numerics. The
  DSP-fail path cannot be xref'd; DSP program bytes not located.
- DSP upload driver: Port-E GPIO bit-bang (transfer 0x0C0FC9xx,
  gates 0x0C0FCA7A/0x0C0FCBB0 waiting PE6+PE8, bring-up
  0x0C0FD7CC-8D0 asserting PEDRL bits 6,8,7 at 1530 ms, PEIORL
  phases 0xF1C0/0x1C0, reply sampling PE9/PE12/PE14 into bytes at a
  RAM buffer). PE12 = DSPDREQ (service-manual schematic: IC103 pin
  72 = DSPDREQ; DSPC56371AF180 HREQ pin 38; DSPRST-H/SRVRST-H
  release, DRVMUTE-L = no disc). Driver state + byte buffer FROZEN
  across a 20 s live sample while port accesses cycle: retry-stuck
  on DSP reply acceptance, NOT slow.
- Release experiments (local-only port harness, REMOVED before
  commit; delivery verified in TRACE: PFPRL reads 0xA000, PBPRL
  0x18, PFPRH 0x01, PEDRL 0x1680/0x5680 with strapped bits; release
  detector = MWATCH on [0x0CE49CD0]): pe12/pe9/pe10/pb3/pb4/
  pf13+pf15/pf16/pf25/all-combined — NO release in any run.
  Static READY levels do not satisfy the DSP reply validation;
  supplying reply bytes would be fabrication. Stop rule applies.
- Servo: PBPRL-bit3 poll (servoTask sub 0x0C0FF7D4) self-exits via
  a 46-spin timeout; pb3 strap (verified 0x18) gives no release;
  NO-DISC strings + DRVMUTE-L honest no-disc state already hold.
  NOT the signaler (no "no disc" answer needed).
- Release machinery PROVEN genuine-needing: PPOKE of one signal
  cell sets the mask, dispatcher runs, then error-halt at
  0x0C0B8ED4 (degenerate task state, 6b-class); GDB full-13-cell
  poke runs the dispatcher repeatedly then halts with IMASK=15
  (tick starved). Flags alone are insufficient — the 13 tasks must
  REALLY execute.
- Peer-event inventory for follow-ups (all sustained spins, no
  fabrication): PB4 (0x0C0062CE), PF13/PF15 = SD_D1/SD_CD
  (0x0C0D69B2/C0), PF16/FRB = flash-ready (0x0C0057E4), PE10
  (0x0C10DCEA, usb region), PF25/SSIWS2 (0x0C106074), panel
  accept-path stores 0x0C7F663C/0x0C7F65BC NEVER written (MWATCH;
  Task 7 loose end (a), Task 9 scope), DSP reply-byte validation.
  IPR arming decoded: IPR13 = SCIF0/1, IPR14 = SSI0 (pri 7),
  IPR15 = SSI1/3 + ROM-DEC; only the CMT tick is delivered.
- Verdict: 8a BLOCKED per its stop rule (neither DSP nor servo
  releases it with driver-justified semantics). 8b/8c/8d stop
  honestly (no carve target, no SHA, no core, no audio hook).
  No code changed this slice (harness removed; tree == 07df07e).

## Task 8b: send-buffer + reply validation mapped, carve BLOCKED (2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8b-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `cfacebc`. Capstone
SH-2A + hand-decode (MOVU.B/BLD gaps vs `translate.c`); `-d in_asm`
spot-check only (driver TBs absent from the window — stated in
report). Live GDB RSP + `info registers` + `CDJ850_TRACE` + standing
32 MB SDRAM dumps.

- Send chain: transfer `0x0C0FC9DC` (5× loop, delay `0x0C107932`,
  helpers `0x0C0C688A/94`, sampler `0x0C0FC8FA`, `bld.b` reply test)
  → mirrors `0x0CEE34B0/84` → port `0xFFFE3A02/06` (`+0x202`,
  phases `0x1C0/0xF1C0` live) → send `0x0C0FCC68/0x0FCD0E`
  (`r4` byte, BSR-only callers) → callers `0x0C0FCAC0/0x0FCBEA`
  (`r12/r14/r11/r13` bytes) → gates `0x0C0FCA7A/0x0FCBB0`
  (wait PE6 then PE8) → bring-up `0x0C0FD7A8/0x0FD7CC`
  (`r4=0x0CE4B5DA`, `pr=0x0C0FC36A` live). Trace: 2–3 words
  (`0x5080/0xB080/0xD080`, 2650–2696 ms) then gate-spin at
  `0x0C0FCBB0` — full program never clocked out.
- Sequencer `0x0C0FDA2C–0x0FDC1E`: 36 hardcoded `(index,data)` via
  `0x0C0FCA5E` (delay-aware parse; 26 such sequencers repo-wide —
  AUDIO-vs-SERVO owner unknown). Control writes, not claimed as
  program. Candidate `0x0CE4B6F4–0x0CE4B810` (284 B, header +
  `07FF…`) has no reader refs and is absent from UPD/main/boot
  contiguously (packed MAIN, entropy 7.55) — carving it would be a
  guess. Refused. No SHA pinned, `notes/cdj850-layout.md` untouched,
  `MODEL_EXPECTED` untouched.
- Reply: sampler `0x0C0FC8FA` → byte `0x0C7F6D6C`; collector
  `0x0C0FC8DC` → 10 B `0x0C7F6D1C` (live zeros/`0x11`, frozen);
  validator `0x0C0FC71C–0x0FC87E` checks `0x0C7F6D6C`,
  `0x0C7F6D5C+2`, `0x0C7F6D67`, `0x0C7F6D02`, `0x0CE4B8A6`,
  `0x0C7F6AE9` — accept stores `1→0x0CE4B8F5` via `0x0C104D64`,
  reject calls `0x0C104E6A`, return `0x0C104EE2`. Never accepted
  live.
- Rename `mbox.c→ssi.c` deferred (forensics-only slice; tree-wide
  touch rides with the next code task). No carve script (would emit
  a guess). 8c inherits: unblock packed-MAIN→SDRAM + AUDIO/SERVO
  owner first; census starts at the 36-pair table + other 25
  sequencers + candidate block (unpinned).

## Task 8c: DSP bootstrap READY + recorded init image; reply BLOCKED (2026-10-13)

> SUPERSEDED 2026-11-16: see Corrections at top (DSP READY pins; Port E is servo).

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8c-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `97ae419`.
Commits: `7f29562` (mbox.c→ssi.c behavior-identical rename),
`dddf5f7` (dsp56371.c bootstrap READY + record). No subagents. No
bridge. No fabricated bytes/replies/vectors. Firmware read-only;
datasheets live in /tmp only (never committed).

- Datasheet fetch: DSP56371 UM Rev 2.1 (404 pp, operating modes §4.3,
  SHI-slave bootstrap modes 5/7, HREQ slave-ready semantics, App. A ROM
  source) + datasheet Rev 4.1 (HREQ/SPI-I2C timing §12/13) + DSP56300FM
  + SH7263 §29/30/34 (PEIORL 0xFFFE3A06, PEDRL, direction-aware reads).
  f70e4d §10 nets: DSPRST→IC301/56371 RESET_B pin47, SRVRST→IC201
  (Toshiba TC94A15FG — NOT a 56371), DSPDREQ←HREQ pin38 → MAIN PE12,
  MAIN PE7-11 ↔ DSP ESAI PE7-11, E-6002 = audio-DSP write fail
  (manual pp.26/34/39/43 — 8a's "absent from MAIN strings" stands, but
  the code is real in the manual/diag path).
- Bootstrap model (`dsp56371.c` at board level): DSPRST tracker
  (PEDRL 0→0x40→0x140→0x1C0, fires ~5-9 s) then READY (PE6/PE8/PE12)
  on Port-E reads; every MAIN PEDRL/PEIORL write recorded
  (`CDJ850_DSP_RECORD`, 20 B packed pc/ms/off/size/val). Reply DATA
  nibble stays loopback — no DSP payloads produced.
- Record: 519 genuine bytes, SHA-256 pinned in notes/cdj850-layout.md
  (identical across 3 boots), 259 (index,data) pairs + 0x00 pad, banks
  F0-F8/D7 — a control init image, NOT opcodes (census N/A, no core).
  Post-image (~9 s) the driver spins 42.8k× in the CD6C/CDDC sample
  loop (strobe 0x10 + sampler, PEDRL ∈ {0x1080,0x1180,0x11C0});
  collector zeros, sample byte 0x11, accept flag 0, mask 0 — the reply
  needs genuine DSP execution. BLOCKED precisely there; static-hunt
  roadmap (packed-MAIN→SDRAM decoder) recorded for follow-ups.

## Task 8d: program-source hunt — MOD=5 host-boot; decoder map; outer caller; carve BLOCKED (2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8d-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `0e01416`. No subagents.
No bridge. No fabricated bytes/replies/vectors. Firmware read-only;
datasheets live in /tmp only (never committed:
`/tmp/371UM.pdf` + `/tmp/DSP56371.pdf` + `/tmp/DSP56300FM.pdf` +
`/tmp/sh7263_hw.pdf`, re-fetched public Motorola docs).

- MODA–D strapping (f70e4d.pdf p.76, §10.4 MAIN ASSY 3/4, vector-level
  schematic read + `371UM.pdf` Table 4-2): R312→MODA (pin 51) and
  R314→MODC (pin 49) to the supply rail (M, y=256.8 H+V topology),
  R313→MODB (pin 50) and R315→MODD (pin 48) to GND (thick y=240 rail
  via CORE_GND_3/pin 53), all 10 k. Alternating 1-0-1-0 assignment is
  deliberate (a single GND plane would serve all-GND; two rails prove
  differential strapping) → MODD:MODC:MODB:MODA = 0:1:0:1 = Mode 5
  (Bootstrap from SHI, slave SPI; cf. mode 2 = 0010 PROM jump).
  Implication: the DSP host-boots from MAIN — a MAIN-resident program
  upload MUST exist; PROM-resident (no MAIN copy) is OUT. Residual:
  M-rail exact voltage (1.25 V vs 3.3 V) unconfirmed from PDF vectors
  alone, but digital-HIGH intent is unambiguous. R316 4.7 k
  PINIT_NMI→GND and R351 100 DSPRST→RESET_B noted in passing.
- Decoder map (`scripts/firmware/c850_unpack.py`, GPL + test): packed
  `main_flat` `0x20–0x19E70A` (BE32 `0x19E6E6` + LZSS ring
  4096/init `0x20`/pos 4078, whole-flat unpack for the bootloader's
  overrun) → SDRAM `0x0C000000–0x0C39231E` (3744542 B, SHA-256 pinned
  in notes/cdj850-layout.md); remainder `0x19E70A–0x1A0000` checksum +
  pad. Verified against live 32 MB SDRAM (code/rodata spot match;
  BSS/heap diverge from `0x1C14DC` as expected); driver bytes
  disassemble identically from the unpack. Bootloader table at
  `boot.bin` `0x4B0–0x52C` (flash/SDRAM/len words incl. staging
  `0x0DDEFA20`/`0x0DDEFC1C`) cited as corroboration, not load-bearing.
- Outer caller (in_asm hot-TB attempt + unpack xref + live RSP
  breakpoint — RSP authoritative): `0x0C0FCD6C–0x0C0FCE52` is one
  function (gate → strobe `0x0C0FCD0E` r4=`0x10` → sampler
  `0x0C0FC8FA` → assembly `0x0C0FCE14–38`); 0 BSR/BRA callers (register-
  indirect, as predicted). Two literal-pool call sites: servo
  `0x0C0FF7D4`-family at `0x0C0FF86A/86C` (via `[0x0C0FF9EC]`,
  comparison `0x0C0FF86E+`) and audio/player `0x0C103984`-family at
  `0x0C1039B2/B4` (via `[0x0C103A60]`, comparison `0x0C1039B6+`).
  Live break at `0x0C0FCD6C` (40 s) lands pr=`0x0C0FF86E` (servo
  caller hot, r3=`0x0C0FCD6C`). In_asm gap recorded honestly (driver
  TBs `0x0C0FC6xx+` absent from the translation window, as in 8b).
  AUDIO-vs-SERVO owner split (8b concern 1): SHARED routine, both
  tasks call it — resolved as shared, not owned.
- Carve: NONE (BLOCKED). 519-B init image and 284-B `0x0CE4B6F4`
  candidate both absent from packed AND unpacked MAIN (byte search);
  candidate offset `0xE4B6F4` lies beyond the `0x39231E` image
  (runtime heap). Mode 5 predicts a host program, but no contiguous
  blob exists in MAIN — the upload stream is assembled from scattered
  immediates/tables, or the SHI payload shape is still misread. Next
  task needs SHI-protocol analysis (UM App. A Bootstrap ROM is public)
  or a logic-analyzer capture; do NOT guess.

## Task 8e: SHI-framed reinterpretation + header search + unpacker-PC trace — carve BLOCKED with new evidence (2026-10-13)

> SUPERSEDED 2026-11-16: see Corrections at top (DSP program is in MAIN image).

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8e-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `694cc05`. No subagents.
No bridge. No fabricated bytes/replies/vectors. Firmware read-only;
datasheets in /tmp only (never committed: `/tmp/371UM.pdf` UM Rev 2.1
404 pp + `/tmp/DSP56300FM.pdf` FM Rev 5 512 pp, re-fetched public
Motorola docs); derived bytes only in /tmp + untracked `extract/`.

- UM App. A sequence extracted verbatim: SHI slave-SPI bootstrap
  (HCSR `$A9`: HEN=1, HM=10, HFIFO=1, HMST=0, HRQE=01) takes 3 B
  count + 3 B start address + 3N B program words into contiguous
  PRAM, then `jmp (r1)` to the start address; 24-bit words (Table
  7-3), MSB-first (§7.4), HREQ asserted when IOSR ready + HRX FIFO
  not full, deasserted at first clock of next word (§7.7.1); PRAM 4K
  `$000000–$000FFF` (Fig. 3-1); opcode markers FM ch. 13
  (NOP `$000000`, RTS `$00000C`, REP `$06+Ax`, BRA `$05`,
  JMP `$0A/$0C/$0D`).
- 519-B re-parse (`scripts/firmware/c850_shi_search.py` + 4 tests,
  commit `68e80fa`): 173 words, offset-0 count `0x8BA08C`/`0x8CA08B`
  (need 171); zero exact-fit offsets in either order; 16-bit /
  data-only / index-only framings misfit; payload census both
  orders barren (0 NOP/RTS/REP/BRA; 2 isolated LE JMP singletons in
  head control bytes; `00Fxxx` words spell the F-bank walk).
- Header scan: unpacked 4816 structural / 253 vector survivors,
  packed 88 / 1 — all classes explained (SH `nop`/load opcodes as
  fake counts; survivors disassemble as valid SH-2A incl. a MAC-loop
  driver at `0x15EB0` and movi20 code at `0x1D1BC`; BSS hits are SH
  tables; packed hit is LZSS-stream coincidence); no survivor meets
  the genuine-program conjunction (pinned in test). Negative redo:
  full-length `bytes.find` — 519 B and 284-B candidate
  (`d47e1d71…c125d9fa4`) absent from packed AND unpacked.
- Unpacker-PC trace (fresh 12 s early `-d in_asm`, 7923 TBs):
  entry `0x0DDEFA20`, bit head `0x0DDEFA52`, literal store
  `0x0DDEFA78/7A`, match loop `0x0DDEFAA6`, exit `0x0DDEFAC2`,
  caller `0x0DDEFBE4`, then `0x0C000000`. Side find: 8d's own
  in_asm log already contained `0xddefa78` — its case-sensitive
  `0DDEFA78` grep missed it (corrected, no blame).
- Carve: NONE (BLOCKED). New evidence for the mode-5-vs-PROM
  re-examination: the 519 B is phase-1 control fill with no SHI
  framing; the program upload (if any) sits behind the
  never-completing reply handshake or is assembled purely from
  scattered immediates. Suite: full `tests/` 665 passed, 38
  skipped, 1 xfailed (2026-10-13).

## Task 8g: validator-driven reply brute force — DONE-BLOCKED (honest stop; 2026-10-13)

Knob: `CDJ850_DSP_REPLY=0xHL` (H,L in 0-7; commit `b3d166e`;
`dsp56371.[ch]` + `xc/port.c` pc threading; default unset = OFF =
loopback-identical; suite `test_sh2a_smoke+test_firmware_cdj850`
13 passed). Drives PE15:13 at the sampler's two phase reads
(`0x0C0FC920`/`0x0C0FC95C`); PE12 READY untouched; assembled sample
byte `((H<<1|1)<<4)|(L<<1|1)`.

- Validator map (Capstone SH-2A BE + hand pools, all quoted in the
  8g report): entry needs `[0x0C7F6D4F]==1` (`0x0C0FC728`); merge
  (`0x0C0FC7B8-7EA`, helpers `0x0C107D9C` assemble / `0x0C107D6A`
  split) folds sample-hi into flags `0x0C7F6D5C..63`; needs
  `[0x0C7F6D5E]==1` (`0x0C0FC7F4`), `[0x0C7F6D67]==1`
  (`0x0C0FC832`) for the accept leg (collector `0x0C0FC8DC` 10
  samples → `0x0C7F6D1C`, `0x0C104D64` low-nibble {0,1,5}+nonzero
  checks, `1→[0x0CE4B8F5]`); tail needs `[0x0C7F6D02]!=0`,
  `[0x0CE4B8A6]!=0`, `[0x0C7F6AE9]==0`; outer caller `0x0C1050EC`
  re-gates on `[0x0C7F6CF6]==7` + `[0x0CE4B8EB]!=0` → `0x0C0FF5FC`.
- Static sweep (8 boots, H==L, 40 s each): samples land exactly
  `0x11…0xFF` as predicted; flags = sample-hi bits (exact);
  `[0x0C7F6D5E]=1` for H in {2,3,6,7}. ACCEPT/MASK/USB all zero;
  no error halt (all stop PCs known park/task PCs).
- Depth (RSP Z0 + manual step-over — plain `c` re-hits, counts
  would lie): accept chain TRUE-zero across 4 boots (validator,
  collector, `0x0C0FD3C0`, `0x0C0FD360`, dispatch setters
  `0x0C10545C/51C`, `0x0C10099C`, `[A6]=1` writer `0x0C104E30`,
  outer `0x0C1050EC`, common-return `0x0C104EE2`, clearer/checker
  except checker). Hot loop = reply-spin → `0x0C0FC704` gate
  (always rts) + dispatch `0x0C100A98` (checker, `[CF6]<12` gate)
  + `0x0C104F1A` (reject tail) + `0x0C0FC24C` via `0x0C0FC6F4`
  trampoline (PR-proved `pr=0x0C0FCD84`, not the accept path —
  correlation≠causation, self-caught).
- Wall: `[4F]` setters (`0x0C107776` family, 11 dispatch slots)
  never run; `[67]` has no hot writer (only the cold
  validator-internal SPLIT to `0x0C7F6D64`); flags bits 4-7 are
  structurally unreachable (sample contributes bits 0-3 only,
  ASM preserves); lo-nibble is unconsumed by every hot reader ⇒
  static-8 already exhausts the single-decisive-byte bound.
  Stopping per honesty gate; hardware capture (reply
  timing/sequence + DSPDREQ pulse behavior) replaces the knob.

## Task 8h: PE12-pulse-at-sampler micro-experiment — NOTHING (honest negative; 2026-10-14)

> SUPERSEDED 2026-11-16: see Corrections at top (PE12 is SRVBUS0, not DSPDREQ).

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8h-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb` (rebuilt twice; warning-free
apart from the pre-existing `ld: -undefined error` notice), BASE
`e2f751c`. No subagents. No bridge. Firmware read-only; run/probe
scripts in /tmp only (uncommitted); no new extract files.

- Basis, diff-quoted (8g reviewer finding): DSP56371 UM Rev 2.1
  Table 2-7 HREQ ("When configured for the slave mode, HREQ is
  asserted to indicate that the SHI is ready for the next data word
  transfer and deasserted at the first clock pulse of the new data
  word transfer.") + UM §7.7.1 "SPI Slave Mode" (p. 7-19: "HREQ is
  deasserted at the first clock pulse of the next data word
  transfer.") + f70e4d.pdf §10.14 p. 91 panel 5 (IC103 pin 72
  DSPDREQ, 1.0 V/div, 20 us/div: trace idles HIGH with a brief LOW
  pulse per transaction). Same quotes live in `dsp56371.c`.
- Knob: `CDJ850_DSP_PULSE=1` (default OFF; composes with
  `CDJ850_DSP_REPLY`): PE12 LOW only at the sampler phase reads,
  HIGH elsewhere; PE6/PE8 never cleared. Self-caught off-by-two:
  phase 1's executing load is the `mov.w @(r0,r11),r5` at
  `0x0C0FC922`, not the `mov r14,r0` setup at `0x0C0FC920` named in
  8g (first pulse-only boot assembled 0x10 = ph1 nibble 1 = PE12
  HIGH at the true read, exposing the miss); 8g reply behavior is
  byte-identical before/after (its default-H branch already covered
  the true ph1 read).
- Live proof: RSP reads at stopped sampler PCs show PE12=0/PE8=1
  (both phases); hot gate `0x0C0FC704` + `0x0C0FC24C` show
  PE6=PE8=PE12=1 (no leak; gates keep passing). (Validator-body
  gate `0x0C0FC736` + xfer gate `0x0C0FCBB0` are cold — 60 s
  no-hit, consistent with 8g depth-zero, not a hang.)
- 2 verdict boots (40 s): pulse-only → sample 0x00, flags all 0;
  pulse+0x77 → sample 0xEE (predicted ((7<<1)<<4)|(7<<1)),
  flags 00 01 01 01, `[5E]=1` (same genuine progress as 8g k77, not
  movement). Both: `[4F]/[67]/[02]/[A6]/[AE9]` + ACCEPT
  `[0x0CE4B8F5]` + MASK `[0x0CE49CD0]` + collector `[0x0C7F6D1C]`
  all zero; stop PCs known park PCs (`0x0C0B8F04`, `0x0C0C682C`).
- Verdict NOTHING: no accept/mask/collector/dispatch change vs 8g
  baseline. Static + temporal-local space now both exhausted; the
  only remaining path is hardware capture (per-strobe PE12-15 +
  strobe clocks from DSPRST release, §10.14-style).
- Suite: `test_sh2a_smoke + test_firmware_cdj850` 13 passed.

## Task 8j: scripted validator-ordered reply sequences — NOTHING (honest negative; 2026-10-14)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8j-report.md.
Branch `cdj850-bringup`, BASE `9025648` (actual HEAD; brief named
`03becd1`, superseded by the VFD-1 commit). EB
`~/qemu-build-eb/qemu-system-sh4eb` (rebuilt once; warning-free
apart from the pre-existing `ld: -undefined error` notice). No
subagents. No bridge. Firmware read-only; run/probe scripts in
/tmp only (uncommitted); no new extract files. Marathon boot
untouched (shared CPU only).

- Hypothesis (untried dynamic dimension): all prior driving was
  STATIC; the validator collects 10 bytes in ORDER, so a scripted
  per-strobe sequence from validator constants + echo-of-last-sent
  (a DSP ACKing uploads is the most common real protocol) was
  still driver-justified.
- Knob: `CDJ850_DSP_SEQ=order|echo|combo` (default OFF; 8g/8h
  static modes intact): per-strobe PE15:13 pattern advancing one
  step per sampler phase read (`0x0C0FC922`/`0x0C0FC95C`);
  overrides static REPLY at sampler reads only; composes with
  PULSE (sequencer never touches PE12/PE6/PE8). order = 20-nibble
  cycle (bytes 51 11 55*6 55 71: B0 lo=1 in {0,1,5} + [5E]-pass
  hi + inside all range bounds); echo = 3-bit windows of the
  last MAIN PEDRL upload byte (identity low-bits + windows);
  combo = ph1 order nibble + ph2 echo window. Branch-polarity
  correction vs 8g: the `[02]==0` leg needs byte6==0 (not !=0) —
  unreachable under PE12=1 (min byte 0x11); the `[02]!=0` leg
  needs only byte0-lo in {0,1,5} (B0=0x51 satisfies it).
- Live proof (probes, not verdict boots): order — consecutive PH2
  hits show PE15:13 0 then 2 (pattern advances per strobe; probe
  m-reads/steps consume elements, noted); echo — 12 s trace:
  4362/4362 PH2 + 4362/4362 PH1 reads carry exact 3-bit windows
  of the latest PEDRL upload byte.
- 3 verdict boots (40 s): order → sample 0x55, flags 01 00 01 00,
  `[5E]=1` (same genuine progress as 8g k22/k33, not movement);
  echo → sample 0x11, flags 01 00 00 00 (== control; echoing the
  spin's own strobes reproduces the latch); combo → sample 0x51,
  flags 01 00 01 00, `[5E]=1`. All three: `[4F]/[67]/[02]/[A6]/
  [AE9]` + ACCEPT + MASK + collector + `[54]/[EB]/[652]/[CF6]/
  [BC]/[AE8]/[882]/[D3]/[AF]/[89B]/[8C7]/[8FC]` all zero; stop
  PCs park (`0x0C0B8F04` x2) / panel-driver range (`0x0C077900`).
- Verdict NOTHING: no accept/mask/collector/dispatch change vs
  8g/8h baselines. Sequences join statics as exhausted; only
  remaining path is hardware capture (per-strobe PE12-15 +
  strobe clocks from DSPRST release, §10.14-style).
- Suite: `test_sh2a_smoke + test_firmware_cdj850` 15 passed.

## Task 8i: SPI-bitstream re-decode — NOT-SPI (offline; 2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8i-report.md.
Branch `cdj850-bringup`, BASE `03becd1`. No subagents. No board-code
changes, no rebuilds, no boots (offline analysis of the existing
`/tmp/8c-record20.bin`; re-record not needed — files present).
Firmware read-only; no new extract files.

- Clock/data assignment (unpacked MAIN, SH-2A BE; record PCs +2 past
  delay-slot writes): PE6 reads (`0x0C0FC73E` + `and #64`, siblings
  `CA86/BB0/CD8C/CE5E`), PE8 reads (`0x0C0FC752` + `and r4=0x0100`
  via `shll8` at `0x0C0FC74C`), sampler nibble (ph1 `0x0C0FC922` +
  `and 0xF000` at `0x0C0FC92E` + `shad #-12`; ph2 `0x0C0FC95C`).
  Exhaustive scan: only #64/0x0100/0xF000 touch Port-E;
  `bset/bclr` only #6; no PE9/PE12/PE14 isolate exists. Delays via
  `0x0C107932` (r4 = count) are nibble-paced; PEIORL
  `0x0C0FCDEA=0xF1C0`/`0x0C0FCDF0=0x01C0` toggles the whole nibble
  (parallel GPIO, not SPI).
- Bitstream (PE8 0->1 rises; PE12-15 serially + nibble-parallel both
  MSB orders; MSB-first): full 25030 rises; upload 5174 PEDRL / 1600
  rises / 375 PE6 rises. Serial upload 200/129/46 B; nibble 800/519 B.
- Framing (both orders, exact-fit every offset): all `fits == []`;
  census 0-4 singletons (noise). Nibble byte-only 15-12 reproduces
  the 519-B image byte-identically (SHA `7132eb44…3972064`) — the
  bitstream collapses to the rejected control image. Full-record
  nibble 12515 B is 93.6% `0x11` idle (reply-spin); head strobe
  `0x55` + burst, tail all `0x11`. Characterization: periodic /
  control-patterned, not program words. Nothing carved; core blocked.
- Tool: `scripts/firmware/c850_spi_decode.py` (reuses
  `c850_shi_search`, GPL-2.0-or-later + SPDX) + tests
  `test_c850_spi_synthetic` / `test_c850_spi_record_pins` (11 passed
  in `test_firmware_cdj850.py`; 63 passed smoke + firmware subset).

## Task VFD-1: MAIN display-command format mapped; vfd.c decodes frames (2026-10-14)

> SUPERSEDED 2026-11-16: see Corrections at top (SSU0 is DSP link, not panel).

Verdict: panel-side-opaque (MAIN sends control indices only; the panel
CPU renders all glass text from its own ROM). vfd.c passes the exchange
frame bytes through as hex, synced on TX[0].bit7. No firmware bytes in
tests (synthetic only). Firmware read-only (C850MAIN.UPD); image via
scripts/firmware/c850_unpack.py (SDRAM base 0x0C000000, BE SH-2A).

Method: correct SH-2A disassembler (movi20/s imm20 = (op[7:4]<<16)|w2,
sign-extended from bit 19, S-variant <<8; mov32-disp12 0x3nm1 + sub nibble;
bitop32 0x3n09; 16-bit 0x86xx/0x87xx BCLR/BSET/BST/BLD; rts/n 0x006B;
nott 0x0068; SHAD 0x4nmC -- all confirmed against
qemu-src/target/sh4/translate.c + the live 0xFFFE391E load at
0x0C08E8EE), then BE32 pool sweep + BSR sweep + live TX/RX trace
(25 s parked boot, /tmp/vfd1probe.main.log).

### Builders (PCs + evidence)

- Panel exchange wrapper 0x0C077570 (6-byte full duplex: TX staging
  0x0C7F671B, RX 0x0C7F6721; inner TX pc 0x0C077808, single RX reader
  pc 0x0C077886; SSU MMIO via movi20: 0xFFFE7000/01/02/03/04/06/07/0A).
  Exactly 3 callers (BE32 pools holding 0x0C077570): cddabuff
  (0x0C006108, tail-call 0x0C005F36 / 0x0C005F4C), DjControl (0x0C014EE8,
  jsr 0x0C014E14), DataIn (0x0C0159C8, jsr 0x0C0158A6). No BSR/BRA
  callers; no other BE32 refs into 0x0C077000-0x0C079000 outside the
  driver cluster, boot/init, and these three tasks (full-image sweep).
- Shared TX-prep 0x0C005D54 (called by all three sites): TX[0] = mode
  byte [0x0C7F66C8] ORed with state bits -- bset #7 (0x0C005D78,
  unconditional), bset/bclr #6 (0x0C005D94/96), bst #5 from
  [0x0C7F672A] (0x0C005DA0), bset/bclr #4 (0x0C005DF0/F2), bset/bclr #1
  (0x0C005E4C/4E); clears display flags [0x0CE6D12E/12F]; bit-tests
  the 0x3A4-stride object table (base 0x0CE59B50, bld.b #5/#6
  @(0x3A1,r2)).
- cddabuff TX builder 0x0C005F04: TX[0]=r5, TX[1]=r0, TX[2]=r14,
  TX[3]=(r13&r4)>>16, TX[4]=(r13&r7)>>8, TX[5]=r13 (24-bit value split).
  Second variant 0x0C005F3A masks TX[0] with ~13 before re-exchange.
- 16-bit command driver 0x0C07800A (word param, stack+10): HI byte to
  SSTDR0 (pc 0x0C078236), LO byte to SSTDR1 (pc 0x0C07824E); TEND-polled.
  Word feeder: [0x0C7F6728] = 0x1310 if object-table +0x3A1 bit4 set
  else 0x1300 (builder 0x0C006386-9E); bit 8 of the shadow word at
  [0x0CE634B6] set/cleared by 0x0C006294.
- Boot upload 0x0C077B06/0x0C077C6C (TE-only): 1140 binary panel-init
  bytes, zero ASCII runs, none of MAIN's message strings present.
- DisplayTask entry 0x0C061454 (task-table ref 0x0C154494, name
  "DisplayTask" 0x0C1544A8): 10-sub-handler poll loop. Display RAM:
  0x0C9D1xxx frame buffers (cleared by 0x0C069FAE, 100 B/0x100 B),
  flags 0x0CE6D1xx. Message tables: 16 B entries 0x0C1544C8-0x0C15472F
  (EJECT/NO DISC/LOAD IN/...), browser/FX text 0x0C154738-0x0C154908
  (Track/ROOT/TRANS/HOLD/WAH/JET/ROLL/PC MODE), utility/charset
  0x0C154918-0x0C154A57 (charset 0x0C154A12: 03 ABC..789!?&@.. 02 +
  0123456789ABCDEF; refs from 0x0C067xxx utility code + 0x0C0E8A60).
  10-byte candidate 0x0C154A88 (77 12 6B 5B 1E 5D 7D 13 7F 5F) has ZERO
  BE32 refs: unreferenced data, NOT claimed as a segment map.

### Display-vs-control split (proven)

- DisplayTask module (0x0C061xxx-0x0C071xxx) holds no SSU driver
  pointers (full-image BE32 sweep: all 0x0C077xxx-0x0C078xxx refs land
  in cddabuff/DjControl/DataIn/boot/init pools) and no movi20 to
  0xFFFE70xx (full-image sweep: all such loads are inside the SSU
  driver cluster itself). Display state reaches the panel ONLY through
  the shared 6-byte exchange + 16-bit words assembled from shared RAM.
- Wire (parked 25 s boot): 1140 upload + 1070 exchange frames, ALL
  `C2 00 00 08 00 00` (distinct 6-byte patterns: 1) + 3 init words
  0x1604/0x10FF/0x11FF then 1070 x 0x1300 (one per poll, ~6 ms after
  its frame); 6420 RX reads all at 0x0C077886 (1:1 full duplex).
  RX idle `00 00 00 01 01 01`. Zero ASCII/text bytes anywhere.
- Therefore the exchange/16-bit traffic is control/indices (mode +
  flag bits + 24-bit state + opcode words); glyph rendering is
  panel-CPU-side (manual: DATA FL via D_DSO/D_SCLK/D_LAT/D_BK, JOG FL
  via J_DSO/J_SCLK/J_LAT/J_BK, both from IC1007). MAIN's text tables
  have no code path to any SSU TX site (exhaustive pool/branch/movi20
  sweep); they serve state-side UI only.
- vfd.c implements index passthrough: hex frame + frame count, synced
  on TX[0].bit7 (builder-proven). `?`+log-once retained (never fires
  on hex; kept for the DisplayTask-text future). vfd_text() signature
  and headless safety unchanged.
- Blind spots (concerns): SSTDR1 (+7) bytes are NOT captured by ser.c
  (16-bit LO bytes invisible to vfd_note_tx; HI bytes arrive, so words
  cannot be reassembled in vfd.c); upload-phase windows are unaligned
  raw bytes until polling locks sync; a future 16-bit word with HI
  bit7 set would cost 1-2 misaligned frames before relock.

## Task 8k: service-mode holds, USB-attached boot, panel ACTIVE frames (2026-10-14)

> SUPERSEDED 2026-11-16: see Corrections at top (panel link is SCIF1, not SSU0).

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8k-report.md.
Branch `cdj850-bringup`, BASE `8c79b14` (actual HEAD at task time).
EB `~/qemu-build-eb/qemu-system-sh4eb` (rebuilt once via
`./build.sh display`; warning-free apart from the pre-existing
`ld: -undefined error` notice). No subagents. No bridge. Firmware
read-only (`C850MAIN.UPD` never written; one COPY staged onto a USB
image, source untouched); derived images only in untracked
`extract/cdj850/` (`usb-8k.img`, `usb-upd-8k.img`); run/probe scripts
in /tmp only (uncommitted).

- Key injection at reset: NONE existed. `panel_send_key` has zero
  callers (holds idle; key-event format unmapped); `boot_main.sh` and
  deck pass no keys to cdj850 (no panel socket/env on this board;
  `cdj_panelkeys.h` report bits belong to the cdj2000-family
  GUI-board socket, NOT the cdj850 SSU frame — reusing them as SSU
  bytes would be invention). boot.bin holds no SSU/port movi20 refs.
  The power switch goes to the PANEL CPU (manual §5 [1-1]) and the
  panel diag is a "Panel microcomputer function" (manual §6) — held
  keys are panel-side inputs; MAIN can only react over serial.
- Mechanism (committed): `CDJ850_PANEL_RX=bb:bb:bb:bb:bb:bb`
  (panel_link.[ch] + board.c init call, default OFF = byte-identical
  idle). Values are documented SYNTHETIC test vectors, never cited
  as panel-matrix key codes (matrix unknown). Causality proven live:
  held `40:01:01:01:01:01` reads back in situ at `0x0C7F6721`.
- (1) SERVICE-MODE BOOTS (5 modes + idle control, 30 s each): all
  park identically (mask `0x0CE49CD0` = 0, ACCEPT `[0x0CE4B8F5]` = 0,
  `[82A]` = 0, stop PCs known park/tick spins). No mode bypasses the
  DSP handshake (Port-E untouched by panel RX); panel diag is
  panel-side by construction; RELOOP-10s version is a runtime hold,
  unreachable while parked; updater/factory/drive entries show no
  MAIN-side divergence. Verdict NOTHING per mode (honest stop).
- (2) USB-ATTACHED BOOT: images built per repo flow
  (`scripts/media/make_usb_image.py`, 32 MB FAT16; `usb-8k.img` = 3
  user WAVs, `usb-upd-8k.img` = + `C850MAIN.UPD` copy; contents
  re-listed via pyfatfs). Attach fails at the QEMU command line:
  `-device usb-storage: No 'usb-bus' bus found` — the cdj850 machine
  defines no USB host controller, so no device can reach the
  firmware. USB block (`0xFFFC1C00`) sees only bounded init writes
  (SYSCFG/INTENB/PIPESEL shapes, `-d unimp` 15 s boot); no
  enumeration without a controller model. Verdict NOTHING (blocked
  at harness; model nothing, characterized).
- (3) PANEL ACTIVE FRAMES (3/3 variants, all routing shapes from the
  disassembled 0x0C005F6E branches): `40:01:01:01:01:01` stores
  `0x0101` at `[0x0C7F672C]`, `41:...` at `[0x0C7F672E]`,
  `20:...` at `[0x0C7F6732]` — first live execution of the cddabuff
  non-idle route, bank/nibble-exact. Consumer
  (`jsr @[0x0C006130]`, needs a bank triple) never fires (static
  hold fills one word; words persist); cold path `0x0C0FF610`
  / `[82A]` store `0x0C0FF6B0` never executes (`[82A]` = 0 —
  panel RX has no writer path to it); mask/collector/accept
  unchanged. Verdict MOVED (new firmware state, ridden within task
  across all 3 banks) with no park movement; stop (triple-fill
  needs a sequenced hold — beyond the variant bound, noted as
  follow-up, not built).
- Also: RX-buf "6th ref" `0x0C0D5C14` is a `mac.l` encoding
  coincidence, not a reader (5 real refs: 3 exchange pools +
  2 arg loads). DjControl accept stores resolve to flag
  `[0x0C7F663C]=1` + dst `[0x0C7F65BC]` (pool-gated downstream of
  task-state compares — still zero live). Cold func `0x0C0FF640`
  has no pool caller; its entry is `bsr 0x0C0FF610` from
  `0x0C0FF7D4` when `[[0x0C0FF9F8]]` != 0 (both zero in park).
- Suite: `test_sh2a_smoke + test_firmware_cdj850` 15 passed.

## Task 8l: sequenced panel hold (bank triples) — NOTHING, consumer fires into nothing new (2026-10-14)

> SUPERSEDED 2026-11-16: see Corrections at top (panel link is SCIF1, not SSU0).

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8l-report.md.
Branch `cdj850-bringup`, BASE `c24a201` (actual HEAD at task time).
EB `~/qemu-build-eb/qemu-system-sh4eb` (rebuilt once via
`./build.sh display`; warning-free apart from the pre-existing
`ld: -undefined error` notice). No subagents. Firmware read-only;
derived images untouched in untracked `extract/cdj850/`; run/probe
scripts in /tmp only (uncommitted: reused `/tmp/8k-run.py`, new
`/tmp/8l-run3.py` triple-read).

- Mechanism (committed): `CDJ850_PANEL_RX=seq40|seq20|seq10|seqall`
  (new keyword syntax; default OFF; static six-hex-byte holds
  byte-identical when sequenced unused). Each frame is
  `[SEQ_k,01,01,01,01,01]` (`0x0101` routed value as the 8k active
  probes; tail `01 01 01` keeps the DjControl gate passing).
  Sequencing rule: advance one step per 6-byte frame at the byte-0
  slot; `panel_exchange_start()` only re-aligns `rx_pos` (a mid-frame
  restart serves the NEXT selector — pinned by test). First frame
  serves `list[0]`. All bytes documented SYNTHETIC test vectors, never
  key codes. Tests: `tests/test_cdj850_panel_seq.py` (7 tests, TDD
  red→green on the REAL `panel_link.c`).
- Boots (3): (a) `seq40` 30 s: RX `41 ...` in situ, 0x40 bank 2/3
  filled (`672C/672E=0101`); (b) `seqall` 30 s: RX `11 ...`, 0x40 AND
  0x20 triples `0000` (CLEARED — consumer fired twice), 0x10 2/3
  mid-fill; (c) `seq40` triple-read one boot (~30/34/38 s): 1-filled
  → 1-filled → 0-with-`42`-in-situ (dynamic just-cleared proof).
  Fill AND clear cycle continuously, in every bank. `[82A]`/mask/
  ACCEPT/collector/sample(`0x11`)/flags unchanged (all zero) in all 5
  snapshots; stops at known park/tick/sustained-spin PCs
  (`fff8dca2` tick ISR, `0c0d69b4` SD spin, same-family others —
  fixed-time-break variance, not movement).
- Verdict NOTHING (honest stop): the tail consumer
  (`jsr @[0x0C006130]=0x0C0723EC`) fires repeatedly but into nothing
  observable — no cddabuff state advance in the evidence block, no
  `[82A]`, no park movement. Panel-RX story complete: static (8k,
  single fills, no fire) + active (8k, routing) + sequenced (8l,
  triples fire, nothing downstream) all explored. No riding further;
  no key-code guessing/fuzzing/other pins/DSP combos (bound honored).
- Suite: `test_sh2a_smoke + test_firmware_cdj850 + test_cdj850_vfd +
  test_cdj850_panel_seq` 28 passed.

## SCIF1 panel input, multi-frame display & USB browse/load (2026-11-17)

- **SCIF1 RX input framing (`hw/cdj/boards/cdj850/xc/panel_peer.c`):**
  - Frame 1 (`tx/rx` index 0, type `0x01`): bytes `[1..2]` version (`01 00`),
    bytes `[3..4]` signed/wrapping 16-bit big-endian browse rotary encoder
    counter, bytes `[5..6]` jog counter, bytes `[13..14]` button word 0
    (`enter`/`push` = `0x0100`, `jog_touch`/`fwd`/`move`), byte `15` 8-bit
    end-around-carry checksum (`sum8ec` over bytes `0..14`).
  - Frame 2 (index 1, type `0x02`): bytes `[1..8]` button words `1..4`
    (verified against MAIN test-mode tables `0x0C194EA4` / `0x0C194FD8`,
    including word 3 `usb` = `0x0002`, `disc`/`cd` = `0x0004`, `link`/`pc`
    = `0x0001`; word 4 `browse` = `0x0100`, `back` = `0x1000`), byte `12`
    `sum8ec` over bytes `0..11`.
  - Frames `>= 2`: panel sends zero padding while clocking remaining TX
    frames from MAIN until MAIN's `TXI1` ISR drops `TIE`.
  - `CDJ850_PANEL_SCRIPT` injects button press/release and `rot+/-N` events
    against emulated `cdj850_vms()`.
- **SCIF1 TX display framing & fixes (`hw/cdj/boards/cdj850/xc/panel_peer.c`, `xc/vfd.c`):**
  - Disassembly of `0x0C065ED0..0x0C06622C` (`0x0C066046` / `0x0C0660D4`)
    and live TX dumps (`0x0C9D07D0`, row structs at `0x0C9D0B18 + row*0x105`)
    showed that MAIN's `TXI1` ISR (`0x0C0FA13C`) streams `0x80` (header:
    `[1]` seq, `[2]` mode) + `0x81..0xAE` (14-byte payload chunks) + trailing
    `0x60`/`0x70..` frames inside a single `PNL_XBUSY` transaction as long as
    `TIE` remains set.
  - Fixes applied in `xc/panel_peer.c`:
    1. Raised per-transaction clocking limit from `PANEL_NFRAMES` (`3`) to
       `PANEL_TX_MAX` (`64`), preventing multi-frame display bursts (`0x82+`)
       from being truncated.
    2. Reset reassembly when `0x81` (`sub == 1`) arrives while
       `disp_next_sub != 1` so retries cannot splice onto stale chunks.
    3. Matched row tags by `(tag & 0x03) == (row + 1)` rather than exact
       `0x11/0x12/0x13` (browse/track screens use `0x25/0x02/0x03` with row 0
       highlighted and `0x01/0x26/0x03` when rotated to row 1), and tracked
       `disp_tag` changes in change-only logging and `vfd_note_panel_text()`.
- **Live E2E USB verification (`tests/test_cdj850_usb.py`):**
  - `test_live_usb_source_select_browse_and_load` boots `flash_cdj850.bin`
    (`snapshot=on`, `CDJ850_DSP_BOOT_HLE=1`) with a disposable FAT16 USB image
    containing `ALPHA.wav`, `BETA.wav`, and `GAMMA.wav`, and verifies the
    SCIF1 panel log progression under `20000:usb:200,24000:enter:200,26000:rot+1,28000:enter:200`:
    `[.FOLDER.] | [] | []` -> `tag=25/02/03` `[ALPHA.wav] | [BETA.wav] | [GAMMA.wav]`
    -> `tag=01/26/03` `[ALPHA.wav] | [BETA.wav] | [GAMMA.wav]` ->
    `mode=0x0a` `[BETA.wav] | [] | []`.

## Audio path: why MAIN never starts SSI DMA after USB play (2026-11-17)

State at this entry: the DSP56300 core (`dsp56300.c`) boots all four SHI
segments with no HLE and writes ESAI TX0 continuously, but every word is
zero. ESAI RX stays 0 because MAIN never arms SSI0/SSI1 DMA. The 16
CDJ-850 tests pass (USB, panel peer, DSP unit tests, live browse→load→play).
The `MAIN status 01 00 78 01` milestone shows the play LED/status changed.
It does not show audio.

Static findings (MAIN, SH-2A, base 0x0C000000; use the analysed project
`extract/cdj850/ghidra/cdj850.gpr`. An ELF wrapper of main_unpacked.bin
imports as SuperH4 because the SuperH processor has no ELF opinion):

- Audio task loop 0x0C014FC8. It waits on flag 2, receives on mailbox 2
  (`prcv_mbx` 0x0C0CB640) and dispatches through FUN_0C01475E. Each tick it
  calls the CD/PCM state machine FUN_0C01550C only when the audio mode byte
  0x0C7F66C8 == 2. Otherwise it calls FUN_0C014A54.
- SSI DMA start (FUN_0C076474: splits 42 interleaved frames into the
  SSI0/SSI1 buffers, then FUN_0C076288 / FUN_0C0763D8 / FUN_0C075EF6) is
  reached only from FUN_0C01550C and from the 0x0C0766AA state machine.
  Both need audio mode 2 and more than 84 words in the PCM ring.
  Ring: read/write pointers at 0x0C7F6474/0x0C7F6470, buffer
  0x0C1E8E5C–0x0C74B05C. Fill = FUN_0C01529E.
- Audio mode 2 is set only by the type-8 handler FUN_0C006494 on command
  0x0F with flag bit 0. Command senders all go through FUN_0C0D7464(cmd,
  flag, …) → `snd_mbx(2)`. (0x0F, 1) comes only from FUN_0C0EF112, which
  is reached from source switch FUN_0C0EF3CA(10) and from 0x0C0EE62E.
- For any other command, FUN_0C006494 sets audio mode 1, unless byte
  0x0CE6D25A is non-zero; then it forces mode 0. USB-mode entry
  FUN_0C0EEFB0 sets 0x0CE6D25A = 1 (and 0x0CE6D252 = 2). Every writer seen
  stores 1. So in USB mode the ring / SSI state machines above are bypassed.

Runtime (QMP `xp` probes plus `-d exec,nochain` windows, script
`20000:usb,24000:enter,26000:rot+1,28000:enter,42000:play`):

- Commands to the audio task: cmd 2 (twice) during browse/load, cmd 1 from
  0x0C0EAD14. Never 0x0F. 0x0C7F66C8 stays 0 the whole run.
- USB reads of BETA.wav run from about 28.5 s to 30.2 s. Play causes no
  more reads (the test WAV is only 1 s and has been read in full).
- Player-side state 0x0CE6D25F: 0x0A at USB entry, 3 after load, 5 after
  play. The audio task's state byte 0x0C7F6627 (selects the per-state
  handler in FUN_0C014A54) stays 3 after play. So the play transition
  reaches the player task but not the audio task. 0x0C7F6620..27 has many
  writers (FUN_0C009310 and others). That is the next trace.
- Only about 7 new translation blocks run in the 6 s after play. The
  firmware stays in the same loops, so play is "accepted" without any new
  audio-side work.

Open question: where USB-mode PCM leaves MAIN. Either 0x0C7F6627 must reach
4/5 (FUN_0C014A54's play handlers) and that path feeds SSI, or USB audio
uses a route other than the CD ring. Separately: `ssu0_16bit_cmd`
(0x0C07800A, PF7 chip select) is the audio task programming the DAC
(0x1604/0x10FF/0x11FF at task init), not the DSP. ser.c now forwards to
the DSP SHI only while PF3 is low, so these bytes no longer reach it.

### Follow-up trace (2026-11-17, same day): play is deferred by VINYL mode

Tools: gdbstub write watchpoints and hardware breakpoints (a small RSP
client; `-gdb tcp::12345 -S`), QMP `xp` probes, `-d exec,nochain`
windows. Watchpoints on bytes that wider stores also hit, or several at
once, slow emulation enough that the panel script drifts. Watch one
address at a time.

- Player state 0x0CE6D25F: 0x0A at USB entry (0x0C0EF00E), 2 when load
  starts (0x0C0D8292), 3 when cued (0x0C0EAE7A), 5 on play (0x0C0DEC8E in
  FUN_0C0DEB50).
- At play, FUN_0C0DEB50 sends audio command (3, 1) through FUN_0C0D7464
  only if 0x0CE6D0DE != 1. Otherwise it defers: 0x0CE6D0F1 = 1,
  0x0CE6D0F2 = 1, countdown 0x0CE6C9CB = 100 (counts down only in state
  5), sub-state 0x0CE6CA9D = 4.
- 0x0CE6D0DE is the live VINYL jog-mode flag. At boot it is loaded from
  bit 7 of settings byte 0x0C9DA23C (getter 0x0C08EFE0; the store sits in
  the next jsr's delay slot at 0x0C0E8C1A). The factory-defaults loader
  FUN_0C08F294 sets that bit to 1, and FUN_0C0D6E34 saves a changed live
  value after 5 s. So VINYL is on by default and play is deferred.
- The deferred release is in the player tick (0x0C0DDE90…0x0C0DDF2C). It
  needs 0x0CE6CA9C != 0 (the 6th argument of the last audio command,
  zeroed by FUN_0C0D7464) and 0x0CE6D0F1 != 1 (set by play itself and
  never cleared). It never fires. Probably jog-related; not traced further.
- Workaround with no pokes: press the panel `vinyl` button before play
  (script `...,38000:vinyl:200,42000:play:200`). Play then sends (3, 1);
  the audio task goes 0x14 → 4 (FUN_0C009310 called from 0x0C0076A0 and
  0x0C0F3836) and runs the USB play-state handler FUN_0C0F6E6C (states
  4/5 of FUN_0C014A54's non-CD branch; state 3 → 0x0C0F65F0, state 0x13
  (load) → 0x0C0F1318).
- Even then there are no SSI or DMAC accesses (CDJ850_TRACE). Audio mode
  0x0C7F66C8 stays 0: the type-8 handler writes 0 at 0x0C006618 because
  0x0CE6D25A = 1 (set on USB entry, mirrored from 0x0CE6D25B by
  FUN_0C0E8ECC; cleared only by FUN_0C0EA890 when the player is idle).
  Poking 0x0CE6D25A/5B = 0 gives mode 1, not 2. The SSI stream start
  still needs mode 2 = (0x0F, 1) from FUN_0C0EF112 (source 5 or 10). In
  USB mode that comes from FUN_0C0E843C → FUN_0C0EF046, which is a "leave
  USB source" teardown. FUN_0C0E843C runs only from the LINK-key handler
  (0x0C0E65EE; LINK = panel word 3 bit 0 → 0x0CE6CA0C bit 0) and from the
  0x0C9D1258 selector switch. So source 5 is probably LINK, not USB.
- The 20 s test track is read completely during load (1197 SCSI reads,
  none after play). 0x0C1E8E5C–0x0C74B05C (~5.6 MB) looks like a
  whole-track RAM buffer, not a streaming ring. At play 0x0C7F6470 jumps
  once from 0x0C1E8E5C to 0x0C7438EC. Where USB-mode PCM leaves MAIN is
  still unidentified.
- Test fixtures (fixed): `_make_pcm_wav` used to write 1 s of digital
  silence. It now writes 10 s of 440 Hz (L) / 660 Hz (R) at about -8.7 dBFS,
  checked by `test_pcm_wav_fixture_is_audible`. The live USB test presses
  `vinyl` at 38 s, before play at 42 s. Its play milestone is now the
  audio task's play-state byte 0x0C7F6627 == 4, read over QMP `xp`, instead
  of the panel LED status `01 00 78 01`. Without the VINYL press that
  milestone fails, as it should.
- PFPRH bit 0 (PF16, 0xFFFE3A9C) is polled about 3000×/s by the audio task.
  In cued state a high level calls FUN_0C0F032C (cue/start logic). Likely
  an external fader-start input. The emulator returns 0.

### USB PCM sink found: DSP-paced pull over IRQ7 → SSI DMA (2026-11-17)

- Track audio buffer: USB WAV is decoded into the same sector ring CD
  uses. It holds 2400 entries of 0x930 bytes (2352 = one CD-DA sector,
  588 frames) at 0x0C1E8E5C, as big-endian 16-bit stereo.
  FUN_0C005C44(idx) sets the read pointer 0x0C7F6470 = base + idx × 0x930.
  FUN_0C005AA8 aligns it to 84-word (42-frame) chunks, the chunk
  FUN_0C076474 hands to SSI DMA. Fill = write index 0x0C7F645E − play
  index 0x0C7F645C (0x0C7F6466 in state 4), modulo 0x960. The USB read
  scheduler in FUN_0C0F6E6C refills while fill < 500.
- Source buttons: FUN_0C0E827E / 0C0E831A / 0C0E843C are DISC / USB /
  LINK. Audio mode 2 / (0x0F, 1) / source 5 is LINK streaming. Local
  playback stays in audio mode 0.
- Handler table at 0x0C155AB4 ({intno, handler, 7}): 0xD6 → 0x0C075FCA,
  0xD7 → 0x0C0761C4 (SSI0/1), 0x70 → 0x0C0762F4, 0x74 → 0x0C076444 (DMAC
  DEI1/DEI2), **0x47 = IRQ7 → 0x0C076624**, 0x44 = IRQ4 → 0x0C0773EE.
  0x0C076624 is the SSI feeder (its mode ≠ 2 branch also checks PF16).
  IRQ7 is DSPDREQ, so the DSP pulls audio: each DSPDREQ request makes
  MAIN copy the next 42-frame chunk to the SSI0/SSI1 DMA buffers.
- DSP side (segment 4 loads at P:0). The request routine P:0x1FC pulls
  PE4 (DSPDREQ) low at P:0x228 only if `jclr #10,Y:$FFFFF5` passes, i.e.
  only if its PF10 input is high. Otherwise it takes P:0x23C and holds
  DSPDREQ high. After boot DSPDREQ stayed high forever, so IRQ7 never fired.
- MAIN never writes PF13 (PFDRL values seen: 0x288/0x280/0x208/0x88/0x8),
  so the core's "MAIN PF13 → DSP PF10" wiring can never be satisfied by
  this firmware. MAIN does drive PF8 (FUN_0C006294, via the PFDRL shadow
  0x0CEE34B6): high when the PF16 input (PFPRH bit 0) is high and
  0xFFFE399E bit 4 is low; low in audio mode 1. With PF16 forced high,
  PF8 goes high from about 7.9 s and stays high.
- Experiment (env hooks, defaults unchanged): `CDJ850_PF16_HIGH=1`
  (port.c) plus `CDJ850_DSP_PLAY_PF=8` (dsp56300.c: DSP PF10 follows
  MAIN PF8). Result: at 1266 ms the DSP pulls DSPDREQ low (P:0x228), the
  IRQ7 feeder arms SSI DMA ch1/ch2 (DMARS 0x23/0x27, 45 words each, TE
  raised), and ESAI RX receives 90 words. Then DSPDREQ returns high
  (P:0x42F) and never requests again.
- Open:
  (1) What really drives MAIN PF16 and DSP PF10 (needs the MAIN/DSP
      schematic sheet). The firmware's behaviour points at PF8 → DSP PF10
      with PF16 as an input that is high in normal operation.
  (2) The DSP has to consume RX at frame rate (ESAI RX interrupts per
      44.1 kHz frame) to request again. That is the core timing and
      interrupt work, not MAIN.
  The track buffer is filled before the first request, so the first burst
  at 1.2 s carries whatever the ring held at boot.

### ESAI data path fixed; real tone samples reach the DSP output (2026-11-17)

DSP application facts (segment 4 at P:0, read at run time and with pyghidra):
ESAI vectors $30 → JSR $3BB (RX), $34 → $3BA (RX exception), $38 → $43F
(TX), $3C → $43E. TCR = 0x527D01 (TE0, TIE). RCR = 0x000903 idle; the
request routine sets RIE (bit 22) only while a block is being received.
IPRP = 3 (ESAI at IPL 2).

Block protocol (MAIN FUN_0C076474 → SSI0/SSI1 → ESAI RX0/RX1):
- Per channel, 45 SSITDR words: [0, header, 42 sample words, 0]. The
  header is 0xCE5C0000 on SSI0 and 0xCA5A0000 on SSI1. Each sample word
  packs two 16-bit samples of one channel (SSI0 right, SSI1 left; frame n
  in bits 15:0, frame n+1 in 31:16). That is 84 frames per block.
- SSICR = 0x90102 gives DWL = SWL = 16 bits, so each 32-bit write goes
  out as two serial words, low half first. The DSP sees them as
  `half << 8`.
- DSP RX handler: hunting (Y:$20 bit 0) for RX0 = 0xCE5C00 after a 0 word
  with RX1 = 0xCA5A00 → data mode, DSPDREQ high (ack), LFSR Y:$71 =
  0xEFFFFF with taps Y:$72 = 0xCE5C00. Each RX interrupt stores one word
  and steps the LFSR. The block ends when it reaches Y:$70 = 0x413F5B,
  which is exactly 84 steps; then RIE is cleared. The request routine
  P:0x1FC asks for a new block when the input fill X:2 − X:1 runs low
  (and the PF10 input is high).

Emulator fixes:
- `xc/ssi.c`: with SSICR.DWL = 16 bits, each SSITDR word goes out as two
  serial words (bits 15:0 first; PDTA = 1 swaps), each pushed as
  `half << 8`. It used to send one `(val >> 8)` word, so the DSP never
  matched its headers. vmstate v3 (tx_half).
- `dsp56300.c`: with RIE set, an RX word stays in the FIFO until the
  interrupt can be taken (`dsp_esai_irq_ready`: not in REP, IPL 2
  unmasked). It used to be popped and its interrupt dropped, which lost a
  word, so the 84-word LFSR count never finished: RIE stayed set and the
  DSP deadlocked after a few blocks.
- `dsp56300.c`: the instruction budget per ESAI slot is now
  `slot_budget` (default 64 ≈ 5.6 MIPS, `$CDJ850_DSP_IPS`), up from 23.
  256 used so much host time that MAIN missed USB timing and never
  selected the USB source (the virtual clock follows the host).
- Debug: `CDJ850_DSP_DEBUG` prints the TX peak per 500 ms and, once at
  tick 3000, the ESAI vectors and registers.

Result (with the experiment hooks `CDJ850_PF16_HIGH=1`,
`CDJ850_DSP_PLAY_PF=8`, a 10 s 440/660 Hz track, VINYL off, play at 42 s):
blocks stream continuously from boot (≈130 requests). During the track
load (≈29.9–31.2 s) the DSP's TX0 output peaks at 0x2EDF00 = 11999 << 8,
the fixture's amplitude, over ≈20k non-zero words. In the same window
DSPDREQ also rises from P:0xEDE (the init path), so MAIN appears to
re-initialise the DSP during load. Afterwards the DSP stops consuming
(input fill 0x1F8, RIE clear), consistent with sitting at the cue point,
and it does not resume at play. A `-audio driver=wav,path=…` capture with
`CDJ_DSP_AUDIO=1` was silent in that run (that run's timing differed);
the TX peak counter is the evidence so far.

Open: how MAIN tells the DSP play/pause. Candidates are the SHI command
word (MAIN TX `c2 80 00 …` before play, `c2 00 00 …` after) and the PF10
PLAY input (real driver still unknown; see PF8/PF16 above).

### DSP play/pause signalling (2026-11-17)

- MAIN builds the 6-byte SHI command in FUN_0C005D54 (TX buffer
  0x0C7F671B), sent every few ms by ssu0_dsp_exchange:
  byte 0 = flags (0x80 always; 0x40 in audio modes 0/2; 0x20 if
  0x0C7F672A; 0x10/0x0C/0x02/0x01 from track and status bits);
  bytes 1–2 = u16 at 0x0CE6C938 (bit 15 = HOLD); bytes 3–5 = speed at
  0x0CE6C940 (0x080000 = 1.0×). In audio mode 2 (LINK) word 1 is a
  constant 0x8000 and the speed comes from 0x0C7F6764.
- FUN_0C0D59B8 (player tick) derives HOLD and speed from the sub-state
  0x0CE6CA9D: 0 → clear HOLD, speed from pitch (only when the pitch value
  0x0CE6C932 differs from the last 0x0CE6C930, or the force flag
  0x0CE6D0F5 is 1; set by a pitch-button handler in FUN_0C0D8D78);
  1 → HOLD clear; 2/0x12 → HOLD unless 4/0x24; 4/0x24 (VINYL start) →
  HOLD clear, speed ramped from the DSP's reported speed (SHI reply
  word 2 with bit 0x80, copied to 0x0C0D5C18); 3/0x23/0x33/0x43 → HOLD.
- SHI RX transitions (now logged under CDJ850_DSP_DEBUG): 0xC20000 +
  0x080000 at boot, 0xE20000 from 1.2 s, 0xE28000 (HOLD) from track load
  (≈28.5 s).
- CDJ mode (VINYL off): play leaves the sub-state at 0, so HOLD is never
  refreshed (the pitch value is constant in the emulator) and stays at
  0xE28000. Poking the force flag 0x0CE6D0F5 = 1 after play makes MAIN
  send 0xE20000, but the DSP still does not resume consuming.
- VINYL mode (default): play sends 0xE20000 and then a speed ramp
  (0x033334, then 0x000000). The ramp follows the DSP's own reported
  speed: 0xC80000 (1.0×) while cued, 0xC4B7E7 from about 37 s with no
  input event, and MAIN ends at speed 0. Holding `jog_touch` gives speed 0
  and reply 0xC00000 immediately, so the touch polarity is right.
- Open: why the DSP's reported platter speed decays from 1.0× while
  cued, and why HOLD-clear (0xE20000) does not restart consumption. Both
  are in the DSP's SHI command handler (main loop P:0x11E…0x1EE, reply
  writer P:0xCA1/P:0xEC3) and possibly its PF10 input, which is still
  wired by the PF8 experiment.

### DSP SHI command handler & reply writer (2026-11-17)

Decoded from `dsp_seg4_app.bin` (`DSP56300:LE:24` in pyghidra; note that
8-bit ALU immediates `#<xx` load bits 23:16, i.e. `xx << 16`):

- **Framing (`P:0x11E`–`0x156`):** polls HCSR (`X:$FFFF91`) bit 17
  (`HRNE`) and reads HRX (`X:$FFFF94`).
  - Word 0 (`bit 23 == 1`): saved to `X:<0x25` (with bit 23 kept set as
    pending flag). If `PF10 == 0` (`Y:$FFFFF5` bit 10), `Y:<0x20` bit 16
    is set, and `(word0 & 0x0C) == 0x0C`, jumps to `P:0xE3C` (re-init /
    ESAI RX enable path). Otherwise returns to `P:0x1EE` to wait for
    Word 1.
  - Word 1 (`bit 23 == 0`): requires `X:<0x25` bit 23 set (clears it),
    logs `(cmd0, speed)` to circular buffer `Y:(r3)+` (`r3 = X:$E7`,
    `m3 = 0x7FF`), and validates speed: for `speed < 0x400000`, `|speed| >
    0x100000` (2.0×) clears `cmd0` bit 21; for `speed >= 0x400000`,
    `(cmd0 & 0x300) == 0x300` clears `cmd0` bit 21 and `speed` bit 22,
    else only `0x400000` or `0x400A00..0x500000` are accepted (others
    jump to `P:0x1EC`).
- **Word 0 bits (`X:<0x25`, `Y:<0x22` = previous Word 0):**
  - Bit 21 (`0x200000`): `0` (`0xC2xxxx`) sets `Y:<0x20` bit 20 and
    `X:<0x0C` bit 21 (skips `FUN_P_000366`); `1` (`0xE2xxxx`) clears
    `Y:<0x20` bit 20 so `P:0x1F2` calls `FUN_P_000366` (resets `X:<0x26`
    and `Y:$104`/`Y:$54` if `X:<0x0C` bit 21 was set).
  - Bit 15 (`0x008000`): **HOLD** (`P:0x1A0`, `P:0x1D4`, `P:0x1E0`).
  - Bits 11, 9, 8 (`0x000B00`): mode bits; changes set `X:<0x0C` bit 19
    (`P:0x16D`). When `(cmd0 & 0x300) == 0x300` and `X:<0x0D` bit 8 == 0,
    `P:0x176`–`0x194` derives speed from `cmd0 & 0xFF`.
  - Bit 10 (`0x000400`): **reverse/direction** (`P:0x195`–`0x196` negates
    speed; mirrored into reply bit 21 at `P:0xC89`–`0xC8C`).
- **Speed & HOLD state (`P:0x197`–`0x1EB`):** `X:<0x1E` is the target
  speed and `X:<0x1F` (`:X:<0x20`) is the active speed. On the normal
  path (`Word 1` bit 22 == 0, `X:<0x0D` bits 8 and 23 == 0):
  - `new_speed == old X:<0x1E`: HOLD=1 writes `X:<0x1F = new_speed`
    (`P:0x1E2`); HOLD=0 skips `P:0x1E2` and only updates `Y:<0x1C`.
  - `new_speed != old X:<0x1E`: updates `X:<0x1E = new_speed`; HOLD=1
    (with `X:<0x0D` bit 13 == 0 or `|new_speed| == 0x080000`) writes
    `X:<0x1F = new_speed`, while HOLD=0 leaves `X:<0x1F` to be ramped by
    `FUN_P_000731` (`P:0x736`–`0x760`).
- **Reply writer (`FUN_P_000c44`, `P:0xC44`–`0xCBA`):** when `HTDE`
  (`X:$FFFF91` bit 15) is set (`P:0xC95`), writes `HTX` (`X:$FFFF93`).
  `P:0xC80`–`0xC8E` builds `Y:<0x1E = 0xC00000 | max(|X:<0x1F|, |Y:<0x2 >>
  2|)` (`0x800000` if `X:<0x0D` bit 23 is set; `| 0x200000` if `Y:<0x22`
  bit 10 is set). Thus the `0xC80000` → `0xC4B7E7` reply decay directly
  reflects `X:<0x1F` decaying.
- **Main loop gates (`P:0x1EE`–`0x1FB`):** calls `0x1FC` (DSPDREQ,
  gated on `PF10 == 1`), `0x2B9` (ESAI RX ring), `0x366` (if `Y:<0x20`
  bit 20 == 0), `0x731` (consumption/speed ramp, gated on `X:<0x0C` bit 2
  == 0 and `X:<0x0C` bit 3 == 1), `0x498`/`0xB24`, and `0xC44` (reply).

### FUN_0C0D59B8: VINYL ramp-down and CDJ HOLD=1 on Play (2026-11-17)

Traced in `/main_unpacked.bin` across `FUN_0c0d59b8` (speed/HOLD builder),
`FUN_0c0deb50` (Play/Pause button handler), `FUN_0c0dd978` (player tick),
`FUN_0c0d7464` (audio command sender), and `FUN_0c0d8d78` (mode buttons):

- **VINYL mode ramps speed down to 0 on Play (`*0x0CE6D0F2 == 1`):**
  - In `FUN_0c0d59b8`, when `0x0CE6CA9D` (`PTR_DAT_0c0d5c24`) is `4` or
    `0x24`, it clears `HOLD` (`0x0CE6C938 &= 0x7FFF`) and branches on
    `*0x0CE6D0F2` (`*PTR_DAT_0c0d5c50`): `*0x0CE6D0F2 == 1` (`0x0C0D5AFE`–
    `0x0C0D5B98`) subtracts `FUN_0c0d5fba(...)` from `0x0CE6C940`
    (`0x0C0D5B7E: sub r0, r1`), ramping speed down to `0` (and leaves
    `0x0CE6CA9D` / `0x0CE6D0F2` unchanged once `0x0CE6C93C & 0x1FFFFF ==
    0`), whereas `*0x0CE6D0F2 == 0` (`0x0C0D5B9A`–`0x0C0D5CF8`) ramps up to
    `FUN_0c0d5fec(*(short *)0x0CE6CA50)` and then clears `0x0CE6CA9D = 0`.
  - When Play (`0x2000`) is pressed from cued (`0x0CE6D25F == 3`) in VINYL
    mode (`0x0CE6D0DE == 1`), `FUN_0c0deb50` (`0x0C0DEC80`–`0x0C0DED28`)
    sets `0x0CE6D25F = 5`, `0x0CE6D0F1 = 1` (`0x0C0DEC98`), `0x0CE6D0F2 = 1`
    (`0x0C0DECB4`), `0x0CE6C9CB = 100`, and `0x0CE6CA9D = 4` (`0x0C0DED28`),
    skipping `FUN_0c0d7464(3, 1)`. (On Pause `5 -> 4` at `0x0C0DEF18`–
    `0x0C0DEF58`, `FUN_0c0deb50` sets `0x0CE6D0F1 = 0`, `0x0CE6D0F2 = 0`,
    `0x0CE6CA9D = 4`.)
  - `FUN_0c0dd978` (`0x0C0DDEC8`–`0x0C0DDF2C`) only clears `0x0CE6D0F2 = 0`
    and switches `0x0CE6CA9D = 0x24` when `*0x0CE6D0F1 != 1` (`0x0C0DDEE2`),
    which is blocked by `0x0C0DEC98` setting `0x0CE6D0F1 = 1`. Thus
    `FUN_0c0d59b8` keeps taking the `*0x0CE6D0F2 == 1` ramp-down path to
    `0`.
- **CDJ mode leaves `HOLD=1` (`0xE28000`) on Play unless `0x0CE6D0F5` is
  poked:**
  - When Play is pressed in CDJ mode (`0x0CE6D0DE == 0`), `FUN_0c0deb50`
    (`0x0C0DED34`–`0x0C0DED56`) sets `0x0CE6CA9D = 2` (since it was
    non-zero while cued) and calls `FUN_0c0d7464(3, 1)`, which at
    `0x0C0D7728` also forces `0x0CE6CA9D = 2` if non-zero.
  - On the first tick of `FUN_0c0d59b8` (`0x0CE6CA9D == 2`), the `2`/`0x12`
    branch sets `HOLD=1` (`0x0CE6C938 |= 0x8000`, `0xE28000`) and clears
    `0x0CE6CA9D = 0`.
  - On subsequent ticks (`0x0CE6CA9D == 0`), `FUN_0c0d59b8` only clears
    `HOLD` (`0x0CE6C938 &= 0x7FFF`) inside `if (*(short *)0x0CE6C930 !=
    *(short *)0x0CE6C932 || *0x0CE6D0F5 == 1)`. Because pitch
    `0x0CE6C932` is constant in the emulator (`== 0x0CE6C930`) and
    `0x0CE6D0F5` is `0` (normally set by the `0x100`/MT button handler in
    `FUN_0c0d8d78` at `0x0C0D8E4E`), `0x0CE6C938` stays `0x8000`
    (`0xE28000`) until `0x0CE6D0F5` is poked to `1`.

### DSP consumption rings, PF10 polarity & `dsp56300.c` decode bugs (2026-11-17)

Traced in `/dsp_seg4_app.bin` (`dsp_seg3_stage3.bin` init constants) and
verified against live QEMU (`CDJ850_DSP_DEBUG=1`):

- **DSP three-ring audio pipeline (`dsp_seg3_stage3.bin` `P:0x000..0x05D`):**
  1. ESAI RX ring (`Y:0x15000..0x157FF`, size `0x800`): written by ESAI RX
     ISR `P:0x3BB..0x43D` (`X:<0x02`, 168 sample words per 84-frame block,
     clears `Y:<0x20` bit 16 and `RCR.RIE` at block end `P:0x3E7..0x3E8`),
     read by `FUN_P_0002b9` (`X:<0x01`). Fill `Y:<0x00> = (X:<0x02> - X:<0x01>) mod 0x800`.
  2. Main sample ring (`X:0x10000..0x11FFF`, size `0x2000`): written by
     `FUN_P_0002b9` (`X:<0x03>`), consumed by `FUN_P_000731` (`X:<0x04>`,
     advancing by `2 * ((|X:<0x1F>| * 16 + X:<0x21>) >> 24)` words per call at
     `P:0x798..0x7AA`). Fill `X:<0x38> = (X:<0x03> - X:<0x04>) mod 0x2000`.
  3. ESAI TX ring (`Y:0xE80..0xEBF`, size `0x40`): written by `FUN_P_000731`
     tail `P:0x895..0x897` (`X:<0x06>`), drained by ESAI TX ISR `P:0x43E..0x454`
     (`X:<0x00>`).
- **`P:0x1FC` (`DSPDREQ`) & `P:0x731` gating:**
  - `P:0x1FC` only requests a new block (`RCR.RIE = 1`, `PE4` low) when
    `PF10 == 1` and `Y:<0x00> <= y0` (`0x161` at `1.0x`, `0xDA` at `0x04B7E7`,
    `0x03` at `0`). Once `P:0x731` stops consuming `X:0x10000`, `0x2B9` stops
    draining `Y:0x15000`, `Y:<0x00>` stays above `y0`, and `DSPDREQ` stops.
  - `P:0x731` requires `Y:<0x20>` bit 16 == `0` (`P:0x1F0`), `X:<0x0C>` bit 2
    == `0` (`P:0x1F4`, TX ring `out_fill < 0x20`), `X:<0x0C>` bit 3 == `1`
    (`P:0x731`, sample ring ready), `Y:<0x20>` bit 17 == `0` (unmuted), and
    `X:<0x1F> != 0`.
  - `HOLD=0` (`0xE20000`) never writes `X:<0x1F>` directly (`P:0x1E2` requires
    `HOLD == 1`); it only sets `X:<0x1E>` and relies on `P:0x731` (`P:0x768`)
    to ramp `X:<0x1F>` toward `X:<0x1E>`.
- **`PF10` (`Y:$FFFFF5` bit 10) polarity (`11` accesses in `dsp_seg4_app.bin`):**
  - `PF10 == 1` enables `DSPDREQ` (`P:0x218`, `P:0x237`, `P:0xE45`) and `0x498`
    (`P:0x49B`), and prevents silence/fade from arming `X:<0x0C>` bit 16
    (`P:0x878`, `P:0x899`).
  - `PF10 == 0` blocks `DSPDREQ`, arms `X:<0x0C>` bit 16 on silence/fade, and
    puts the DSP into `Y:<0x20>` bit 16 standby (`P:0x242..0x244`). Re-init
    `P:0xE3C` (`P:0x128`) only triggers if `PF10 == 0`, `Y:<0x20>` bit 16 == `1`,
    and `(word0 & 0x0C) == 0x0C`.
- **Live QEMU diagnostic (`CDJ850_DSP_DEBUG=1`, without `CDJ850_PF16_HIGH`):**
  - MAIN itself drives `PFDRH` bit 0 = `1` (`PFDRH=0x0401`, `PFIORH=0x0001`,
    `PFPRH=0x0401`, so `PF10=1`) during track load (~29.15 s).
  - ESAI RX stops during track load/cue at varying block counts (e.g. `720`
    words / 4 blocks, `1440` words / 8 blocks, `3330` words / 18.5 blocks)
    with `X:<0x0C>` bit 2 set (`0x01000c` / `0x00000c`) and `X:<0x0D>` in
    `0x006000`/`0x026000`.
  - Poking `0x0CE6D0F5 = 1` clears `0x0CE6C938` (`0x8000 -> 0x0000`) and
    switches SHI to `0xe2 0x00 0x00 0x08 0x00 0x00` (`Y:<0x22> = 0x620000`),
    but DSP consumption does not resume.
- **Emulator audit — critical `dsp56300.c` decode bugs in hot audio/SHI paths:**
  1. `0x0A`/`0x0B` long-displacement `MOVE` (`0x0B71xx` / `0x0B72xx` with
     `bit 7=1, bit 5=0` in `P:045c`, `P:0b68`, `P:0bb4`, `P:0dd2`, `P:0de5`,
     `P:0dea` — called from `P:0498`, `P:0b24`, `P:0dd0`) is misdecoded at
     `dsp56300.c:1668-1742` as `JSCLR`/`JSSET`, jumping to `P:0002` (reset
     vector) / `P:0003` / `P:0009` and triggering `P:0ec3`/`P:0ede` resets.
  2. `Tcc` (`dsp56300.c:1743-1762`) tests bit 15 (`0x8000`) instead of bit 11
     (`0x0800`) for `S2, D2`, breaking `TCS X1, A/B` (`P:02d5`, `P:081f`,
     `P:0820`), `TCS X0, A` (`P:0c88`), and `Tcc S2, D2` (`P:063d`, `P:069b`,
     `P:06a1`, `P:06ab`).
  3. `0x0A`/`0x0B` `<ea>` bit-ops (`mid` in `0x40..0x7F`, `dsp56300.c:1696`)
     use `mid & 0x3F` instead of `eval_ea`, breaking 13 instructions (`P:037d..0382`,
     `P:047c..0481`, `P:0bd1`, `P:0c5f..0c65`).
  4. `ROL A` (`0x37`, `P:044c: 0x202a37` in ESAI TX ISR `P:043f`) is omitted
     from `exec_alu` (`dsp56300.c:858-959`) and silently skipped.

### MAIN play-path audit: VINYL release, CDJ HOLD, PF16 (2026-11-17)

Static trace in `/main_unpacked.bin` (base 0x0C000000), started by an
Antigravity session (its notes: `~/.gemini/antigravity/brain/3ae13e34-…/scratch/`)
and finished here. No emulator code changed. Several conclusions in the
three entries above are corrected here.

**PF16 (answered).**
- PF16 is a MAIN **output**. `0x0C0B91CA` writes PFIORH = 0x0001 (shadow
  0x0CEE348E); boot.bin never touches PFIORH. Reads of PFPRH bit 0 are MAIN
  reading back its own pin, not an external input (not fader start).
- `FUN_0C005B14` = "start DSP stream": if PF16 is low, it clears PF8 (unless
  audio mode 2), clears a stale IRQ7 (DSPDREQ) flag in 0xFFFE0806 bit 7, sets
  the low nibble of 0xFFFE081A to 7 (IRQ7 priority), then PFDRH |= 1.
  `FUN_0C005BB6` = "stop": 0x0C7F6678 = 1, PFDRH &= ~1. Both have dozens of
  callers in the CD and USB audio-task state handlers. The USB play handler
  `FUN_0C0F6E6C` calls the start routine at 0x0C0F6F86.
- Net "PLAY" (service manual 10.2/10.4) carries MAIN PF16 to DSP PF10, the
  gate of the DSP's DSPDREQ request routine `P:0x1FC`. e47bad7 already models
  this (port.c PFPRH readback, dsp56300.c `YIO_PDRF`). So
  `CDJ850_PF16_HIGH` and `CDJ850_DSP_PLAY_PF=8` are obsolete (table updated).
  The PFPRH checks in `FUN_0C0DD978` (with 0x0CE6CA80 = last audio command,
  0x0CE6CA81 = its second argument) are "stream not running" gates.

**CDJ mode HOLD (answered; not a MAIN blocker).**
- HOLD (0x0CE6C938 bit 15, SHI word 0 bit 15) means "set the speed now,
  without a ramp", not "pause". DSP `P:0x1E2` writes X:<0x1F> (active speed)
  directly only with HOLD = 1; with HOLD = 0, `FUN_P_000731` ramps it.
- CDJ Play: sub-state 0x0CE6CA9D = 2. `FUN_0C0D59B8` then sets
  C940 = 1.0x + pitch with HOLD = 1 (instant start) and drops to sub-state 0.
  In sub-state 0, HOLD clears only on a pitch change (0x0CE6C932 ≠ 0x0CE6C930,
  from pitch ADC `FUN_0C0D1F14(9, 0)` in `FUN_0C0E49CC`) or a
  Master Tempo/Tempo Range press (0x0CE6D0F5, `FUN_0C0D8D78` at 0x0C0D8E4E),
  so pitch changes glide. HOLD staying 1 after Play is intended.
- Therefore "CDJ mode Play with HOLD = 1 (0xE28000, speed 0x080000) and the
  DSP does not consume" is a DSP fault, not a missing MAIN input. Check it
  again after the `dsp56300.c` decode fixes. Also check whether the DSP's speed
  reply reads 0 while cued: cued is sub-state 3, which makes `FUN_0C0D59B8`
  send speed 0 with HOLD, so the 0xC80000 (1.0x) reply seen earlier is
  suspect.
- Correction: `FUN_0C0D7464` forces sub-state 2 only for commands 2, 4
  and 0x0E, not command 3 (play). The entry above said otherwise.
  0x0CE6CA9C is that function's 6th argument.

**VINYL release (narrowed; still open).**
- Flags:
  - 0x0CE6D0F1 is a **play-intent latch**. Play sets it (from cue in both
    modes; `FUN_0C0DE75E` toggles it during load).
  - 0x0CE6D0F2 is the **brake/held flag**.
  - In sub-state 4/0x24 of `FUN_0C0D59B8`, D0F2 = 1 lowers speed by
    `(0x0CE6C954 << 19) / table[0x0CE6C950]` (the brake curve, setting
    0x0CE6C950). D0F2 = 0 raises it toward `FUN_0C0D5FEC(pitch 0x0CE6CA50)`
    (the start curve).
  - 0x0CE6D0E0 is Master Tempo (speed bit 22). With MT on, the brake branch
    adopts the DSP's reported speed instead.
  - 0x0CE6D0F3 is jog touch (`FUN_0C0ED0B4`/`FUN_0C0ED10C`, panel byte 0x0C
    bit 0x20).
- VINYL Play from cue (0x0C0DEC98…0x0C0DED28): D0F1 = 1, D0F2 = 1,
  countdown 0x0CE6C9CB = 100, sub-state 3 (if 0x0CE6D0FB ≠ 0 or
  `FUN_0C008546(play index)` returns 0) or 4. So the unit enters
  brake/hold at speed 0.
- Every path found that leaves that posture is blocked while D0F1 = 1:
  - the release in `FUN_0C0DD978` (needs D0F1 ≠ 1 to set sub-state 0x24 and
    D0F2 = 0);
  - `FUN_0C0ED10C` touch release (needs D0F1 = 0 unless state > 9);
  - `FUN_0C0D7464` (clears D0F2 only for commands 2/4/0x0E and with
    D0F1 = 0).
- Every D0F1 = 0 writer checked is pause, cue, eject or load reset
  (`FUN_0C0DEB50` 5→3/4, `FUN_0C0DC996`, `FUN_0C0EB704`, `FUN_0C0E4BBE`,
  `FUN_0C0DE680`).
- The only motion path in that posture is jog movement within the 100-tick
  window while the DSP reports a non-zero speed, giving 0x0CE6D0E6 = 1
  (`FUN_0C0DD492`). That is scratch mode: speed follows the jog
  (`FUN_0C0DD978` second half, sub-state 1).
- Conclusion: no MAIN-internal VINYL start from cue was found with MT off.
  Next step is dynamic: once the DSP decode bugs are fixed, run VINYL Play
  with gdbstub write watchpoints on 0x0CE6D0F1, then 0x0CE6D0F2 (one at a
  time), and see whether a correct DSP speed reply changes the path. Until
  then, CDJ mode (press `vinyl` once) is the test path.


### `dsp56300.c` decode fixes and first post-Play audio (2026-11-17)

All four decode bugs from the audit above are fixed, with opcode tests
taken from the DSP application in `tests/test_cdj850_dsp.py`:

1. Long-displacement `MOVE X/Y:(Rn+xxxx)` (`0000 101S 0111 0RRR 1WDD DDDD`
   plus a displacement word) is decoded before the bit-op page; W=1 loads.
   For example P:045c `0x0b7197` = `MOVE R7,Y:(R1+9)`, P:0dd2 `0x0b72d4` =
   `MOVE Y:(R2+2),R4`.
2. `Tcc`: the address-only form is bit 11 with bit 16 clear (CCCC fills bits
   15:12). P:02d5 `0x028060` = `TCS X1,A`; `0x022902` = `TNE R1,R2`.
3. Bit ops with middle byte `01MMMRRR` go through `eval_ea` (with Rn
   updates; the absolute form takes the extension word, jumps excepted).
   P:037d `0x0a6140` = `BCLR #0,Y:(R1)`, P:047c `0x0a79c0` =
   `JCLR #0,Y:-(R1),$481`.
4. `ROL` (0x37/0x3F) and `ROR` (0x27/0x2F) rotate A1/B1 through C.

Live run (CDJ mode, `38000:vinyl`, `42000:play`, no experiment knobs,
`CDJ_DSP_AUDIO=1 -audio driver=wav`): no unimplemented ops, no WILD PC.
- The DSP's output now carries signal at the fixture amplitude (peak 11999)
  for about 15 s around load/cue and again after Play (host-clock seconds
  47-62 and 66-68). The old captures were silent.
- It is **not** the fixture tone. Both channels peak around 1.2-1.5 kHz
  with broadband energy (the strongest bin holds 10-30 % of the energy),
  against 440 Hz L / 660 Hz R expected.
- Next: find where samples get scrambled. Candidates are the SSI half-word
  order and channel interleave (SSI0 = right, SSI1 = left), the DSP sample
  ring stride `2 * ((|X:<0x1F>| * 16 + X:<0x21>) >> 24)` at P:0x798, and
  host pacing of the WAV sink.
- Flake: with `CDJ850_DSP_DEBUG=1` plus WAV capture, or during a full
  test-file run, MAIN sometimes misses the 20 s `usb` press and stays on
  "NO DISC". The live test alone passed 3/3.

### Audio path debugged: clean 440/660 Hz tone from the DSP (2026-11-17)

After 9d62f5b the DSP output had the fixture's level but no tone. The debug
used temporary taps (raw ESAI RX/TX words, SSITDR/SSICR writes, presented
interrupt vectors and IRQ7 priority writes, all with virtual timestamps);
the taps are removed. Five causes, in the order they were found:

1. **SSI slots paced by a timer chain** (`xc/ssi.c`). Each serial word went
   SSI tick (11 us) -> DMAC timer (10 us) -> SSITDR -> next tick. QEMU
   services timers only between TCG slices (~1 ms here), so a word took
   ~1.6 ms and a 45-word block ~70 ms. MAIN's DEI1/DEI2 handlers
   (0x0C0762F4/0x0C076444) ran late, and DEI2 is what re-enables IRQ7
   (DSPDREQ) after the feeder masks it (IPR02 low nibble, 0x0C076640): the
   DSP got ~5 blocks/s. Fix: the tick shifts every slot elapsed on the
   virtual clock (88.2 kHz, remainder carried) and refills SSITDR with
   `cdj850_dmac_serve()` synchronously. vmstate v4.
2. **MPY/MPYR/MAC/MACR S,#n,D undecoded** (0x0100C0 family). P:07EC
   `0x0108E0` MPY Y0,#8,A builds the index for P:07F7 `R4 = Y:(R4+N4)`
   and P:07FC `JMP (R4)`. Decoded as BSET, A stayed stale, R4 = 0 and the
   DSP jumped to its reset vector on every pass (P:07FC -> P:0000 -> P:0100
   -> P:0EBB..0EDE, stack growing by one each time). Also missing and now
   decoded: MPYsu/MACsu (0x0126/0x0127, P:077C `0x0127A6`), DMAC
   (0x0124/0x0125, P:077D `0x0124A3`) and LUA (`0000 0100 010M MRRR 0001
   dddd`, P:0634 `0x045B14`, P:0704 `0x044A13`; it was decoded as MOVEP).
   Found by sweeping every non-parallel instruction shape in Ghidra's full
   disassembly of segment 4 (2134 instructions) against the decoder; the
   rest match.
3. **Re-presenting a lost interrupt waited for the pump timer**
   (`xc/cmt.c`, `target/sh4`). IRQ7 (level 7) lost to the CMT tick (level
   8, ~450/s); after the CPU took CMT, IRQ7 was presented again only on the
   next 10 us pump timer, i.e. the next slice, then often lost again: ~5 ms
   per block. Fix: a per-CPU hook `env->sh2a_irq_taken`, called by the
   SH-2A acceptance path in `superh_cpu_do_interrupt` right after it clears
   the request and masks SR.IMASK; the board presents the next pending
   vector at once, so it is taken on the handler's RTE. Patches regenerated
   (`target_sh4_cpu.h.patch`, `target_sh4_helper.c.patch`; verify.sh
   clean). After this every DSP request was answered (requests = blocks).
4. **DSP audio tick quantisation** (`dsp56300.c`). The tick ran 44/45
   frames per 1 ms. One 84-frame block request is in flight at a time, MAIN
   can only answer between bursts and a block is 90 serial words (> 88
   slots per tick), so each block took exactly two ticks: 500 blocks/s =
   42000 frames/s, the DSP underran (0.95x pitch, ~2100 discontinuities/s).
   Late ticks also lost frames (~31k frames per virtual second early on).
   Fix: frames are counted from the virtual clock (44100/s, remainder
   carried) and the tick is 0.5 ms.
5. **DSP compute budget.** At the default `CDJ850_DSP_IPS=64` the DSP asks
   for only ~45 blocks/s (8 % of real time); 256 gave 170. Real time needs
   ~750 per slot. Without icount the host cannot sustain that yet (1024 got
   worse: each tick then costs more than its 0.5-1 ms).

Verification (`-icount shift=2,sleep=off`, `CDJ850_DSP_IPS=2048`, script
`...,38000:vinyl:200,42000:play:200`, raw ESAI TX words): MAIN feeds exactly
525 blocks/s; the DSP output is 440 Hz L / 660 Hz R at the fixture level
(11999) with zero discontinuities for the whole of BETA.wav. After the 10 s
track ends, MAIN loops its last 13 sectors (a jump every 7644 frames =
13 x 588) until GAMMA.wav loads: firmware end-of-track behaviour.

Not addressed:
- Real-time playback (no icount) needs a faster DSP core. The interpreter
  does ~46 M instructions/s in a tight loop on the Intel dev Mac, against
  ~66 M needed, and the audio tick runs under the BQL that MAIN's vCPU
  needs. A predecoded-instruction cache is the obvious next step.
- With icount, the host audio sink (`cdj_host_audio.c`) still underruns
  in the first seconds and bends its ASRC to -4 %, so a WAV capture is
  slightly flat at first; the raw ESAI tap is the reference.
- After load the deck plays BETA.wav before the scripted Play press, and the
  load hit "NO DISC" in 3 of ~14 real-time runs (MAIN misses the 20 s `usb`
  press under host load). Both predate these changes.

### Player states re-read: auto-play after load, PLAY pauses; VINYL works (2026-11-17)

Traced with gdbstub reads of MAIN RAM once a second (USB deck, script
`20000:usb,24000:enter,26000:rot+1,28000:enter` plus the presses below).
**This corrects the state labels used in the entries above** (Task "Audio
path: why MAIN never starts SSI DMA", "Follow-up trace", the MAIN play-path
audit and the live test).

- Player state 0x0CE6D25F (0x0CE6D25D + source index 2): 0x0A USB browse,
  2 loading, **3 playing**, **5 paused/braking** (PLAY pressed while
  playing), 4 paused (CDJ-mode pause/CUE paths). Audio command (0x0CE6CA80):
  **1 = play**, **3 = pause**. Audio-task state 0x0C7F6627: 3 playing,
  4 paused (CDJ mode).
- **The deck plays a track as soon as it is loaded.** Load completion
  (0x0C0EAE2A..0x0C0EAE7A) picks state 5 if PLAY was pressed during the load
  (0x0CE6D0F1), 4 if 0x0CE6D0ED, otherwise 3, and sends command (1, 1); the
  audio task starts the stream (`FUN_0C005B14`, PF16 high) once 75 sectors
  are buffered. The read index advances (2 -> 0xF2 over 14 s), the track
  plays to its end and continue mode loads the next file. This is firmware
  behaviour, not an emulator fault. (The load start, `FUN_0C0EA8AE` /
  `FUN_0C0E9E68`, sets sub-state 2 in CDJ mode or untouched VINYL; that is
  the "start at pitch speed with HOLD" path.)
- **VINYL mode (factory default) behaves correctly**, which closes the
  "VINYL release" question: there was no deferred play, the first PLAY press
  after a load is a *pause*. Measured (`34000:play,40000:play`): PLAY at 34 s
  sets 0x0CE6D0F1 = 1, 0x0CE6D0F2 = 1 (brake), sub-state 4; speed
  0x0CE6C940 -> 0, the DSP-reported speed decays 0xC5ECD8 -> 0xC00000 and the
  read index freezes (silent stop). PLAY at 40 s releases: the reported speed
  ramps 0xC3F400 -> 0xC80000 (1.0x) and the index advances again.
- HOLD (0x0CE6C938 bit 15) stays set after a load because the emulated pitch
  ADC never changes (0x0CE6C932 == 0x0CE6C930); with the DSP fixed it does not
  matter: HOLD only makes the DSP apply the speed at once.
- **CDJ mode pause keeps sounding: expected behaviour (confirmed by the
  owner 2026-11-18), not an emulation bug.** With `vinyl` pressed first, PLAY
  gives state 5 / audio state 4 / command 3, but the speed stays 0x080000,
  PF16 stays high and the 6-byte DSP command is identical to playback
  (`e28000 080000`); MAIN rocks the read index around the pause point
  (0x42..0x4E) and the DSP plays that window at full level
  (`CDJ850_DSP_DEBUG` TX peak 0x2EDF00 throughout the pause). Every MAIN port
  data register is unchanged between play and pause, and MAIN sends no panel
  status frames while paused. In the pause handler `FUN_0C0F6E6C` the stream
  stops (`FUN_0C005BB6`, 0x0C0F6F20) only when 0x0CE6D0FB == 1, which is
  never set here. The rocking window around the pause point is the real
  unit's CDJ-mode pause sound, so no panel/hardware mute needs emulating
  for it. (Earlier guess, now dropped: MAIN_XMUTE/PNL_XMUTE on CN2005 muting
  the pause.)
- The DAC routine `FUN_0C016020` only switches between full level
  (0x10FF/0x11FF) and an attenuation (0x10E7/0x11E7) on bit 3 of 0x0CE6CA2F.
- `0x0C1C1460`/`0x0C7F6674` gate the feeder's saved-pointer path: that is the
  loop machinery (`FUN_0C015BD4` builds a four-segment loop, `FUN_0C015B18`
  clears it), not a cue hold. The per-sector table at 0x0C750C3C holds
  `[sector, 0, track number, -1]`; 0x0C7F6484 is the track at the read point
  and 0x0CE5A9E0 the track count.

**Live test flake fixed.** The USB stick mount (display flashes "0 DISC")
landed anywhere from 2.9 s to 26.7 s of virtual time across 33 runs today,
tracking host load (the virtual clock follows the host, so MAIN gets fewer
instructions per virtual second when the host is busy); the two "NO DISC"
failures were the two mounts after the fixed 20 s `usb` press. Under
`-icount shift=2,sleep=off` the mount is at 2875 ms every time.
`test_live_usb_source_select_browse_load_and_play` now runs under icount
with presses at 6/9/11/13 s and checks the corrected behaviour: loaded ->
playing (state 3, read index advancing) -> PLAY brakes to a stop (state 5,
speed 0) -> PLAY restarts (state 3, speed 0x080000). 8/8 passes (5 serial,
3 in parallel), ~24 s each.

`CDJ850_DSP_DEBUG` status lines now print every 500 ms of virtual time and
carry the virtual time (ticks no longer have a fixed length).

### DSP56371 predecoded icache, worker thread & default `256` IPS (2026-11-18)

Added a predecoded instruction cache, hot-path optimizations, and off-BQL
non-`icount` execution in `hw/cdj/boards/cdj850/dsp56300.c`:

1. **Predecoded icache + computed-goto `dsp_exec()`.** `icache[DSP_PRAM_SIZE]`
   caches `DspInsn` descriptors populated by `dsp_icache_fill()` across
   computed-goto `DspOpKind` handlers (including `*_NOALU` parallel-move fast
   paths). `mem_write_p()` invalidates `addr` and `addr - 1` (for 2-word ops),
   `DSP_FETCH` checks `slot->op != d->p[pc]` to catch direct `d->p[]` writes
   (such as SHI segment reloads and `JMP $FF0000` boot-ROM re-entry), and
   2-word instructions read `dsp_ext(d, pc)` dynamically.
2. **Hot-path cleanups.** `CDJ850_DSP_DEBUG` and `CDJ850_DSP_PLAY_PF` are now
   cached once in `dsp56300_init()`. `agu_add()` adds a power-of-2 modulo fast
   path (`(m & (m + 1)) == 0`), `eval_ea()` reads `d->n[rrr]` only for modes
   `0, 1, 5`, `read_acc24_limited()`/`set_ccr56()`/`set_ccr_logic()` are marked
   `always_inline`, and `.cold`-inducing `__builtin_expect` hints were removed.
3. **Stock segment-4 cadence & non-`icount` worker thread.** Disassembly of
   stock `dsp_seg4_app.bin` shows zero `WAIT`/`STOP` instructions, so the DSP
   continuously spends ~750 insns/slot (~66.2 M IPS). Running `CDJ850_DSP_IPS=750`
   on the synchronous BQL tick starved MAIN (~413 ms/tick catch-up spiral). When
   `!icount_enabled()`, execution now runs on `dsp_worker_thread()` in chunks
   (`DSP_WORKER_CHUNK = 22`), paced against `QEMU_CLOCK_VIRTUAL`:
   - `QemuMutex lock` + `host_waiting` (`dsp_host_lock()`/`dsp_host_unlock()`)
     guard SHI transactions and VM state/exit hooks (`dsp_vm_state_cb`,
     `dsp_exit_cb`).
   - `rx_lock` protects `dsp56300_esai_rx_push()` (`xc/ssi.c`).
   - `irq_lock` + `irq7_q[32]` + `dsp_irq_bh()` deliver `cdj850_intc_set_pin(7,
     lvl)` under the BQL.
   - `main_pfdr` mirrors PF16 atomically from `cdj850_port_write()` via
     `dsp56300_set_pf()` (`dsp56300.h`, `xc/port.c`).
   Under `-icount`, synchronous `dsp_audio_tick()` lockstep is preserved unchanged.
4. **Default `CDJ850_DSP_IPS` raised `64 -> 256`.** Provides ~4x faster DSP
   block feeding (~22.6 M IPS) in default interactive runs with headroom against
   thermal throttling; `-icount shift=2,sleep=off` + `CDJ850_DSP_IPS=2048`
   remains the deterministic firmware-testing mode.

Benchmarks & verification:
- Standalone benchmarks (`bench_dsp`, `bench_seg4`): pre-final constant tweak
  at unthrottled turbo (~3.2–3.5 GHz) reached `295.24 M IPS` (`tight_nop_jmp`, vs
  `131.36` baseline), `57.06 M IPS` (`mac_xy_modulo_loop`, vs `42.31`),
  `141.63 M IPS` (`mixed_control_alu`, vs `57.99`), and `94.67 M IPS` / `349.4 us`
  per 500 us tick on `seg4` (vs `65.22 M IPS` / `507.1 us` baseline). Post-edit
  under host thermal cap (`1.90 GHz`): `157.13`, `47.11`, `85.82`, and
  `53.22 M IPS` (`621.5 us/tick`).
- Live non-`icount` playback at `CDJ850_DSP_IPS=750` (throttled `1.90 GHz` host):
  MAIN booted, mounted USB, and played `BETA.wav` at 1:1 virtual/wall-clock pace
  without starving MAIN (`1073008` WAV frames / `24.33 s @ 44100 Hz`, mild
  underrun gaps and `-4%` ASRC bend when pinned to 750 below 66.2 M IPS).
  `CDJ850_DSP_IPS=256` runs 1:1 (`30.68 s` wall / `30.58 s` virtual) with ample
  host headroom (at the ~1/3 DSP block budget).
- Regression tests (`tests/test_cdj850_dsp.py` incl. 3 new P-RAM icache
  invalidation / `JMP $FF0000` reload tests, plus `panel_seq`, `vfd`): `15 passed`
  (`3.59 s`); `tests/test_cdj850_usb.py` (`-icount` + default `256` IPS):
  `9 passed` (`57.68 s`).


