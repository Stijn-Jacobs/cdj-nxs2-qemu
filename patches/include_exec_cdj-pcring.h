/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Ring buffer of recently-entered translation blocks, with the SR each was
 * entered under.
 *
 * The guest turns a BL-masked exception into a machine reset inside
 * superh_cpu_do_interrupt(), which returns before any of QEMU's exception
 * logging runs (so "-d int" sees nothing), and a full "-d exec" over the
 * seconds of boot before a reset is far too large to keep. SR is recorded so
 * the history shows where SR.BL was set, not just the PCs.
 *
 * This records translation-block chain heads: QEMU chains blocks and executes
 * a whole chain inside one tcg_qemu_tb_exec() call. Pass "-d nochain" when a
 * complete trace is needed.
 */
#ifndef CDJ_PCRING_H
#define CDJ_PCRING_H

/* Power of two: the index is masked, never wrapped. */
#define CDJ_PC_RING_LEN 65536

typedef struct CdjPcRingEntry {
    uint64_t pc;
    uint32_t sr;
} CdjPcRingEntry;

extern CdjPcRingEntry cdj_pc_ring[CDJ_PC_RING_LEN];
extern unsigned cdj_pc_ring_idx;

/* SR.BL: exceptions blocked. */
#define CDJ_SR_BL_BIT 28

/*
 * accel/tcg is built once for every target, so what the ring and the
 * CDJ_JCPROF jump-cache profile read from the guest CPU's state comes from the
 * target through these hooks, set when its CPU is realized.
 */
#define CDJ_EXIT_KINDS_MAX 16

typedef struct CdjTcgHooks {
    /* SR as the ring records it. */
    uint32_t (*ring_sr)(CPUState *cpu);
    /* Why the last TB was left, as stored by the translator; resets it. */
    unsigned (*take_exit_kind)(CPUState *cpu);
    const char *const *exit_kind_names;
    unsigned n_exit_kinds;                  /* at most CDJ_EXIT_KINDS_MAX */
    /* Appends the target's own counters to a jcprof report line. */
    void (*jcprof_report)(CPUState *cpu);
} CdjTcgHooks;

extern const CdjTcgHooks *cdj_tcg_hooks;

#endif /* CDJ_PCRING_H */
