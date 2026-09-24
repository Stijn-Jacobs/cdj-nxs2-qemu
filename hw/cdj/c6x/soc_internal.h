/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C6655 SoC models -- state shared between the block files. Not a public API.
 */
#ifndef SOC_INTERNAL_H
#define SOC_INTERNAL_H

#include "soc_c6655.h"

#define NS_PER_S 1000000000ull

/* ---- system signals: every peripheral output that routes somewhere -------- */
typedef enum soc_sig {
    SIG_TINT_L0, SIG_TINT_H0,             /* Timer n low/high: SIG_TINT_L0 + 2n */
    SIG_TIMER_LAST = SIG_TINT_L0 + 2 * 8 - 1,
    SIG_UPPINT,
    SIG_SPIINT0, SIG_SPIINT1, SIG_SPIXEVT, SIG_SPIREVT,
    SIG_MCBSP_RINT0, SIG_MCBSP_XINT0, SIG_MCBSP_REVT0, SIG_MCBSP_XEVT0,
    SIG_MCBSP_RINT1, SIG_MCBSP_XINT1, SIG_MCBSP_REVT1, SIG_MCBSP_XEVT1,
    SIG_EDMA_ERRINT, SIG_EDMA_GINT,
    SIG_EDMA_INT0,                        /* region n: SIG_EDMA_INT0 + n */
    SIG_EDMA_INT_LAST = SIG_EDMA_INT0 + 7,
    SIG_GPINT0,                           /* pin n: SIG_GPINT0 + n */
    SIG_GPINT_LAST = SIG_GPINT0 + 31,
    SIG_COUNT
} soc_sig;

void soc_signal(c6655_soc *s, soc_sig sig, int level);
void soc_pulse(c6655_soc *s, soc_sig sig);
void soc_log(c6655_soc *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void soc_log_unimp(c6655_soc *s, const char *block, uint32_t addr, int is_write, uint32_t val);

/* DMA access to DSP memory: routes SoC register windows back into the SoC and
 * folds the CorePac global alias onto L2. */
uint32_t soc_dma_read(c6655_soc *s, uint32_t addr, unsigned size);
void     soc_dma_write(c6655_soc *s, uint32_t addr, uint32_t val, unsigned size);

/* Sized access to a 32-bit register at a byte offset (little-endian). */
static inline uint32_t reg_get(uint32_t reg, uint32_t off, unsigned size)
{
    uint32_t v = reg >> (8 * (off & 3));
    return size >= 4 ? v : v & ((1u << (8 * size)) - 1);
}
static inline uint32_t reg_put(uint32_t reg, uint32_t off, uint32_t val, unsigned size)
{
    if (size >= 4 && (off & 3) == 0)
        return val;
    unsigned sh = 8 * (off & 3);
    uint32_t mask = (size >= 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1)) << sh;
    return (reg & ~mask) | ((val << sh) & mask);
}

/* ---- CorePac INTC ---------------------------------------------------------- */
typedef struct soc_intc {
    uint32_t flag[4], mask[4], expmask[4];
    uint32_t intmux[3];                   /* INTMUX1..3 */
    uint32_t aegmux[2];
    uint32_t intxstat, intdmask;
    uint8_t  combiner_level;              /* last EVT0..3 output levels */
} soc_intc;

void     intc_reset(soc_intc *ic);
void     intc_event(c6655_soc *s, unsigned evt, int rising);
uint32_t intc_read(c6655_soc *s, uint32_t off, unsigned size);
void     intc_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size);

/* ---- CIC0 ------------------------------------------------------------------ */
#define CIC_NSYS  208                     /* CH_MAP_REG0..51 */
#define CIC_NHOST 96                      /* ENABLE_HINT_REG0..2 */
typedef struct soc_cic {
    uint32_t control, host_control, global_enable;
    uint32_t raw[7], enable[7];
    uint8_t  level[7 * 32];               /* input line levels */
    uint8_t  chmap[CIC_NSYS];
    uint8_t  hintmap[CIC_NHOST];
    uint32_t hint_enable[3];
    uint8_t  out_level[CIC_NHOST];
} soc_cic;

void     cic_reset(soc_cic *c);
void     cic_input(c6655_soc *s, unsigned sysint, int level);
uint32_t cic_read(c6655_soc *s, uint32_t off, unsigned size);
void     cic_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size);

/* ---- Timer64 --------------------------------------------------------------- */
typedef struct soc_timer_half {
    int      running;
    uint64_t base_ns, base_cnt;           /* count at base_ns */
    uint64_t match_ns;                    /* next period match, UINT64_MAX if none */
} soc_timer_half;

typedef struct soc_timer {
    unsigned index;
    uint32_t emumgt, cnt[2], prd[2], tcr, tgcr, wdtcr, rel[2], cap[2], intctlstat;
    soc_timer_half half[2];               /* 0 = LO (or the 64-bit timer), 1 = HI */
} soc_timer;

void     timer_reset(c6655_soc *s, soc_timer *t, unsigned index);
uint32_t timer_read(c6655_soc *s, soc_timer *t, uint32_t off, unsigned size);
void     timer_write(c6655_soc *s, soc_timer *t, uint32_t off, uint32_t val, unsigned size);
uint64_t timer_next_event(const soc_timer *t);
void     timer_fire(c6655_soc *s, soc_timer *t);

/* ---- uPP ------------------------------------------------------------------- */
typedef struct soc_upp_window {
    uint32_t addr, lines, bytes, lnoffset;
} soc_upp_window;

typedef struct soc_upp {
    uint32_t pcr, dlb, ctl, icr, ivr, tcr, isr, ies, eoi;
    uint32_t id[3], qd[3];
    int      act, pend;
    soc_upp_window cur, next;
    uint32_t line, byte;                  /* progress in cur */
    uint8_t *q;                           /* RX byte queue (ring) */
    size_t   qcap, qhead, qlen;
    size_t   dropped;                     /* ship bytes past the window they completed */
    uint64_t windows_done;
} soc_upp;

void     upp_reset(c6655_soc *s, soc_upp *u);
void     upp_free(soc_upp *u);
uint32_t upp_read(c6655_soc *s, uint32_t off, unsigned size);
void     upp_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size);
void     upp_drain(c6655_soc *s);

/* ---- SPI ------------------------------------------------------------------- */
typedef struct soc_spi {
    uint32_t gcr0, gcr1, int0, lvl, flg, pc0, dat1_ctl, delay, def, fmt[4];
    uint16_t txbuf;
    uint32_t rxbuf;                       /* 16-bit data | status bits 24..30 */
    int      txfull, rxempty, cshold_prev;
    uint64_t done_ns;                     /* UINT64_MAX when idle */
} soc_spi;

void     spi_reset(c6655_soc *s, soc_spi *p);
uint32_t spi_read(c6655_soc *s, uint32_t off, unsigned size);
void     spi_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size);
void     spi_fire(c6655_soc *s);

/* ---- McBSP ----------------------------------------------------------------- */
typedef struct soc_mcbsp {
    unsigned port;
    uint32_t drr, dxr, spcr, rcr, xcr, srgr, mcr, pcr, rce[4], xce[4];
    int      dxr_fresh;                   /* DXR written since the last word */
    uint64_t start_ns, start_words;       /* rate anchor: words counted since */
    uint64_t next_word_ns;                /* UINT64_MAX when the clock is off */
    uint64_t underruns, words;
} soc_mcbsp;

void     mcbsp_reset(c6655_soc *s, soc_mcbsp *m, unsigned port);
uint32_t mcbsp_read(c6655_soc *s, soc_mcbsp *m, uint32_t off, unsigned size);
void     mcbsp_write(c6655_soc *s, soc_mcbsp *m, uint32_t off, uint32_t val, unsigned size);
void     mcbsp_fire(c6655_soc *s, soc_mcbsp *m);
uint64_t mcbsp_word_ns(const c6655_soc *s, const soc_mcbsp *m, uint64_t k);
/* Longest run of McBSP words clocked out without returning to the scheduler. */
#define MCBSP_BATCH_WORDS 4096
uint64_t edma_events_until_irq(const c6655_soc *s, unsigned ch, uint64_t limit);

/* ---- EDMA3 CC -------------------------------------------------------------- */
#define EDMA_PARAM_BASE  0x4000
#define EDMA_PARAM_SETS  1024
#define EDMA_PARAM_END   (EDMA_PARAM_BASE + 32 * EDMA_PARAM_SETS)

typedef struct soc_edma {
    uint8_t  cfg[0x1000];                 /* 0x000..0xFFF, RAM-backed config */
    uint64_t er, eer, ier, ipr, ser, cer, emr;
    uint8_t  qer, qeer, qser, qemr;
    uint8_t  level[64];                   /* event input levels */
    uint8_t  param[32 * EDMA_PARAM_SETS];
    int      depth;                       /* chaining recursion guard */
    uint64_t null_transfers;              /* events on exhausted sets */
} soc_edma;

void     edma_reset(soc_edma *e);
void     edma_event(c6655_soc *s, unsigned ch, int level);
uint32_t edma_read(c6655_soc *s, uint32_t off, unsigned size);
void     edma_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size);

/* ---- GPIO ------------------------------------------------------------------ */
typedef struct soc_gpio {
    uint32_t pcr, binten, dir, out, ris, fal;
    uint32_t ext;                         /* external input levels */
    uint32_t last_pins;                   /* for edge detection */
} soc_gpio;

/* ---- the SoC --------------------------------------------------------------- */
typedef struct soc_ram_region {
    uint32_t base, size;
    uint8_t *mem;
    const char *name;
} soc_ram_region;

struct c6655_soc {
    c6655_soc_config cfg;
    uint64_t now_ns;

    uint8_t  sig_level[SIG_COUNT];

    soc_intc  intc;
    soc_cic   cic0;
    soc_timer timer[8];
    soc_upp   upp;
    soc_spi   spi;
    soc_mcbsp mcbsp[2];
    soc_edma  edma;
    soc_gpio  gpio;

    soc_ram_region ram[8];
    unsigned       nram;

    /* next_event_ns() result, valid until a register write or a fired event */
    uint64_t next_cache;
    int      cache_valid;
    int      in_catchup;                  /* EDMA3 moving McBSP words: no re-entry */
    uint64_t stat[6];                     /* advance calls, fast path, event loops, next_event computes, words, catch-ups */

    uint32_t logged[512];                 /* addresses already reported */
    unsigned nlogged;
};

#endif
