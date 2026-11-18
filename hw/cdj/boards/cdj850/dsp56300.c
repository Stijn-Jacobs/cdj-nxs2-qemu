/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_host_audio.h"
#include "qemu/timer.h"
#ifndef DSP_TEST_COMMON
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/runstate.h"
#endif
#include "dsp56300.h"
#include "xc/intc.h"
#include "xc/port.h"
#include "xc/trace.h"

/*
 * DSP56371 (DSP56300 family) interpreter + peripherals.
 *
 * Memory map:
 *   P:$000000..$00FFFF  Program RAM (64K x 24-bit)
 *   P:$FF0000           Bootstrap ROM entry (boot mode 5: SHI slave SPI)
 *   X:$000000..$01FFFF  X Data RAM (128K x 24-bit)
 *   X:$FFFF80..$FFFFFF  On-chip peripherals (SHI, ESAI, PLL, INTC, ...)
 *   Y:$000000..$01FFFF  Y Data RAM (128K x 24-bit)
 *   Y:$FFFF80..$FFFFFF  On-chip peripherals (ESAI_1, Port E GPIO, Timer, ...)
 */

#define DSP_PRAM_SIZE   0x10000u
#define DSP_DRAM_SIZE   0x20000u
#define DSP_IO_BASE     0xFFFF80u
#define DSP_IO_SIZE     0x80u
#define DSP_BOOT_ROM    0xFF0000u
#define DSP_MASK24      0xFFFFFFu
#define DSP_MASK56      0x00FFFFFFFFFFFFFFULL

/* SR bits */
#define SR_C            (1u << 0)
#define SR_V            (1u << 1)
#define SR_Z            (1u << 2)
#define SR_N            (1u << 3)
#define SR_U            (1u << 4)
#define SR_E            (1u << 5)
#define SR_L            (1u << 6)
#define DSP_SR_S        (1u << 7)
#define DSP_SR_I0       (1u << 8)
#define DSP_SR_I1       (1u << 9)
#define SR_IMASK_SHIFT  8
#define SR_S0           (1u << 10)
#define SR_S1           (1u << 11)
#define SR_LF           (1u << 15)
#define SR_FV           (1u << 16)

/* Peripheral addresses (6-bit offset from 0xFFFF80) */
#define IO_OFF(a)       ((a) - DSP_IO_BASE)
/* X peripherals */
#define XIO_HCSR        IO_OFF(0xFFFF91)
#define XIO_HTX         IO_OFF(0xFFFF93)
#define XIO_HRX         IO_OFF(0xFFFF94)
#define XIO_ESAI_TX0    IO_OFF(0xFFFFA0)
#define XIO_ESAI_RX0    IO_OFF(0xFFFFA8)
#define XIO_ESAI_RX1    IO_OFF(0xFFFFA9)
#define XIO_ESAI_RX2    IO_OFF(0xFFFFAA)
#define XIO_ESAI_RX3    IO_OFF(0xFFFFAB)
#define XIO_ESAI_SAISR  IO_OFF(0xFFFFB3)
#define XIO_ESAI_TCR    IO_OFF(0xFFFFB5)
#define XIO_ESAI_TCCR   IO_OFF(0xFFFFB6)
#define XIO_ESAI_RCR    IO_OFF(0xFFFFB7)
#define XIO_ESAI_RCCR   IO_OFF(0xFFFFB8)
/* Y peripherals */
#define YIO_PEDR        IO_OFF(0xFFFF9D)
#define YIO_PEDDR       IO_OFF(0xFFFF9E)
#define YIO_PECR        IO_OFF(0xFFFF9F)
#define YIO_PDRF        IO_OFF(0xFFFFF5)

#define HCSR_HEN        (1u << 0)
#define HCSR_HRNE       (1u << 17)
#define HCSR_HTDE       (1u << 15)

#define SAISR_RFS       (1u << 6)
#define SAISR_RDF       (1u << 8)
#define SAISR_TFS       (1u << 13)
#define SAISR_TDE       (1u << 15)

#define FIFO_CAP        256u

typedef struct {
    uint32_t buf[FIFO_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} DspFifo;

static void fifo_push(DspFifo *f, uint32_t v)
{
    if (f->count >= FIFO_CAP) {
        f->head = (f->head + 1) % FIFO_CAP;
        f->count--;
    }
    f->buf[f->tail] = v & DSP_MASK24;
    f->tail = (f->tail + 1) % FIFO_CAP;
    f->count++;
}

static bool fifo_pop(DspFifo *f, uint32_t *out)
{
    if (f->count == 0) {
        return false;
    }
    *out = f->buf[f->head];
    f->head = (f->head + 1) % FIFO_CAP;
    f->count--;
    return true;
}

typedef enum {
    DSP_OP_INVALID = 0,
    DSP_OP_NOP,
    DSP_OP_RTI,
    DSP_OP_INC_DEC,
    DSP_OP_RTS,
    DSP_OP_DO_FOREVER,
    DSP_OP_ENDDO,
    DSP_OP_STOP,
    DSP_OP_WAIT,
    DSP_OP_ANDI,
    DSP_OP_ORI,
    DSP_OP_SHIFT56,
    DSP_OP_SHIFT24,
    DSP_OP_JMP12,
    DSP_OP_JSR12,
    DSP_OP_JCC12,
    DSP_OP_JSCC12,
    DSP_OP_JMP24,
    DSP_OP_JSR24,
    DSP_OP_JCC24,
    DSP_OP_JSCC24,
    DSP_OP_BRA9,
    DSP_OP_BRA24,
    DSP_OP_LUA,
    DSP_OP_MOVEC_RR,
    DSP_OP_MOVEP_LOW,
    DSP_OP_MOVEC_05_IMM8,
    DSP_OP_MOVEC_05_IMM24,
    DSP_OP_MOVEC_05_MEM,
    DSP_OP_REP,
    DSP_OP_DO,
    DSP_OP_MOVEP_80,
    DSP_OP_MOVEP_MEM,
    DSP_OP_MOVE_DISP7,
    DSP_OP_JMP_EA,
    DSP_OP_JCC_EA,
    DSP_OP_JSR_EA,
    DSP_OP_JSCC_EA,
    DSP_OP_MPY_IMM,
    DSP_OP_DMAC_MPYSU,
    DSP_OP_MOVE_LONGDISP,
    DSP_OP_IMM_ALU_01,
    DSP_OP_BIT_OP,
    DSP_OP_BIT_BRANCH,
    DSP_OP_TCC,
    DSP_OP_PAR_UPDATE_RN,
    DSP_OP_PAR_XY_DUAL,
    DSP_OP_PAR_XY_DUAL_NOALU,
    DSP_OP_PAR_IFCC,
    DSP_OP_PAR_REG_REG,
    DSP_OP_PAR_REG_REG_NOALU,
    DSP_OP_PAR_SHORT_IMM,
    DSP_OP_PAR_SHORT_IMM_NOALU,
    DSP_OP_PAR_CLASS1,
    DSP_OP_PAR_LONG_XY,
    DSP_OP_PAR_IMM24,
    DSP_OP_PAR_EA_MOVE,
    DSP_OP_PAR_EA_NOALU,
    DSP_OP_PAR_AA_MOVE,
    DSP_OP_PAR_AA_NOALU,
    DSP_OP_UNIMP,
} DspOpKind;

typedef struct {
    uint32_t op;
    uint32_t imm;
    uint8_t  kind;
    uint8_t  words;
    uint8_t  alu;
    uint8_t  a;
    uint32_t _pad;
} DspInsn;

typedef struct {
    /* Data ALU */
    union {
        struct {
            uint64_t a, b;  /* 56-bit in low 56 bits */
        };
        uint64_t acc[2];
    };
    union {
        struct {
            uint32_t x0, x1, y0, y1;
        };
        uint32_t xy[4];
    };

    /* AGU */
    uint32_t r[8], n[8], m[8];

    /* PCU */
    uint32_t pc, sr, omr, la, lc, sp, vba, sz, sc, ep;
    uint32_t ssh[16], ssl[16];
    uint32_t rep_pc, rep_remaining, rep_saved_lc;

    bool halted;
    bool yield_requested;
    bool wait_mode;
    bool stop_mode;
    bool in_boot_rom;

    /* Bootstrap mode 5 state */
    enum { BOOT_COUNT, BOOT_ADDR, BOOT_DATA, BOOT_RUN } boot_state;
    uint32_t boot_count, boot_left, boot_addr, boot_entry;
    uint32_t shi_acc;
    unsigned shi_nbytes;
    unsigned boot_segs;

    /* SHI FIFOs */
    DspFifo shi_rx;
    DspFifo shi_tx;
    uint32_t shi_tx_word;
    unsigned shi_tx_bytes_left;

    /* SSI -> ESAI RX FIFOs (0 = SSI0/RX0, 1 = SSI1/RX1) */
    DspFifo ssi_rx[2];

    /* Audio output */
    CdjHostAudio *audio_sink;
    QEMUTimer *audio_timer;
    uint32_t audio_tick;
    int64_t audio_last_ns;      /* virtual time frames were counted up to */
    uint64_t audio_frac;        /* frame fraction, in ns * 44100 units */
    int64_t debug_next_ns;      /* next CDJ850_DSP_DEBUG status line */
    int16_t left_sample;
    bool have_left;
    bool tfs_slot;
    bool esai_tx_written;
    uint8_t esai_rx_pending;
    uint32_t unimp_logged;
    uint64_t esai_tx_words, esai_tx_nonzero, esai_rx_words;
    uint32_t esai_tx_peak;
    uint32_t shi_last_tx, shi_tx_logged;
    /* Instructions run per ESAI slot (88.2 kHz). The DSP56371 manages
     * about 4000; stock firmware needs ~750 (~66.2 MIPS) for full 525 blocks/s
     * real-time audio. 256 (~22.6 MIPS) keeps ample host headroom when running
     * on the non-icount worker thread without slowing -icount tests.
     * $CDJ850_DSP_IPS overrides. */
    unsigned slot_budget;
    bool debug_enabled;
    bool play_pf_override;
    unsigned play_pf_bit;
    bool dspdreq_pin;

    /* Cross-thread state for non-icount worker mode */
    bool threaded;
    uint32_t main_pfdr;         /* atomic mirror: (pfdrh << 16) | pfdrl */
#ifndef DSP_TEST_COMMON
    QemuMutex lock;             /* protects DSP core state & SHI FIFOs */
    QemuMutex rx_lock;          /* leaf lock for ssi_rx[0..1] & esai_rx_words */
    QemuMutex irq_lock;         /* leaf lock for irq7_q */
    QEMUBH *irq_bh;
    uint8_t irq7_q[32];
    unsigned irq7_head, irq7_tail, irq7_count;
    QemuThread thread;
    VMChangeStateEntry *vmstate;
    Notifier exit;
    unsigned host_waiting;
    bool running;
    bool paused;
#endif

    uint32_t p[DSP_PRAM_SIZE];
    DspInsn icache[DSP_PRAM_SIZE];
    uint32_t x[DSP_DRAM_SIZE];
    uint32_t y[DSP_DRAM_SIZE];
    uint32_t x_io[DSP_IO_SIZE];
    uint32_t y_io[DSP_IO_SIZE];
} Dsp56300;

static Dsp56300 *dsp;

static inline int32_t sext24(uint32_t v)
{
    v &= DSP_MASK24;
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

static inline int64_t sext56(uint64_t v)
{
    v &= DSP_MASK56;
    return (v & (1ULL << 55)) ? (int64_t)(v | 0xFF00000000000000ULL)
                              : (int64_t)v;
}

static inline uint64_t pack_acc24(uint32_t v24)
{
    uint64_t ext = (v24 & 0x800000u) ? 0xFFULL : 0x00ULL;

    return ((ext << 48) | ((uint64_t)(v24 & DSP_MASK24) << 24)) & DSP_MASK56;
}

static inline __attribute__((always_inline)) uint32_t
read_acc24_limited(Dsp56300 *d, uint64_t acc)
{
    /* Scaling mode S1:S0 */
    unsigned sm = (d->sr >> 10) & 3;
    int64_t s = sext56(acc);
    int64_t val;

    if (sm == 1) {
        val = s >> 23;
    } else if (sm == 2) {
        val = s >> 25;
    } else {
        val = s >> 24;
    }
    if (val > 0x7FFFFF) {
        d->sr |= SR_L;
        return 0x7FFFFFu;
    }
    if (val < -0x800000) {
        d->sr |= SR_L;
        return 0x800000u;
    }
    return (uint32_t)val & DSP_MASK24;
}

#ifndef DSP_TEST_COMMON
static void dsp_flush_irq7_bql(Dsp56300 *d)
{
    for (;;) {
        uint8_t lvl;

        qemu_mutex_lock(&d->irq_lock);
        if (d->irq7_count == 0) {
            qemu_mutex_unlock(&d->irq_lock);
            break;
        }
        lvl = d->irq7_q[d->irq7_head];
        d->irq7_head = (d->irq7_head + 1) % ARRAY_SIZE(d->irq7_q);
        d->irq7_count--;
        qemu_mutex_unlock(&d->irq_lock);
        cdj850_intc_set_pin(7, lvl != 0);
    }
}

static void dsp_irq_bh(void *opaque)
{
    dsp_flush_irq7_bql(opaque);
}
#endif

static void update_pedr_dspdreq(Dsp56300 *d)
{
    /* Port E bit 4 (PE4) = DSPDREQ -> MAIN IRQ7 when configured as GPIO out */
    uint32_t pedr = d->y_io[YIO_PEDR];
    uint32_t peddr = d->y_io[YIO_PEDDR];
    uint32_t pecr = d->y_io[YIO_PECR];

    if ((peddr & (1u << 4)) && !(pecr & (1u << 4))) {
        bool lvl = (pedr & (1u << 4)) != 0;
        if (d->dspdreq_pin != lvl) {
            if (d->debug_enabled) {
                info_report("cdj850-dsp: DSPDREQ=%d at P:%06x (%" PRIu64 " ms)",
                            lvl, d->pc, cdj850_vms());
            }
            d->dspdreq_pin = lvl;
            /* Let MAIN observe the GPIO edge before another interpreter burst. */
            d->yield_requested = true;
#ifndef DSP_TEST_COMMON
            if (d->threaded && !bql_locked()) {
                qemu_mutex_lock(&d->irq_lock);
                if (d->irq7_count < ARRAY_SIZE(d->irq7_q)) {
                    d->irq7_q[d->irq7_tail] = lvl ? 1 : 0;
                    d->irq7_tail = (d->irq7_tail + 1) % ARRAY_SIZE(d->irq7_q);
                    d->irq7_count++;
                }
                qemu_mutex_unlock(&d->irq_lock);
                qemu_bh_schedule(d->irq_bh);
            } else {
                if (d->threaded) {
                    dsp_flush_irq7_bql(d);
                }
                cdj850_intc_set_pin(7, lvl);
            }
#else
            cdj850_intc_set_pin(7, lvl);
#endif
        }
    }
}

static uint32_t io_read_x(Dsp56300 *d, uint32_t addr)
{
    uint32_t off = IO_OFF(addr);

    if (off >= DSP_IO_SIZE) {
        return 0;
    }
    switch (off) {
    case XIO_HCSR: {
        uint32_t v = d->x_io[off] & ~(HCSR_HTDE | HCSR_HRNE);

        if (!d->shi_tx.count) {
            v |= HCSR_HTDE;
        }

        if (d->shi_rx.count > 0) {
            v |= HCSR_HRNE;
        } else {
            v &= ~HCSR_HRNE;
        }
        return v;
    }
    case XIO_HRX: {
        uint32_t w = 0;

        fifo_pop(&d->shi_rx, &w);
        return w;
    }
    case XIO_ESAI_RX0: case XIO_ESAI_RX1:
    case XIO_ESAI_RX2: case XIO_ESAI_RX3:
        d->esai_rx_pending &= ~(1u << (off - XIO_ESAI_RX0));
        if (!d->esai_rx_pending) {
            d->x_io[XIO_ESAI_SAISR] &= ~SAISR_RDF;
        }
        return d->x_io[off];
    case XIO_ESAI_SAISR: {
        uint32_t v = d->x_io[off];
        if (!d->esai_tx_written) {
            v |= SAISR_TDE;
        } else {
            v &= ~SAISR_TDE;
        }

        if (d->tfs_slot) {
            v |= SAISR_TFS;
        } else {
            v &= ~SAISR_TFS;
        }
        return v;
    }
    default:
        return d->x_io[off];
    }
}

static void io_write_x(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    uint32_t off = IO_OFF(addr);

    if (off >= DSP_IO_SIZE) {
        return;
    }
    val &= DSP_MASK24;
    d->x_io[off] = val;
    switch (off) {
    case XIO_HTX:
        if (d->debug_enabled && d->shi_tx_logged < 40 &&
            (!d->shi_tx_logged || d->shi_last_tx != val)) {
            info_report("cdj850-dsp: SHI TX 0x%06x at P:%06x (%" PRIu64 " ms)",
                        val, d->pc, cdj850_vms());
            d->shi_last_tx = val;
            d->shi_tx_logged++;
        }
        fifo_push(&d->shi_tx, val);
        break;
    case XIO_ESAI_TX0:
        d->esai_tx_words++;
        d->esai_tx_nonzero += val != 0;
        {
            int32_t sv = sext24(val);
            uint32_t mag = (uint32_t)(sv < 0 ? -sv : sv);

            if (mag > d->esai_tx_peak) {
                d->esai_tx_peak = mag;
            }
        }
        d->esai_tx_written = true;
        break;
    default:
        break;
    }
}

static uint32_t io_read_y(Dsp56300 *d, uint32_t addr)
{
    uint32_t off = IO_OFF(addr);

    if (off >= DSP_IO_SIZE) {
        return 0;
    }
    if (off == YIO_PDRF) {
        /* Service manual 10.2/10.4: MAIN PF16 (PFDRH bit 0) -> PLAY -> DSP PF10.
         * PF0..9 are outputs (PRRF = 0x3ff); PF10 is the PLAY input.
         */
#ifdef DSP_TEST_COMMON
        bool play = d->play_pf_override
            ? ((cdj850_port_pfdrl() & (1u << d->play_pf_bit)) != 0)
            : ((cdj850_port_pfdrh() & 1u) != 0);
#else
        uint32_t pf = qatomic_read(&d->main_pfdr);
        bool play = d->play_pf_override
            ? ((pf & (1u << d->play_pf_bit)) != 0)
            : (((pf >> 16) & 1u) != 0);
#endif

        return (d->y_io[off] & ~(1u << 10)) | (play ? 1u << 10 : 0);
    }
    return d->y_io[off];
}

static void io_write_y(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    uint32_t off = IO_OFF(addr);

    if (off >= DSP_IO_SIZE) {
        return;
    }
    d->y_io[off] = val & DSP_MASK24;
    if (off == YIO_PEDR || off == YIO_PEDDR || off == YIO_PECR) {
        update_pedr_dspdreq(d);
    }
}

static inline __attribute__((always_inline)) uint32_t
mem_read_x(Dsp56300 *d, uint32_t addr)
{
    addr &= DSP_MASK24;
    if (__builtin_expect(addr < DSP_DRAM_SIZE, 1)) {
        return d->x[addr];
    }
    return addr >= DSP_IO_BASE ? io_read_x(d, addr) : 0;
}

static inline __attribute__((always_inline)) void
mem_write_x(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    addr &= DSP_MASK24;
    if (__builtin_expect(addr < DSP_DRAM_SIZE, 1)) {
        d->x[addr] = val & DSP_MASK24;
        return;
    }
    if (addr >= DSP_IO_BASE) {
        io_write_x(d, addr, val);
    }
}

static inline __attribute__((always_inline)) uint32_t
mem_read_y(Dsp56300 *d, uint32_t addr)
{
    addr &= DSP_MASK24;
    if (__builtin_expect(addr < DSP_DRAM_SIZE, 1)) {
        return d->y[addr];
    }
    return addr >= DSP_IO_BASE ? io_read_y(d, addr) : 0;
}

static inline __attribute__((always_inline)) void
mem_write_y(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    addr &= DSP_MASK24;
    if (__builtin_expect(addr < DSP_DRAM_SIZE, 1)) {
        d->y[addr] = val & DSP_MASK24;
        return;
    }
    if (addr >= DSP_IO_BASE) {
        io_write_y(d, addr, val);
    }
}

static inline uint32_t mem_read_p(Dsp56300 *d, uint32_t addr)
{
    addr &= DSP_MASK24;
    return addr < DSP_PRAM_SIZE ? d->p[addr] : 0;
}

static inline void mem_write_p(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    addr &= DSP_MASK24;
    if (addr < DSP_PRAM_SIZE) {
        d->p[addr] = val & DSP_MASK24;
        d->icache[addr].kind = DSP_OP_INVALID;
        if (addr > 0) {
            d->icache[addr - 1].kind = DSP_OP_INVALID;
        }
    }
}

/* Stack push / pop (SP bits 3:0 = index 0..15) */
static inline void stack_push(Dsp56300 *d, uint32_t h, uint32_t l)
{
    unsigned idx = ((d->sp & 0xF) + 1) & 0xF;

    d->sp = (d->sp & ~0xFu) | idx;
    d->ssh[idx] = h & DSP_MASK24;
    d->ssl[idx] = l & DSP_MASK24;
}

static inline void stack_pop(Dsp56300 *d, uint32_t *h, uint32_t *l)
{
    unsigned idx = d->sp & 0xF;

    if (h) {
        *h = d->ssh[idx];
    }
    if (l) {
        *l = d->ssl[idx];
    }
    d->sp = (d->sp & ~0xFu) | ((idx - 1) & 0xF);
}

/* AGU address register update with modulo support */
static inline __attribute__((always_inline)) uint32_t
agu_add(Dsp56300 *d, unsigned reg, int32_t delta)
{
    uint32_t r = d->r[reg] & DSP_MASK24;
    uint32_t m = d->m[reg] & DSP_MASK24;

    if (m == DSP_MASK24 || m >= 0x8000u) {
        return (r + (uint32_t)delta) & DSP_MASK24;
    }
    if ((m & (m + 1)) == 0) {
        return ((r & ~m) | ((r + (uint32_t)delta) & m)) & DSP_MASK24;
    }
    /* Modulo (m + 1): bit-smear m to get next-power-of-two mask (p2 - 1). */
    uint32_t mod = m + 1;
    uint32_t p2_mask = m | (m >> 1);
    p2_mask |= p2_mask >> 2;
    p2_mask |= p2_mask >> 4;
    p2_mask |= p2_mask >> 8;
    uint32_t base = r & ~p2_mask;
    int32_t off = (int32_t)(r - base) + delta;

    if (off < 0) {
        off += (int32_t)mod;
        if (off < 0) {
            off = ((off % (int32_t)mod) + (int32_t)mod) % (int32_t)mod;
        }
    } else if (off >= (int32_t)mod) {
        off -= (int32_t)mod;
        if (off >= (int32_t)mod) {
            off %= (int32_t)mod;
        }
    }
    return (base + (uint32_t)off) & DSP_MASK24;
}

/* Compute EA from 6-bit MMM RRR (does not handle mode 6 ext unless passed) */
static inline __attribute__((always_inline)) uint32_t
eval_ea(Dsp56300 *d, unsigned mmmrrr, uint32_t ext,
        bool *used_ext, bool writeback)
{
    unsigned mmm = (mmmrrr >> 3) & 7;
    unsigned rrr = mmmrrr & 7;
    uint32_t addr = d->r[rrr];

    if (used_ext) {
        *used_ext = false;
    }
    switch (mmm) {
    case 0: /* (Rn)-Nn */
        if (writeback) {
            d->r[rrr] = agu_add(d, rrr, -sext24(d->n[rrr]));
        }
        return addr;
    case 1: /* (Rn)+Nn */
        if (writeback) {
            d->r[rrr] = agu_add(d, rrr, sext24(d->n[rrr]));
        }
        return addr;
    case 2: /* (Rn)- */
        if (writeback) {
            d->r[rrr] = agu_add(d, rrr, -1);
        }
        return addr;
    case 3: /* (Rn)+ */
        if (writeback) {
            d->r[rrr] = agu_add(d, rrr, 1);
        }
        return addr;
    case 4: /* (Rn) */
        return addr;
    case 5: /* (Rn+Nn) */
        return agu_add(d, rrr, sext24(d->n[rrr]));
    case 6: /* absolute or immediate */
        if (used_ext) {
            *used_ext = true;
        }
        return ext & DSP_MASK24;
    case 7: /* -(Rn) */
        addr = agu_add(d, rrr, -1);
        if (writeback) {
            d->r[rrr] = addr;
        }
        return addr;
    }
    return addr;
}

/* Condition code evaluation (DSP56300FM Table A-4) */
static inline bool eval_cc(Dsp56300 *d, unsigned cc)
{
    bool c = (d->sr & SR_C) != 0;
    bool v = (d->sr & SR_V) != 0;
    bool z = (d->sr & SR_Z) != 0;
    bool n = (d->sr & SR_N) != 0;
    bool u = (d->sr & SR_U) != 0;
    bool e = (d->sr & SR_E) != 0;
    bool l = (d->sr & SR_L) != 0;

    switch (cc & 0xF) {
    case 0x0: return !c;                 /* CC (HS) */
    case 0x1: return n == v;             /* GE */
    case 0x2: return !z;                 /* NE */
    case 0x3: return !n;                 /* PL */
    case 0x4: return !z && !(!u && !e);  /* NN */
    case 0x5: return !e;                 /* EC */
    case 0x6: return !l;                 /* LC */
    case 0x7: return !z && (n == v);     /* GT */
    case 0x8: return c;                  /* CS (LO) */
    case 0x9: return n != v;             /* LT */
    case 0xA: return z;                  /* EQ */
    case 0xB: return n;                  /* MI */
    case 0xC: return z || (!u && !e);    /* NR */
    case 0xD: return e;                  /* ES */
    case 0xE: return l;                  /* LS */
    case 0xF: return z || (n != v);      /* LE */
    }
    return false;
}

/* Update CCR flags for 56-bit result */
static inline __attribute__((always_inline)) void
set_ccr56(Dsp56300 *d, uint64_t res, bool carry, bool ovf)
{
    uint32_t f = d->sr & ~(SR_C | SR_V | SR_Z | SR_N | SR_U | SR_E);
    uint64_t r56 = res & DSP_MASK56;
    uint32_t ext = (uint32_t)(r56 >> 47) & 0x1FFu; /* bits 55:47 */

    if (carry) {
        f |= SR_C;
    }
    if (ovf) {
        f |= SR_V | SR_L;
    }
    if (r56 == 0) {
        f |= SR_Z;
    }
    if (r56 & (1ULL << 55)) {
        f |= SR_N;
    }
    if (((r56 >> 47) & 1) == ((r56 >> 46) & 1)) {
        f |= SR_U;
    }
    if (ext != 0 && ext != 0x1FFu) {
        f |= SR_E;
    }
    d->sr = f;
}

static inline __attribute__((always_inline)) void
set_ccr_logic(Dsp56300 *d, uint64_t acc)
{
    uint32_t a1 = (uint32_t)((acc >> 24) & DSP_MASK24);
    uint32_t f = d->sr & ~(SR_V | SR_Z | SR_N);

    if (a1 == 0) {
        f |= SR_Z;
    }
    if (a1 & 0x800000u) {
        f |= SR_N;
    }
    d->sr = f;
}

/* 6-bit register table (DDDDDD) for MOVEC / MOVE / TFR / etc. */
static inline uint32_t read_reg6(Dsp56300 *d, unsigned reg)
{
    switch (reg & 0x3F) {
    case 0x04: return d->x0;
    case 0x05: return d->x1;
    case 0x06: return d->y0;
    case 0x07: return d->y1;
    case 0x08: return (uint32_t)(d->a & DSP_MASK24);
    case 0x09: return (uint32_t)(d->b & DSP_MASK24);
    case 0x0A: return (uint32_t)((d->a >> 48) & 0xFFu);
    case 0x0B: return (uint32_t)((d->b >> 48) & 0xFFu);
    case 0x0C: return (uint32_t)((d->a >> 24) & DSP_MASK24);
    case 0x0D: return (uint32_t)((d->b >> 24) & DSP_MASK24);
    case 0x0E: return read_acc24_limited(d, d->a);
    case 0x0F: return read_acc24_limited(d, d->b);
    case 0x10 ... 0x17: return d->r[reg & 7];
    case 0x18 ... 0x1F: return d->n[reg & 7];
    case 0x20 ... 0x27: return d->m[reg & 7];
    case 0x2A: return d->ep;
    case 0x30: return d->vba;
    case 0x31: return d->sc;
    case 0x38: return d->sz;
    case 0x39: return d->sr;
    case 0x3A: return d->omr;
    case 0x3B: return d->sp;
    case 0x3C: {
        uint32_t value;
        stack_pop(d, &value, NULL);
        return value;
    }
    case 0x3D: return d->ssl[d->sp & 0xF];
    case 0x3E: return d->la;
    case 0x3F: return d->lc;
    default: return 0;
    }
}

static inline void write_reg6(Dsp56300 *d, unsigned reg, uint32_t val)
{
    val &= DSP_MASK24;
    switch (reg & 0x3F) {
    case 0x04: d->x0 = val; break;
    case 0x05: d->x1 = val; break;
    case 0x06: d->y0 = val; break;
    case 0x07: d->y1 = val; break;
    case 0x08: d->a = (d->a & ~0xFFFFFFULL) | val; break;
    case 0x09: d->b = (d->b & ~0xFFFFFFULL) | val; break;
    case 0x0A: d->a = (d->a & 0x00FFFFFFFFFFFFULL) | ((uint64_t)(val & 0xFFu) << 48); break;
    case 0x0B: d->b = (d->b & 0x00FFFFFFFFFFFFULL) | ((uint64_t)(val & 0xFFu) << 48); break;
    case 0x0C: d->a = (d->a & ~0xFFFFFF000000ULL) | ((uint64_t)val << 24); break;
    case 0x0D: d->b = (d->b & ~0xFFFFFF000000ULL) | ((uint64_t)val << 24); break;
    case 0x0E: d->a = pack_acc24(val); break;
    case 0x0F: d->b = pack_acc24(val); break;
    case 0x10 ... 0x17: d->r[reg & 7] = val; break;
    case 0x18 ... 0x1F: d->n[reg & 7] = val; break;
    case 0x20 ... 0x27: d->m[reg & 7] = val; break;
    case 0x2A: d->ep = val; break;
    case 0x30: d->vba = val; break;
    case 0x31: d->sc = val; break;
    case 0x38: d->sz = val; break;
    case 0x39: d->sr = val; break;
    case 0x3A: d->omr = val; break;
    case 0x3B: d->sp = val; break;
    case 0x3C:
        d->sp = (d->sp & ~0xFu) | ((d->sp + 1) & 0xFu);
        d->ssh[d->sp & 0xF] = val;
        break;
    case 0x3D: d->ssl[d->sp & 0xF] = val; break;
    case 0x3E: d->la = val; break;
    case 0x3F: d->lc = val; break;
    default: break;
    }
}

/* 5-bit register table (ddddd) for parallel moves */
static inline __attribute__((always_inline)) uint32_t
read_reg5(Dsp56300 *d, unsigned reg5)
{
    reg5 &= 0x1Fu;
    if (reg5 >= 0x10u) {
        return (reg5 < 0x18u) ? d->r[reg5 & 7u] : d->n[reg5 & 7u];
    }
    if (reg5 < 8u) {
        return (reg5 >= 4u) ? d->xy[reg5 - 4u] : 0;
    }
    switch (reg5) {
    case 0x08: case 0x09: return (uint32_t)(d->acc[reg5 & 1u] & DSP_MASK24);
    case 0x0A: case 0x0B: return (uint32_t)((d->acc[reg5 & 1u] >> 48) & 0xFFu);
    case 0x0C: case 0x0D: return (uint32_t)((d->acc[reg5 & 1u] >> 24) & DSP_MASK24);
    default:              return read_acc24_limited(d, d->acc[reg5 & 1u]);
    }
}

static inline __attribute__((always_inline)) void
write_reg5(Dsp56300 *d, unsigned reg5, uint32_t val)
{
    reg5 &= 0x1Fu;
    val &= DSP_MASK24;
    if (reg5 >= 0x10u) {
        if (reg5 < 0x18u) {
            d->r[reg5 & 7u] = val;
        } else {
            d->n[reg5 & 7u] = val;
        }
        return;
    }
    if (reg5 < 8u) {
        if (reg5 >= 4u) {
            d->xy[reg5 - 4u] = val;
        }
        return;
    }
    switch (reg5) {
    case 0x08: case 0x09:
        d->acc[reg5 & 1u] = (d->acc[reg5 & 1u] & ~0xFFFFFFULL) | val;
        break;
    case 0x0A: case 0x0B:
        d->acc[reg5 & 1u] = (d->acc[reg5 & 1u] & 0x00FFFFFFFFFFFFULL) |
                            ((uint64_t)(val & 0xFFu) << 48);
        break;
    case 0x0C: case 0x0D:
        d->acc[reg5 & 1u] = (d->acc[reg5 & 1u] & ~0xFFFFFF000000ULL) |
                            ((uint64_t)val << 24);
        break;
    default:
        d->acc[reg5 & 1u] = pack_acc24(val);
        break;
    }
}

static inline __attribute__((always_inline)) uint32_t
read_xy_reg(Dsp56300 *d, unsigned reg)
{
    if (reg < 8u) {
        return d->xy[reg - 4u];
    }
    return read_acc24_limited(d, d->acc[reg & 1u]);
}

static inline __attribute__((always_inline)) void
write_xy_reg(Dsp56300 *d, unsigned reg, uint32_t val)
{
    val &= DSP_MASK24;
    if (reg < 8u) {
        d->xy[reg - 4u] = val;
        return;
    }
    d->acc[reg & 1u] = pack_acc24(val);
}

/* Execute 8-bit Data ALU MAC/MPY opcode (bits 7:0, bit 7 set) */
static inline __attribute__((always_inline)) void
exec_alu_mac(Dsp56300 *d, uint8_t alu)
{
    /* Multiply / MAC family: 1 QQQ d k sss */
    static const uint8_t qqq_s1[8] = { 0, 2, 1, 3, 0, 2, 1, 3 };
    static const uint8_t qqq_s2[8] = { 0, 2, 0, 2, 3, 0, 2, 1 };
    unsigned qqq = (alu >> 4) & 7u;
    unsigned dst_b = (alu >> 3) & 1u;
    int32_t s1 = ((int32_t)(d->xy[qqq_s1[qqq]] << 8)) >> 8;
    int32_t s2 = ((int32_t)(d->xy[qqq_s2[qqq]] << 8)) >> 8;
    /* Fractional 24x24 -> 48 bit shifted left by 1 */
    int64_t prod = (int64_t)s1 * s2 * 2;
    if (alu & 4u) {
        prod = -prod;
    }
    int64_t acc = (alu & 2u) ? sext56(d->acc[dst_b]) : 0;
    int64_t res = acc + prod;
    if (alu & 1u) {
        /* Convergent rounding at bit 23 */
        uint64_t r = (uint64_t)res;
        if ((r & 0xFFFFFFULL) > 0x800000ULL ||
            ((r & 0xFFFFFFULL) == 0x800000ULL && (r & 0x1000000ULL))) {
            res += 0x1000000LL;
        }
        res &= ~0xFFFFFFLL;
    }
    uint64_t res56 = (uint64_t)res & DSP_MASK56;
    d->acc[dst_b] = res56;
    set_ccr56(d, res56, false, false);
}

/* Execute non-MAC 8-bit Data ALU opcode (0x01..0x7F) */
static __attribute__((noinline)) void exec_alu_other(Dsp56300 *d, uint8_t alu)
{
    /* Non-MAC ALU: 0000 .. 0111 1111 */
    bool dst_b = (alu >> 3) & 1;
    uint64_t *dst = dst_b ? &d->b : &d->a;
    uint64_t *other = dst_b ? &d->a : &d->b;
    unsigned op3 = alu & 7;
    unsigned hi = (alu >> 4) & 7;

    if (hi == 0) {
        /* 00..0F */
        switch (op3) {
        case 0: /* MOVE */ break;
        case 1: { /* TFR */
            *dst = *other;
            break;
        }
        case 2: { /* ADDR */
            int64_t res = (sext56(*dst) >> 1) + sext56(*other);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            break;
        }
        case 3: { /* TST */
            set_ccr56(d, *dst, false, false);
            break;
        }
        case 4: /* Reserved */ break;
        case 5: { /* CMP other, dst */
            int64_t a = sext56(*dst), b = sext56(*other);
            int64_t res = a - b;
            bool c = (uint64_t)*dst < (uint64_t)*other;
            set_ccr56(d, (uint64_t)res, c, false);
            break;
        }
        case 6: { /* SUBR */
            int64_t res = (sext56(*dst) >> 1) - sext56(*other);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            break;
        }
        case 7: { /* CMPM other, dst */
            int64_t a = sext56(*dst), b = sext56(*other);
            if (a < 0) a = -a;
            if (b < 0) b = -b;
            int64_t res = a - b;
            set_ccr56(d, (uint64_t)res, a < b, false);
            break;
        }
        }
        return;
    }

    if (hi == 1) {
        /* 10..1F */
        switch (op3) {
        case 0: /* ADD other, dst */ {
            int64_t res = sext56(*dst) + sext56(*other);
            bool c = ((*dst + *other) >> 56) != 0;
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, c, false);
            break;
        }
        case 1: /* RND */ {
            uint64_t r = *dst;
            if ((r & 0xFFFFFFULL) > 0x800000ULL ||
                ((r & 0xFFFFFFULL) == 0x800000ULL && (r & 0x1000000ULL))) {
                r += 0x1000000ULL;
            }
            *dst = r & ~0xFFFFFFULL & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            break;
        }
        case 2: /* ADDL */ {
            int64_t res = sext56(*dst) * 2 + sext56(*other);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            break;
        }
        case 3: /* CLR */ {
            *dst = 0;
            set_ccr56(d, 0, false, false);
            break;
        }
        case 4: /* SUB other, dst */ {
            int64_t res = sext56(*dst) - sext56(*other);
            bool c = *dst < *other;
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, c, false);
            break;
        }
        case 5: /* MAX (A, B -> B) or MAXM */ {
            /* 0x15: MAXM A, B; 0x1D: MAX A, B */
            int64_t va = sext56(d->a), vb = sext56(d->b);
            if (!dst_b) {
                if (va < 0) va = -va;
                if (vb < 0) vb = -vb;
            }
            if (va >= vb) {
                d->b = d->a;
                d->sr &= ~SR_C;
            } else {
                d->sr |= SR_C;
            }
            break;
        }
        case 6: /* SUBL */ {
            int64_t res = sext56(*dst) * 2 - sext56(*other);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            break;
        }
        case 7: /* NOT */ {
            uint64_t a1 = (~(*dst >> 24)) & DSP_MASK24;
            *dst = (*dst & ~0xFFFFFF000000ULL) | (a1 << 24);
            set_ccr_logic(d, *dst);
            break;
        }
        }
        return;
    }

    if (hi == 2 || hi == 3) {
        /* Decode 0x20..0x3F per DSP56300FM Table A-2 */
        switch (alu) {
        case 0x22: case 0x2A: {
            bool c = (*dst & 1) != 0;
            *dst = (uint64_t)(sext56(*dst) >> 1) & DSP_MASK56;
            set_ccr56(d, *dst, c, false);
            return;
        }
        case 0x23: case 0x2B: {
            uint32_t a1 = (uint32_t)((*dst >> 24) & DSP_MASK24);
            bool c = (a1 & 1) != 0;
            a1 >>= 1;
            *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
            set_ccr_logic(d, *dst);
            if (c) d->sr |= SR_C; else d->sr &= ~SR_C;
            return;
        }
        case 0x26: case 0x2E: {
            int64_t v = sext56(*dst);
            if (v < 0) v = -v;
            *dst = (uint64_t)v & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x32: case 0x3A: {
            bool c = ((*dst >> 55) & 1) != 0;
            *dst = (*dst << 1) & DSP_MASK56;
            set_ccr56(d, *dst, c, false);
            return;
        }
        case 0x33: case 0x3B: {
            uint32_t a1 = (uint32_t)((*dst >> 24) & DSP_MASK24);
            bool c = (a1 >> 23) & 1;
            a1 = (a1 << 1) & DSP_MASK24;
            *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
            set_ccr_logic(d, *dst);
            if (c) d->sr |= SR_C; else d->sr &= ~SR_C;
            return;
        }
        case 0x27: case 0x2F:   /* ROR: C -> bit 47, bit 24 -> C */
        case 0x37: case 0x3F: { /* ROL: C -> bit 24, bit 47 -> C */
            uint32_t a1 = (uint32_t)((*dst >> 24) & DSP_MASK24);
            bool cin = (d->sr & SR_C) != 0;
            bool c;
            if (alu & 0x10) {
                c = (a1 >> 23) & 1;
                a1 = ((a1 << 1) | cin) & DSP_MASK24;
            } else {
                c = a1 & 1;
                a1 = (a1 >> 1) | (cin ? 0x800000u : 0);
            }
            /* A2 and A0 are unchanged; N/Z come from A1, V is cleared. */
            *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
            set_ccr_logic(d, *dst);
            if (c) d->sr |= SR_C; else d->sr &= ~SR_C;
            return;
        }
        case 0x36: case 0x3E: {
            int64_t v = -sext56(*dst);
            *dst = (uint64_t)v & DSP_MASK56;
            set_ccr56(d, *dst, v != 0, false);
            return;
        }
        case 0x20: case 0x28: { /* ADD X (56-bit: X1:X0) to A/B */
            uint64_t x56 = (((uint64_t)sext24(d->x1) << 24) | d->x0) & DSP_MASK56;
            int64_t res = sext56(*dst) + sext56(x56);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x24: case 0x2C: { /* SUB X (56-bit) */
            uint64_t x56 = (((uint64_t)sext24(d->x1) << 24) | d->x0) & DSP_MASK56;
            int64_t res = sext56(*dst) - sext56(x56);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x30: case 0x38: { /* ADD Y (56-bit: Y1:Y0) */
            uint64_t y56 = (((uint64_t)sext24(d->y1) << 24) | d->y0) & DSP_MASK56;
            int64_t res = sext56(*dst) + sext56(y56);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x34: case 0x3C: { /* SUB Y (56-bit) */
            uint64_t y56 = (((uint64_t)sext24(d->y1) << 24) | d->y0) & DSP_MASK56;
            int64_t res = sext56(*dst) - sext56(y56);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x21: case 0x29: { /* ADC X */
            uint64_t x56 = (((uint64_t)sext24(d->x1) << 24) | d->x0) & DSP_MASK56;
            int64_t res = sext56(*dst) + sext56(x56) + ((d->sr & SR_C) ? 1 : 0);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x25: case 0x2D: { /* SBC X */
            uint64_t x56 = (((uint64_t)sext24(d->x1) << 24) | d->x0) & DSP_MASK56;
            int64_t res = sext56(*dst) - sext56(x56) - ((d->sr & SR_C) ? 1 : 0);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x31: case 0x39: { /* ADC Y */
            uint64_t y56 = (((uint64_t)sext24(d->y1) << 24) | d->y0) & DSP_MASK56;
            int64_t res = sext56(*dst) + sext56(y56) + ((d->sr & SR_C) ? 1 : 0);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        case 0x35: case 0x3D: { /* SBC Y */
            uint64_t y56 = (((uint64_t)sext24(d->y1) << 24) | d->y0) & DSP_MASK56;
            int64_t res = sext56(*dst) - sext56(y56) - ((d->sr & SR_C) ? 1 : 0);
            *dst = (uint64_t)res & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
            return;
        }
        }
        return;
    }

    /* 0x40..0x7F: JJ = (alu >> 4) & 3 -> 0=X0, 1=Y0, 2=X1, 3=Y1 */
    uint32_t src24 = 0;
    switch ((alu >> 4) & 3) {
    case 0: src24 = d->x0; break;
    case 1: src24 = d->y0; break;
    case 2: src24 = d->x1; break;
    case 3: src24 = d->y1; break;
    }
    uint64_t src56 = pack_acc24(src24);

    switch (op3) {
    case 0: { /* ADD S, D */
        int64_t res = sext56(*dst) + sext56(src56);
        bool c = ((*dst + src56) >> 56) != 0;
        *dst = (uint64_t)res & DSP_MASK56;
        set_ccr56(d, *dst, c, false);
        break;
    }
    case 1: { /* TFR S, D */
        *dst = src56;
        break;
    }
    case 2: { /* OR S, D1 */
        uint32_t a1 = ((uint32_t)(*dst >> 24) | src24) & DSP_MASK24;
        *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
        set_ccr_logic(d, *dst);
        break;
    }
    case 3: { /* EOR S, D1 */
        uint32_t a1 = ((uint32_t)(*dst >> 24) ^ src24) & DSP_MASK24;
        *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
        set_ccr_logic(d, *dst);
        break;
    }
    case 4: { /* SUB S, D */
        int64_t res = sext56(*dst) - sext56(src56);
        bool c = *dst < src56;
        *dst = (uint64_t)res & DSP_MASK56;
        set_ccr56(d, *dst, c, false);
        break;
    }
    case 5: { /* CMP S, D */
        int64_t res = sext56(*dst) - sext56(src56);
        set_ccr56(d, (uint64_t)res, *dst < src56, false);
        break;
    }
    case 6: { /* AND S, D1 */
        uint32_t a1 = ((uint32_t)(*dst >> 24) & src24) & DSP_MASK24;
        *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
        set_ccr_logic(d, *dst);
        break;
    }
    case 7: { /* CMPM S, D */
        int64_t a = sext56(*dst), b = sext56(src56);
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        set_ccr56(d, (uint64_t)(a - b), a < b, false);
        break;
    }
    }
}

static inline __attribute__((always_inline)) void
exec_alu(Dsp56300 *d, uint8_t alu)
{
    if (alu == 0x00) {
        return;
    }
    if (alu & 0x80u) {
        exec_alu_mac(d, alu);
    } else {
        exec_alu_other(d, alu);
    }
}

/* Decode a 24-bit instruction into DspInsn (extension word is read dynamically). */
static inline uint32_t dsp_ext(const Dsp56300 *d, uint32_t pc)
{
    return (pc + 1 < DSP_PRAM_SIZE) ? d->p[pc + 1] : 0;
}

static void dsp_decode_insn(DspInsn *ins, uint32_t pc, uint32_t op)
{
    ins->op = op;
    ins->imm = 0;
    ins->alu = 0;
    ins->a = 0;
    ins->words = 1;

    /* Parallel move instructions (bits 23:20 != 0) */
    if ((op >> 20) != 0) {
        uint8_t alu = op & 0xFFu;
        ins->alu = alu;

        if ((op & 0xFFC000u) == 0x204000u) {
            /* Update (Rn) parallel move: 0010 0000 010m mrrr */
            ins->kind = DSP_OP_PAR_UPDATE_RN;
            ins->a = (op >> 8) & 0x3Fu;
            return;
        }

        uint32_t mv = (op >> 8) & 0xFFFFu;
        if (mv & 0x8000u) {
            /* X:Y encoding: 1 w mm ee ff W rr MM RRR (FM 13-129). */
            static const uint8_t xregs[] = { 4, 5, 14, 15 };
            static const uint8_t yregs[] = { 6, 7, 14, 15 };
            unsigned xr = xregs[(mv >> 10) & 3];
            unsigned yr = yregs[(mv >> 8) & 3];
            unsigned xn = mv & 7;
            unsigned yn = ((mv >> 5) & 3) | ((xn & 4) ^ 4);
            unsigned xm = (mv >> 3) & 3;
            unsigned ym = (mv >> 12) & 3;
            unsigned rx = (mv >> 7) & 1;
            unsigned ry = (mv >> 14) & 1;

            ins->kind = alu ? DSP_OP_PAR_XY_DUAL : DSP_OP_PAR_XY_DUAL_NOALU;
            ins->imm = xr | (yr << 4) | (xn << 8) | (yn << 11) |
                       (xm << 14) | (ym << 16) | (rx << 18) | (ry << 19);
            return;
        }

        unsigned top4 = (op >> 20) & 0xF;
        if (top4 == 2 || top4 == 3) {
            if (mv >= 0x2020u && mv <= 0x203Fu) {
                ins->kind = DSP_OP_PAR_IFCC;
                ins->a = mv & 0x1Fu;
                return;
            }
            if ((mv & 0xFC00u) <= 0x22FFu) {
                ins->kind = alu ? DSP_OP_PAR_REG_REG : DSP_OP_PAR_REG_REG_NOALU;
                ins->a = (mv >> 5) & 0x1Fu;
                ins->imm = mv & 0x1Fu;
                return;
            }
            unsigned dst = (mv >> 8) & 0x1Fu;
            uint32_t imm = mv & 0xFFu;
            if ((dst >= 4 && dst <= 7) || dst == 14 || dst == 15) {
                imm <<= 16;
            }
            ins->kind = alu ? DSP_OP_PAR_SHORT_IMM : DSP_OP_PAR_SHORT_IMM_NOALU;
            ins->a = dst;
            ins->imm = imm;
            return;
        }

        if (top4 == 1) {
            bool y = (op >> 14) & 1;
            bool read = (op >> 15) & 1;
            unsigned reg = y ? ((op >> 16) & 3) : ((op >> 18) & 3);
            static const uint8_t xr[] = { 4, 5, 14, 15 };
            static const uint8_t yr[] = { 6, 7, 14, 15 };
            unsigned memory_reg = y ? yr[reg] : xr[reg];
            unsigned acc_reg = 14 + ((op >> (y ? 19 : 17)) & 1);
            unsigned input_reg = (y ? 4 : 6) + ((op >> (y ? 18 : 16)) & 1);
            unsigned ea = (op >> 8) & 0x3Fu;

            ins->kind = DSP_OP_PAR_CLASS1;
            ins->words = ((ea >> 3) == 6) ? 2 : 1;
            ins->imm = memory_reg | (acc_reg << 5) | (input_reg << 10) |
                       (ea << 15) | ((unsigned)y << 21) | ((unsigned)read << 22);
            return;
        }

        if ((op & 0xF40000u) == 0x400000u) {
            unsigned reg = ((op >> 17) & 4) | ((op >> 16) & 3);
            unsigned ea = (op >> 8) & 0x3Fu;
            bool ea_mode = (op & 0x4000u) != 0;
            bool read = (op & 0x8000u) != 0;

            ins->kind = DSP_OP_PAR_LONG_XY;
            ins->words = (ea_mode && ((ea >> 3) == 6)) ? 2 : 1;
            ins->imm = reg | (ea << 3) | ((unsigned)ea_mode << 9) |
                       ((unsigned)read << 10);
            return;
        }

        if (top4 >= 4 && top4 <= 7) {
            unsigned reg5 = ((op >> 17) & 0x18u) | ((op >> 16) & 0x07u);
            unsigned space_y = (op >> 19) & 1;
            unsigned mem_to_reg = (op >> 15) & 1;
            bool ea_mode = (op >> 14) & 1;
            unsigned mmmrrr = (op >> 8) & 0x3Fu;

            if (ea_mode && mmmrrr == 0x34 && mem_to_reg) {
                ins->kind = DSP_OP_PAR_IMM24;
                ins->words = 2;
                ins->a = reg5;
                return;
            }
            ins->a = (uint8_t)(reg5 | (space_y << 5) | (mem_to_reg << 6));
            ins->imm = mmmrrr;
            if (ea_mode) {
                ins->kind = alu ? DSP_OP_PAR_EA_MOVE : DSP_OP_PAR_EA_NOALU;
                ins->words = ((mmmrrr >> 3) == 6) ? 2 : 1;
            } else {
                ins->kind = alu ? DSP_OP_PAR_AA_MOVE : DSP_OP_PAR_AA_NOALU;
            }
            return;
        }

        ins->kind = DSP_OP_NOP;
        return;
    }

    /* Non-parallel instructions (bits 23:20 == 0) */
    if (op == 0x000000u) {
        ins->kind = DSP_OP_NOP;
    } else if (op == 0x000004u) {
        ins->kind = DSP_OP_RTI;
    } else if (op >= 0x000008u && op <= 0x00000Bu) {
        ins->kind = DSP_OP_INC_DEC;
        ins->a = op & 3u;
    } else if (op == 0x00000Cu) {
        ins->kind = DSP_OP_RTS;
    } else if (op == 0x000203u) {
        ins->kind = DSP_OP_DO_FOREVER;
        ins->words = 2;
    } else if (op == 0x00008Cu) {
        ins->kind = DSP_OP_ENDDO;
    } else if (op == 0x000086u) {
        ins->kind = DSP_OP_STOP;
    } else if (op == 0x000087u) {
        ins->kind = DSP_OP_WAIT;
    } else if ((op & 0xFF00FCu) == 0x0000B8u) {
        ins->kind = DSP_OP_ANDI;
        ins->imm = (op >> 8) & 0xFFu;
        ins->a = op & 3u;
    } else if ((op & 0xFF00FCu) == 0x0000F8u) {
        ins->kind = DSP_OP_ORI;
        ins->imm = (op >> 8) & 0xFFu;
        ins->a = op & 3u;
    } else if ((op & 0xFFFE00u) == 0x0C1C00u ||
               (op & 0xFFFFE0u) == 0x0C1E40u ||
               (op & 0xFFFFE0u) == 0x0C1E60u) {
        static const uint8_t regs[] = { 0, 0, 12, 13, 4, 6, 5, 7 };
        bool immediate = (op & 0xFFFE00u) == 0x0C1C00u;
        bool left = immediate ? ((op & 0x100u) != 0) : !(op & 0x20u);
        bool src_b = (op & (immediate ? 0x80u : 0x10u)) != 0;
        bool dst_b = (op & 1u) != 0;

        ins->kind = DSP_OP_SHIFT56;
        ins->a = (uint8_t)(dst_b | (src_b << 1) | (left << 2) | (immediate << 3));
        ins->imm = immediate ? ((op >> 1) & 0x3Fu) : regs[(op >> 1) & 7];
    } else if ((op & 0xFFFF80u) == 0x0C1E80u ||
               (op & 0xFFFFD0u) == 0x0C1E10u) {
        static const uint8_t regs[] = { 0, 0, 12, 13, 4, 6, 5, 7 };
        bool immediate = (op & 0x80u) != 0;
        bool left = !(op & (immediate ? 0x40u : 0x20u));
        bool dst_b = (op & 1u) != 0;

        ins->kind = DSP_OP_SHIFT24;
        ins->a = (uint8_t)(dst_b | (left << 2) | (immediate << 3));
        ins->imm = immediate ? ((op >> 1) & 31u) : regs[(op >> 1) & 7];
    } else if ((op & 0xFFF000u) == 0x0C0000u) {
        ins->kind = DSP_OP_JMP12;
        ins->imm = op & 0xFFFu;
    } else if ((op & 0xFFF000u) == 0x0D0000u) {
        ins->kind = DSP_OP_JSR12;
        ins->imm = op & 0xFFFu;
    } else if ((op & 0xFF0000u) == 0x0E0000u) {
        ins->kind = DSP_OP_JCC12;
        ins->a = (op >> 12) & 0xFu;
        ins->imm = op & 0xFFFu;
    } else if ((op & 0xFF0000u) == 0x0F0000u) {
        ins->kind = DSP_OP_JSCC12;
        ins->a = (op >> 12) & 0xFu;
        ins->imm = op & 0xFFFu;
    } else if (op == 0x0AF080u) {
        ins->kind = DSP_OP_JMP24;
        ins->words = 2;
    } else if (op == 0x0BF080u) {
        ins->kind = DSP_OP_JSR24;
        ins->words = 2;
    } else if ((op & 0xFFFFF0u) == 0x0AF0A0u) {
        ins->kind = DSP_OP_JCC24;
        ins->words = 2;
        ins->a = op & 0xFu;
    } else if ((op & 0xFFFFF0u) == 0x0BF0A0u) {
        ins->kind = DSP_OP_JSCC24;
        ins->words = 2;
        ins->a = op & 0xFu;
    } else if ((op & 0xFFFC20u) == 0x050C00u ||
               (op & 0xFFFC20u) == 0x050800u ||
               (op & 0xFF0C20u) == 0x050400u) {
        unsigned bits = ((op >> 1) & 0x1E0u) | (op & 0x1Fu);
        int32_t disp = (bits & 0x100u) ? (int32_t)bits - 0x200 : (int32_t)bits;
        bool bsr = (op & 0xC00u) == 0x800u;
        bool is_cond = (op & 0xC00u) == 0x400u;
        unsigned cc = (op >> 12) & 15u;

        ins->kind = DSP_OP_BRA9;
        ins->imm = (pc + disp) & DSP_MASK24;
        ins->a = (uint8_t)(cc | (bsr << 4) | (is_cond << 5));
    } else if (op == 0x0D10C0u || op == 0x0D1080u) {
        ins->kind = DSP_OP_BRA24;
        ins->words = 2;
        ins->a = (op == 0x0D1080u) ? 1 : 0;
    } else if ((op & 0xFFE0F0u) == 0x044010u) {
        ins->kind = DSP_OP_LUA;
        ins->a = (uint8_t)(((op >> 8) & 7u) | (((op >> 11) & 3u) << 3));
        ins->imm = op & 0xFu;
    } else if ((op & 0xFFC000u) == 0x044000u ||
               (op & 0xFFC000u) == 0x04C000u) {
        unsigned reg = (op >> 8) & 0x3Fu;
        bool write = (op >> 15) & 1;

        if ((op & 0xA0u) == 0xA0u) {
            ins->kind = DSP_OP_MOVEC_RR;
            ins->a = reg;
            ins->imm = (op & 0x3Fu) | ((unsigned)write << 8);
        } else {
            bool space_x = (op >> 7) & 1;
            ins->kind = DSP_OP_MOVEP_LOW;
            ins->a = reg;
            ins->alu = (uint8_t)(((unsigned)write << 1) | (unsigned)space_x);
            ins->imm = 0xFFFF80u + ((op >> 1) & 0x20u) + (op & 0x1Fu);
        }
    } else if ((op & 0xFF0000u) == 0x050000u) {
        if ((op & 0x0000E0u) == 0x0000A0u) {
            ins->kind = DSP_OP_MOVEC_05_IMM8;
            ins->a = op & 0x3Fu;
            ins->imm = (op >> 8) & 0xFFu;
        } else {
            unsigned mid = (op >> 8) & 0xFFu;
            bool w = (mid >> 7) & 1;
            bool ea_mode = (mid >> 6) & 1;
            unsigned mmmrrr = mid & 0x3Fu;
            bool space_y = (op >> 6) & 1;
            unsigned reg6 = op & 0x3Fu;

            if (ea_mode && mmmrrr == 0x34 && w) {
                ins->kind = DSP_OP_MOVEC_05_IMM24;
                ins->words = 2;
                ins->a = reg6;
            } else {
                ins->kind = DSP_OP_MOVEC_05_MEM;
                ins->words = (ea_mode && ((mmmrrr >> 3) == 6)) ? 2 : 1;
                ins->a = reg6;
                ins->alu = (uint8_t)((unsigned)ea_mode | ((unsigned)w << 1) |
                                     ((unsigned)space_y << 2));
                ins->imm = mmmrrr;
            }
        }
    } else if ((op & 0xFF0000u) == 0x060000u) {
        bool is_rep = (op & 0x20u) != 0;
        uint8_t cnt_mode;
        uint32_t cnt_arg;

        if ((op & 0xD0u) == 0x80u) {
            cnt_mode = 0;
            cnt_arg = ((op & 0x0Fu) << 8) | ((op >> 8) & 0xFFu);
        } else if ((op & 0xC0DFu) == 0xC000u) {
            cnt_mode = 1;
            cnt_arg = (op >> 8) & 0x3Fu;
        } else {
            cnt_mode = 2;
            cnt_arg = (op >> 8) & 0x3Fu;
        }
        ins->kind = is_rep ? DSP_OP_REP : DSP_OP_DO;
        ins->words = is_rep ? 1 : 2;
        ins->a = cnt_mode | (uint8_t)(((op >> 6) & 1u) << 2);
        ins->imm = cnt_arg;
    } else if ((op & 0xFE0000u) == 0x080000u || (op & 0xFF0000u) == 0x070000u) {
        if ((op & 0xFE00C0u) == 0x080000u) {
            ins->kind = DSP_OP_MOVEP_80;
            ins->imm = 0xFFFFC0u + (op & 0x3Fu);
            ins->a = (op >> 8) & 0x3Fu;
            ins->alu = (uint8_t)((((op >> 15) & 1u) << 1) | ((op >> 16) & 1u));
        } else {
            unsigned hi8 = (op >> 16) & 0xFFu;
            uint32_t paddr;
            bool space_y;

            if (hi8 == 0x07u) {
                paddr = 0xFFFF80u + (op & 0x3Fu);
                space_y = (op >> 7) & 1;
            } else {
                paddr = 0xFFFFC0u + (op & 0x3Fu);
                space_y = (hi8 == 0x09u);
            }
            bool w = (op >> 15) & 1;
            unsigned mmmrrr = (op >> 8) & 0x3Fu;
            bool ea_y = (op >> 6) & 1;

            ins->kind = DSP_OP_MOVEP_MEM;
            ins->words = ((mmmrrr >> 3) == 6) ? 2 : 1;
            ins->imm = paddr;
            ins->a = (uint8_t)(mmmrrr | ((unsigned)w << 6) |
                               ((unsigned)space_y << 7));
            ins->alu = (uint8_t)((unsigned)ea_y |
                                 ((mmmrrr == 0x34 && w) ? 2u : 0u));
        }
    } else if ((op & 0xFE0080u) == 0x020080u) {
        unsigned rn = (op >> 8) & 7;
        unsigned bits = ((op >> 10) & 0x7Eu) | ((op >> 6) & 1);
        int32_t disp = (bits & 0x40u) ? (int32_t)bits - 0x80 : (int32_t)bits;
        unsigned reg = op & 0xFu;

        ins->kind = DSP_OP_MOVE_DISP7;
        ins->imm = (uint32_t)disp;
        ins->a = (uint8_t)(rn | (reg << 3));
        ins->alu = (uint8_t)(((op >> 5) & 1u) | (((op >> 4) & 1u) << 1));
    } else if ((op & 0xFFC0FFu) == 0x0AC080u) {
        unsigned mmmrrr = (op >> 8) & 0x3Fu;
        ins->kind = DSP_OP_JMP_EA;
        ins->words = ((mmmrrr >> 3) == 6) ? 2 : 1;
        ins->imm = mmmrrr;
    } else if ((op & 0xFFC0F0u) == 0x0AC0A0u) {
        unsigned mmmrrr = (op >> 8) & 0x3Fu;
        ins->kind = DSP_OP_JCC_EA;
        ins->words = ((mmmrrr >> 3) == 6) ? 2 : 1;
        ins->a = op & 0xFu;
        ins->imm = mmmrrr;
    } else if ((op & 0xFFC0FFu) == 0x0BC080u) {
        unsigned mmmrrr = (op >> 8) & 0x3Fu;
        ins->kind = DSP_OP_JSR_EA;
        ins->words = ((mmmrrr >> 3) == 6) ? 2 : 1;
        ins->imm = mmmrrr;
    } else if ((op & 0xFFC0F0u) == 0x0BC0A0u) {
        unsigned mmmrrr = (op >> 8) & 0x3Fu;
        ins->kind = DSP_OP_JSCC_EA;
        ins->words = ((mmmrrr >> 3) == 6) ? 2 : 1;
        ins->a = op & 0xFu;
        ins->imm = mmmrrr;
    } else if ((op & 0xFFE0C0u) == 0x0100C0u) {
        static const uint8_t qq_reg[4] = { 0x07, 0x04, 0x06, 0x05 };
        ins->kind = DSP_OP_MPY_IMM;
        ins->a = op & 0xFu;
        ins->imm = ((op >> 8) & 0x1Fu) | (qq_reg[(op >> 4) & 3] << 5);
    } else if ((op & 0xFFFC80u) == 0x012480u) {
        ins->kind = DSP_OP_DMAC_MPYSU;
        ins->imm = op;
    } else if ((op & 0xFEF880u) == 0x0A7080u) {
        ins->kind = DSP_OP_MOVE_LONGDISP;
        ins->words = 2;
        ins->alu = (op >> 8) & 7;
        ins->a = (uint8_t)((op & 0x3Fu) | ((op & 0x40u) ? 0x40u : 0) |
                           ((op & 0x10000u) ? 0x80u : 0));
    } else if ((op & 0xFF0000u) == 0x010000u ||
               (op & 0xFF0000u) == 0x0A0000u ||
               (op & 0xFF0000u) == 0x0B0000u) {
        if ((op & 0xFF0080u) == 0x010080u && (op & 0x008000u) == 0 &&
            (op & 0x004000u) != 0) {
            bool long_imm = (op & 0x0000C0u) == 0x0000C0u;
            ins->kind = DSP_OP_IMM_ALU_01;
            ins->words = long_imm ? 2 : 1;
            ins->imm = long_imm ? 0 : ((op >> 8) & 0x3Fu);
            ins->a = op & 0xFu;
        } else {
            unsigned bit = op & 0x1Fu;
            bool is_reg_op = ((op >> 16) == 0x0Au || (op >> 16) == 0x0Bu) &&
                             (((op >> 8) & 0xFFu) >= 0xC0u);
            bool is_branch = ((op >> 16) == 0x01u) ? ((op >> 15) & 1) :
                             (is_reg_op ? !((op >> 6) & 1) : ((op >> 7) & 1));
            bool is_jsr = ((op >> 16) == 0x0Bu) && is_branch;
            bool bit_set_cond = (op >> 5) & 1;
            bool space_y = (op >> 6) & 1;
            uint8_t mode;
            uint8_t arg;
            uint8_t subop = 0;

            if ((op >> 16) == 0x01u) {
                mode = 0;
                arg = (op >> 8) & 0x3Fu;
            } else {
                unsigned mid = (op >> 8) & 0xFFu;
                if (mid >= 0xC0u) {
                    mode = 1;
                    arg = mid & 0x3Fu;
                } else if (mid >= 0x80u) {
                    mode = 2;
                    arg = mid & 0x3Fu;
                } else if (mid >= 0x40u) {
                    mode = 3;
                    arg = mid & 0x3Fu;
                } else {
                    mode = 4;
                    arg = mid & 0x3Fu;
                }
            }
            if (!is_branch) {
                if ((op >> 16) == 0x01u) {
                    if (op & 0x4000u) {
                        subop = (op & 0x20u) ? 2 : 1;
                    }
                } else if ((op >> 16) == 0x0Bu) {
                    subop = (op & 0x20u) ? 2 : 1;
                }
            }
            ins->kind = is_branch ? DSP_OP_BIT_BRANCH : DSP_OP_BIT_OP;
            ins->words = (is_branch || (mode == 3 && ((arg >> 3) == 6))) ? 2 : 1;
            ins->imm = 1u << bit;
            ins->alu = arg;
            ins->a = mode | (uint8_t)((unsigned)space_y << 3) |
                     (uint8_t)((unsigned)bit_set_cond << 4) |
                     (uint8_t)((unsigned)is_jsr << 5) |
                     (uint8_t)(subop << 6);
        }
    } else if ((op & 0xFE0000u) == 0x020000u) {
        unsigned cc = (op >> 12) & 0xFu;
        bool addr_only = !(op & 0x10000u) && (op & 0x800u);
        bool has_addr = (op & 0x10000u) || addr_only;
        ins->kind = DSP_OP_TCC;
        ins->a = (uint8_t)(cc | ((unsigned)addr_only << 4) |
                           ((unsigned)has_addr << 5));
        ins->imm = op & 0xFFFu;
    } else {
        ins->kind = DSP_OP_UNIMP;
    }
}

static __attribute__((noinline)) bool
step_one_cold(Dsp56300 *d, const DspInsn *ins, uint32_t pc, uint32_t *next_pc_out)
{
    uint32_t next_pc = *next_pc_out;

    switch (ins->kind) {
    case DSP_OP_PAR_CLASS1: {
        uint32_t pk = ins->imm;
        unsigned memory_reg = pk & 0x1Fu;
        unsigned acc_reg = (pk >> 5) & 0x1Fu;
        unsigned input_reg = (pk >> 10) & 0x1Fu;
        unsigned ea = (pk >> 15) & 0x3Fu;
        bool y = (pk >> 21) & 1;
        bool read = (pk >> 22) & 1;
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t addr = eval_ea(d, ea, ext, NULL, true);
        uint32_t value = read ? (ea == 0x34 ? ext :
                                   y ? mem_read_y(d, addr) : mem_read_x(d, addr)) :
                               read_reg6(d, memory_reg);
        uint32_t acc = read_reg6(d, acc_reg);

        if (ins->alu) {
            exec_alu(d, ins->alu);
        }
        write_reg6(d, input_reg, acc);
        if (read) {
            write_reg6(d, memory_reg, value);
        } else if (y) {
            mem_write_y(d, addr, value);
        } else {
            mem_write_x(d, addr, value);
        }
        if (ins->words == 2) {
            next_pc = pc + 2;
        }
        break;
    }

    case DSP_OP_PAR_LONG_XY: {
        uint32_t pk = ins->imm;
        unsigned reg = pk & 7u;
        unsigned ea = (pk >> 3) & 0x3Fu;
        bool ea_mode = (pk >> 9) & 1;
        bool read = (pk >> 10) & 1;
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t addr = ea_mode ? eval_ea(d, ea, ext, NULL, true) : ea;
        uint32_t hi = 0, lo = 0;

        if (read) {
            hi = mem_read_x(d, addr);
            lo = mem_read_y(d, addr);
        } else {
            switch (reg) {
            case 0: case 4: hi = d->a >> 24; lo = d->a; break;
            case 1: case 5: hi = d->b >> 24; lo = d->b; break;
            case 2: hi = d->x1; lo = d->x0; break;
            case 3: hi = d->y1; lo = d->y0; break;
            case 6: hi = read_reg6(d, 14); lo = read_reg6(d, 15); break;
            case 7: hi = read_reg6(d, 15); lo = read_reg6(d, 14); break;
            }
        }
        if (ins->alu) {
            exec_alu(d, ins->alu);
        }
        if (read) {
            switch (reg) {
            case 0: case 4: d->a = pack_acc24(hi) | lo; break;
            case 1: case 5: d->b = pack_acc24(hi) | lo; break;
            case 2: d->x1 = hi; d->x0 = lo; break;
            case 3: d->y1 = hi; d->y0 = lo; break;
            case 6: write_reg6(d, 14, hi); write_reg6(d, 15, lo); break;
            case 7: write_reg6(d, 15, hi); write_reg6(d, 14, lo); break;
            }
        } else {
            mem_write_x(d, addr, hi);
            mem_write_y(d, addr, lo);
        }
        if (ins->words == 2) {
            next_pc = pc + 2;
        }
        break;
    }

    case DSP_OP_RTI: {
        uint32_t h, l;
        stack_pop(d, &h, &l);
        d->sr = l;
        next_pc = h;
        break;
    }

    case DSP_OP_INC_DEC: {
        uint64_t *dst = (ins->a & 1) ? &d->b : &d->a;
        int64_t v = sext56(*dst) + ((ins->a & 2) ? -1 : 1);
        *dst = (uint64_t)v & DSP_MASK56;
        set_ccr56(d, *dst, false, false);
        break;
    }

    case DSP_OP_DO_FOREVER:
        stack_push(d, d->la, d->lc);
        stack_push(d, pc + 2, d->sr);
        d->la = dsp_ext(d, pc) & DSP_MASK24;
        d->sr |= SR_LF | SR_FV;
        next_pc = pc + 2;
        break;

    case DSP_OP_ENDDO: {
        uint32_t h, l, saved_sr;
        stack_pop(d, NULL, &saved_sr);
        stack_pop(d, &h, &l);
        d->la = h;
        d->lc = l;
        d->sr = (d->sr & ~(SR_LF | SR_FV)) | (saved_sr & (SR_LF | SR_FV));
        break;
    }

    case DSP_OP_STOP:
        d->stop_mode = true;
        break;

    case DSP_OP_WAIT:
        d->wait_mode = true;
        break;

    case DSP_OP_ANDI:
        if (ins->a == 0) {
            d->sr = (d->sr & 0x00FFu) | (((d->sr >> 8) & ins->imm) << 8);
        } else if (ins->a == 1) {
            d->sr = (d->sr & 0xFF00u) | (d->sr & ins->imm);
        } else if (ins->a == 2) {
            d->omr = (d->omr & ~0xFFu) | (d->omr & ins->imm);
        }
        break;

    case DSP_OP_ORI:
        if (ins->a == 0) {
            d->sr |= (ins->imm << 8);
        } else if (ins->a == 1) {
            d->sr |= ins->imm;
        } else if (ins->a == 2) {
            d->omr |= ins->imm;
        }
        break;

    case DSP_OP_SHIFT56: {
        bool left = (ins->a >> 2) & 1;
        uint64_t value = (ins->a & 2) ? d->b : d->a;
        unsigned n = (ins->a & 8) ? ins->imm : (read_reg6(d, ins->imm) & 0x3Fu);
        bool carry = n ? (left ? (n <= 56 && ((value >> (56 - n)) & 1)) :
                                ((value >> (n - 1)) & 1)) : (d->sr & SR_C);
        uint64_t result = left ? (value << n) & DSP_MASK56 :
                                 (uint64_t)(sext56(value) >> n) & DSP_MASK56;
        bool overflow = left && (sext56(result) >> n) != sext56(value);
        if (ins->a & 1) {
            d->b = result;
        } else {
            d->a = result;
        }
        set_ccr56(d, result, carry, overflow);
        break;
    }

    case DSP_OP_SHIFT24: {
        bool left = (ins->a >> 2) & 1;
        unsigned n = (ins->a & 8) ? ins->imm : (read_reg6(d, ins->imm) & 31u);
        uint64_t *dst = (ins->a & 1) ? &d->b : &d->a;
        uint32_t value = (*dst >> 24) & DSP_MASK24;
        bool carry = n ? (left ? (n <= 24 && ((value >> (24 - n)) & 1)) :
                                ((value >> (n - 1)) & 1)) : (d->sr & SR_C);
        uint32_t result = left ? (value << n) & DSP_MASK24 : value >> n;
        *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)result << 24);
        set_ccr_logic(d, *dst);
        d->sr = (d->sr & ~SR_C) | (carry ? SR_C : 0);
        break;
    }

    case DSP_OP_JMP24: {
        uint32_t target = dsp_ext(d, pc) & DSP_MASK24;
        if (target == DSP_BOOT_ROM) {
            d->boot_state = BOOT_COUNT;
            d->halted = true;
            d->pc = DSP_BOOT_ROM;
            return false;
        }
        next_pc = target;
        break;
    }

    case DSP_OP_JSR24:
        stack_push(d, pc + 2, d->sr);
        next_pc = dsp_ext(d, pc) & DSP_MASK24;
        break;

    case DSP_OP_JCC24:
        next_pc = eval_cc(d, ins->a) ? (dsp_ext(d, pc) & DSP_MASK24) : pc + 2;
        break;

    case DSP_OP_JSCC24:
        if (eval_cc(d, ins->a)) {
            stack_push(d, pc + 2, d->sr);
            next_pc = dsp_ext(d, pc) & DSP_MASK24;
        } else {
            next_pc = pc + 2;
        }
        break;

    case DSP_OP_BRA9:
        if (!(ins->a & 0x20u) || eval_cc(d, ins->a & 0xFu)) {
            if (ins->a & 0x10u) {
                stack_push(d, pc + 1, d->sr);
            }
            next_pc = ins->imm;
        }
        break;

    case DSP_OP_BRA24:
        if (ins->a) {
            stack_push(d, pc + 2, d->sr);
        }
        next_pc = (pc + sext24(dsp_ext(d, pc))) & DSP_MASK24;
        break;

    case DSP_OP_LUA: {
        unsigned rn = ins->a & 7u;
        int32_t delta;
        switch ((ins->a >> 3) & 3u) {
        case 0: delta = -sext24(d->n[rn]); break;
        case 1: delta = sext24(d->n[rn]); break;
        case 2: delta = -1; break;
        default: delta = 1; break;
        }
        uint32_t ea = agu_add(d, rn, delta);
        if (ins->imm & 8u) {
            d->n[ins->imm & 7u] = ea;
        } else {
            d->r[ins->imm & 7u] = ea;
        }
        break;
    }

    case DSP_OP_MOVEC_RR: {
        unsigned control = ins->imm & 0x3Fu;
        if (ins->imm & 0x100u) {
            write_reg6(d, control, read_reg6(d, ins->a));
        } else {
            write_reg6(d, ins->a, read_reg6(d, control));
        }
        break;
    }

    case DSP_OP_MOVEP_LOW: {
        bool write = (ins->alu >> 1) & 1;
        bool space_x = ins->alu & 1;
        if (write) {
            uint32_t v = read_reg6(d, ins->a);
            if (space_x) {
                mem_write_x(d, ins->imm, v);
            } else {
                mem_write_y(d, ins->imm, v);
            }
        } else {
            write_reg6(d, ins->a, space_x ? mem_read_x(d, ins->imm) :
                                           mem_read_y(d, ins->imm));
        }
        break;
    }

    case DSP_OP_MOVEC_05_IMM8:
        write_reg6(d, ins->a, ins->imm);
        break;

    case DSP_OP_MOVEC_05_IMM24:
        write_reg6(d, ins->a, dsp_ext(d, pc));
        next_pc = pc + 2;
        break;

    case DSP_OP_MOVEC_05_MEM: {
        bool ea_mode = ins->alu & 1;
        bool w = (ins->alu >> 1) & 1;
        bool space_y = (ins->alu >> 2) & 1;
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t addr = ea_mode ? eval_ea(d, ins->imm, ext, NULL, true) :
                                  ins->imm;
        if (ins->words == 2) {
            next_pc = pc + 2;
        }
        if (w) {
            uint32_t v = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            write_reg6(d, ins->a, v);
        } else {
            uint32_t v = read_reg6(d, ins->a);
            if (space_y) {
                mem_write_y(d, addr, v);
            } else {
                mem_write_x(d, addr, v);
            }
        }
        break;
    }

    case DSP_OP_REP: {
        uint32_t cnt;
        switch (ins->a & 3u) {
        case 0: cnt = ins->imm; break;
        case 1: cnt = read_reg6(d, ins->imm); break;
        default: {
            uint32_t ea = eval_ea(d, ins->imm, 0, NULL, true);
            cnt = (ins->a & 4u) ? mem_read_y(d, ea) : mem_read_x(d, ea);
            break;
        }
        }
        d->rep_saved_lc = d->lc;
        d->rep_remaining = cnt ? cnt : 65536;
        d->lc = d->rep_remaining;
        d->rep_pc = pc + 1;
        break;
    }

    case DSP_OP_DO: {
        uint32_t cnt;
        switch (ins->a & 3u) {
        case 0: cnt = ins->imm; break;
        case 1: cnt = read_reg6(d, ins->imm); break;
        default: {
            uint32_t ea = eval_ea(d, ins->imm, 0, NULL, true);
            cnt = (ins->a & 4u) ? mem_read_y(d, ea) : mem_read_x(d, ea);
            break;
        }
        }
        uint32_t la = dsp_ext(d, pc) & DSP_MASK24;
        if (!cnt && !(d->sc & (1u << 13))) {
            next_pc = (la + 1) & DSP_MASK24;
        } else {
            if (d->debug_enabled) {
                info_report("cdj850-dsp: DO at P:%06x op=0x%06x cnt=%u la=0x%06x",
                            pc, ins->op, cnt, la);
            }
            stack_push(d, d->la, d->lc);
            stack_push(d, pc + 2, d->sr);
            d->la = la;
            d->lc = cnt ? cnt : 65536;
            d->sr = (d->sr & ~SR_FV) | SR_LF;
            next_pc = pc + 2;
        }
        break;
    }

    case DSP_OP_MOVEP_80: {
        bool w = (ins->alu >> 1) & 1;
        bool y = ins->alu & 1;
        if (w) {
            uint32_t v = read_reg6(d, ins->a);
            if (y) {
                mem_write_y(d, ins->imm, v);
            } else {
                mem_write_x(d, ins->imm, v);
            }
        } else {
            write_reg6(d, ins->a, y ? mem_read_y(d, ins->imm) :
                                      mem_read_x(d, ins->imm));
        }
        break;
    }

    case DSP_OP_MOVEP_MEM: {
        bool space_y = (ins->a >> 7) & 1;
        if (ins->words == 2) {
            next_pc = pc + 2;
        }
        if (ins->alu & 2u) {
            uint32_t ext = dsp_ext(d, pc);
            if (space_y) {
                mem_write_y(d, ins->imm, ext);
            } else {
                mem_write_x(d, ins->imm, ext);
            }
        } else {
            bool w = (ins->a >> 6) & 1;
            bool ea_y = ins->alu & 1;
            uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
            uint32_t ea = eval_ea(d, ins->a & 0x3Fu, ext, NULL, true);
            if (w) {
                uint32_t v = ea_y ? mem_read_y(d, ea) : mem_read_x(d, ea);
                if (space_y) {
                    mem_write_y(d, ins->imm, v);
                } else {
                    mem_write_x(d, ins->imm, v);
                }
            } else {
                uint32_t v = space_y ? mem_read_y(d, ins->imm) :
                                       mem_read_x(d, ins->imm);
                if (ea_y) {
                    mem_write_y(d, ea, v);
                } else {
                    mem_write_x(d, ea, v);
                }
            }
        }
        break;
    }

    case DSP_OP_MOVE_DISP7: {
        unsigned rn = ins->a & 7u;
        unsigned reg = (ins->a >> 3) & 0xFu;
        bool y = ins->alu & 1;
        bool w = (ins->alu >> 1) & 1;
        uint32_t addr = (d->r[rn] + (int32_t)ins->imm) & DSP_MASK24;

        if (w) {
            write_reg6(d, reg, y ? mem_read_y(d, addr) : mem_read_x(d, addr));
        } else if (y) {
            mem_write_y(d, addr, read_reg6(d, reg));
        } else {
            mem_write_x(d, addr, read_reg6(d, reg));
        }
        break;
    }

    case DSP_OP_JMP_EA: {
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t target = eval_ea(d, ins->imm, ext, NULL, false);
        if (target == DSP_BOOT_ROM) {
            d->boot_state = BOOT_COUNT;
            d->halted = true;
            d->pc = DSP_BOOT_ROM;
            return false;
        }
        next_pc = target;
        break;
    }

    case DSP_OP_JCC_EA: {
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t target = eval_ea(d, ins->imm, ext, NULL, false);
        if (eval_cc(d, ins->a)) {
            next_pc = target;
        } else if (ins->words == 2) {
            next_pc = pc + 2;
        }
        break;
    }

    case DSP_OP_JSR_EA: {
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t target = eval_ea(d, ins->imm, ext, NULL, false);
        stack_push(d, ins->words == 2 ? pc + 2 : pc + 1, d->sr);
        next_pc = target;
        break;
    }

    case DSP_OP_JSCC_EA: {
        uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
        uint32_t target = eval_ea(d, ins->imm, ext, NULL, false);
        if (eval_cc(d, ins->a)) {
            stack_push(d, ins->words == 2 ? pc + 2 : pc + 1, d->sr);
            next_pc = target;
        } else if (ins->words == 2) {
            next_pc = pc + 2;
        }
        break;
    }

    case DSP_OP_MPY_IMM: {
        unsigned n = ins->imm & 0x1Fu;
        unsigned src_reg = (ins->imm >> 5) & 0x3Fu;
        uint64_t *dst = (ins->a & 8u) ? &d->b : &d->a;
        int64_t prod = sext56(pack_acc24(read_reg6(d, src_reg))) >> n;
        int64_t res = ((ins->a & 2u) ? sext56(*dst) : 0) +
                      ((ins->a & 4u) ? -prod : prod);
        if (ins->a & 1u) {
            uint64_t r = (uint64_t)res;
            if ((r & 0xFFFFFFULL) > 0x800000ULL ||
                ((r & 0xFFFFFFULL) == 0x800000ULL && (r & 0x1000000ULL))) {
                res += 0x1000000LL;
            }
            res &= ~0xFFFFFFLL;
        }
        *dst = (uint64_t)res & DSP_MASK56;
        set_ccr56(d, *dst, false, false);
        break;
    }

    case DSP_OP_DMAC_MPYSU: {
        static const uint8_t qqqq[16][2] = {
            { 4, 4 }, { 6, 6 }, { 5, 4 }, { 7, 6 }, { 4, 7 }, { 6, 4 },
            { 5, 6 }, { 7, 5 }, { 5, 5 }, { 7, 7 }, { 4, 5 }, { 6, 7 },
            { 7, 4 }, { 4, 6 }, { 6, 5 }, { 5, 7 },
        };
        uint32_t op = ins->imm;
        unsigned kind = (op >> 8) & 3;
        bool s_bit = op & 0x40u;
        bool s1_unsigned = kind <= 1 ? (op & 0x100u) && s_bit : s_bit;
        bool s2_unsigned = kind <= 1 ? (op & 0x100u) || s_bit : true;
        uint32_t v1 = read_reg6(d, qqqq[op & 15][0]);
        uint32_t v2 = read_reg6(d, qqqq[op & 15][1]);
        int64_t m1 = s1_unsigned ? (int64_t)v1 : sext24(v1);
        int64_t m2 = s2_unsigned ? (int64_t)v2 : sext24(v2);
        int64_t prod = m1 * m2 * 2;
        uint64_t *dst = (op & 0x20u) ? &d->b : &d->a;
        int64_t acc = kind == 3 ? 0 :
                      kind == 2 ? sext56(*dst) : sext56(*dst) >> 24;

        *dst = (uint64_t)(acc + ((op & 0x10u) ? -prod : prod)) & DSP_MASK56;
        set_ccr56(d, *dst, false, false);
        break;
    }

    case DSP_OP_MOVE_LONGDISP: {
        uint32_t addr = (d->r[ins->alu] + dsp_ext(d, pc)) & DSP_MASK24;
        bool y = (ins->a & 0x80u) != 0;
        unsigned reg = ins->a & 0x3Fu;

        if (ins->a & 0x40u) {
            write_reg6(d, reg, y ? mem_read_y(d, addr) : mem_read_x(d, addr));
        } else if (y) {
            mem_write_y(d, addr, read_reg6(d, reg));
        } else {
            mem_write_x(d, addr, read_reg6(d, reg));
        }
        next_pc = pc + 2;
        break;
    }

    case DSP_OP_IMM_ALU_01: {
        uint64_t *dst = (ins->a & 8u) ? &d->b : &d->a;
        uint32_t imm = (ins->words == 2) ? dsp_ext(d, pc) : ins->imm;
        uint64_t imm56 = pack_acc24(imm);
        unsigned kind = ins->a & 7u;

        if (ins->words == 2) {
            next_pc = pc + 2;
        }
        if (kind == 0) {
            *dst = (uint64_t)(sext56(*dst) + sext56(imm56)) & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
        } else if (kind == 2) {
            uint32_t a1 = ((uint32_t)(*dst >> 24) | imm) & DSP_MASK24;
            *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
            set_ccr_logic(d, *dst);
        } else if (kind == 3) {
            uint32_t a1 = ((uint32_t)(*dst >> 24) ^ imm) & DSP_MASK24;
            *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
            set_ccr_logic(d, *dst);
        } else if (kind == 4) {
            *dst = (uint64_t)(sext56(*dst) - sext56(imm56)) & DSP_MASK56;
            set_ccr56(d, *dst, false, false);
        } else if (kind == 5) {
            set_ccr56(d, (uint64_t)(sext56(*dst) - sext56(imm56)),
                      *dst < imm56, false);
        } else if (kind == 6) {
            uint32_t a1 = ((uint32_t)(*dst >> 24) & imm) & DSP_MASK24;
            *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
            set_ccr_logic(d, *dst);
        }
        break;
    }

    case DSP_OP_BIT_OP: {
        uint8_t mode = ins->a & 7u;
        bool space_y = (ins->a >> 3) & 1;
        bool bit_set_cond = (ins->a >> 4) & 1;
        uint8_t subop = (ins->a >> 6) & 3u;
        uint32_t val = 0, addr = 0;

        switch (mode) {
        case 0:
            addr = 0xFFFF80u + ins->alu;
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        case 1:
            val = read_reg6(d, ins->alu);
            break;
        case 2:
            addr = 0xFFFFC0u + ins->alu;
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        case 3:
            addr = eval_ea(d, ins->alu, ins->words == 2 ? dsp_ext(d, pc) : 0, NULL, true);
            if (ins->words == 2) {
                next_pc = pc + 2;
            }
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        default:
            addr = ins->alu;
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        }
        if (val & ins->imm) {
            d->sr |= SR_C;
        } else {
            d->sr &= ~SR_C;
        }
        if (subop != 2) {
            uint32_t nval = (subop == 1) ? (val ^ ins->imm) :
                            (bit_set_cond ? (val | ins->imm) :
                                            (val & ~ins->imm));
            if (mode == 1) {
                write_reg6(d, ins->alu, nval);
            } else if (space_y) {
                mem_write_y(d, addr, nval);
            } else {
                mem_write_x(d, addr, nval);
            }
        }
        break;
    }

    case DSP_OP_BIT_BRANCH: {
        uint8_t mode = ins->a & 7u;
        bool space_y = (ins->a >> 3) & 1;
        bool bit_set_cond = (ins->a >> 4) & 1;
        bool is_jsr = (ins->a >> 5) & 1;
        uint32_t val = 0, addr = 0;

        switch (mode) {
        case 0:
            addr = 0xFFFF80u + ins->alu;
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        case 1:
            val = read_reg6(d, ins->alu);
            break;
        case 2:
            addr = 0xFFFFC0u + ins->alu;
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        case 3:
            addr = eval_ea(d, ins->alu, dsp_ext(d, pc), NULL, true);
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        default:
            addr = ins->alu;
            val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            break;
        }
        bool was_set = (val & ins->imm) != 0;
        if (bit_set_cond ? was_set : !was_set) {
            if (is_jsr) {
                stack_push(d, pc + 2, d->sr);
            }
            next_pc = dsp_ext(d, pc) & DSP_MASK24;
        } else {
            next_pc = pc + 2;
        }
        break;
    }

    case DSP_OP_TCC:
        if (eval_cc(d, ins->a & 0xFu)) {
            uint32_t op = ins->imm;
            if (!(ins->a & 0x10u)) {
                unsigned src = (op >> 4) & 7u;
                bool dst_b = op & 8u;
                uint64_t value = dst_b ? d->a : d->b;
                switch (src) {
                case 4: value = pack_acc24(d->x0); break;
                case 5: value = pack_acc24(d->y0); break;
                case 6: value = pack_acc24(d->x1); break;
                case 7: value = pack_acc24(d->y1); break;
                }
                if (dst_b) {
                    d->b = value;
                } else {
                    d->a = value;
                }
            }
            if (ins->a & 0x20u) {
                d->r[op & 7u] = d->r[(op >> 8) & 7u];
            }
        }
        break;

    case DSP_OP_UNIMP:
    default:
        if (d->unimp_logged < 20) {
            d->unimp_logged++;
            info_report("cdj850-dsp: unimplemented op 0x%06x at P:%06x",
                        ins->op, pc);
        }
        break;
    }
    *next_pc_out = next_pc;
    return true;
}

static __attribute__((noinline)) DspInsn *
dsp_icache_fill(Dsp56300 *d, uint32_t pc, uint32_t op)
{
    DspInsn *slot = &d->icache[pc];

    dsp_decode_insn(slot, pc, op);
    return slot;
}

static bool dsp_exec(Dsp56300 *d, unsigned budget, int32_t stop_sp)
{
    if (__builtin_expect(d->halted || d->wait_mode || d->stop_mode || !budget, 0)) {
        return false;
    }

    static const void *const dispatch[] = {
        [DSP_OP_INVALID]             = &&lbl_COLD,
        [DSP_OP_NOP]                 = &&lbl_NOP,
        [DSP_OP_RTI]                 = &&lbl_COLD,
        [DSP_OP_INC_DEC]             = &&lbl_COLD,
        [DSP_OP_RTS]                 = &&lbl_RTS,
        [DSP_OP_DO_FOREVER]          = &&lbl_COLD,
        [DSP_OP_ENDDO]               = &&lbl_COLD,
        [DSP_OP_STOP]                = &&lbl_COLD,
        [DSP_OP_WAIT]                = &&lbl_COLD,
        [DSP_OP_ANDI]                = &&lbl_COLD,
        [DSP_OP_ORI]                 = &&lbl_COLD,
        [DSP_OP_SHIFT56]             = &&lbl_SHIFT56,
        [DSP_OP_SHIFT24]             = &&lbl_COLD,
        [DSP_OP_JMP12]               = &&lbl_JMP12,
        [DSP_OP_JSR12]               = &&lbl_JSR12,
        [DSP_OP_JCC12]               = &&lbl_JCC12,
        [DSP_OP_JSCC12]              = &&lbl_JSCC12,
        [DSP_OP_JMP24]               = &&lbl_COLD,
        [DSP_OP_JSR24]               = &&lbl_COLD,
        [DSP_OP_JCC24]               = &&lbl_COLD,
        [DSP_OP_JSCC24]              = &&lbl_COLD,
        [DSP_OP_BRA9]                = &&lbl_COLD,
        [DSP_OP_BRA24]               = &&lbl_COLD,
        [DSP_OP_LUA]                 = &&lbl_COLD,
        [DSP_OP_MOVEC_RR]            = &&lbl_COLD,
        [DSP_OP_MOVEP_LOW]           = &&lbl_COLD,
        [DSP_OP_MOVEC_05_IMM8]       = &&lbl_COLD,
        [DSP_OP_MOVEC_05_IMM24]      = &&lbl_COLD,
        [DSP_OP_MOVEC_05_MEM]        = &&lbl_COLD,
        [DSP_OP_REP]                 = &&lbl_COLD,
        [DSP_OP_DO]                  = &&lbl_COLD,
        [DSP_OP_MOVEP_80]            = &&lbl_COLD,
        [DSP_OP_MOVEP_MEM]           = &&lbl_COLD,
        [DSP_OP_MOVE_DISP7]          = &&lbl_COLD,
        [DSP_OP_JMP_EA]              = &&lbl_COLD,
        [DSP_OP_JCC_EA]              = &&lbl_COLD,
        [DSP_OP_JSR_EA]              = &&lbl_COLD,
        [DSP_OP_JSCC_EA]             = &&lbl_COLD,
        [DSP_OP_MPY_IMM]             = &&lbl_MPY_IMM,
        [DSP_OP_DMAC_MPYSU]          = &&lbl_COLD,
        [DSP_OP_MOVE_LONGDISP]       = &&lbl_COLD,
        [DSP_OP_IMM_ALU_01]          = &&lbl_COLD,
        [DSP_OP_BIT_OP]              = &&lbl_BIT_OP,
        [DSP_OP_BIT_BRANCH]          = &&lbl_BIT_BRANCH,
        [DSP_OP_TCC]                 = &&lbl_COLD,
        [DSP_OP_PAR_UPDATE_RN]       = &&lbl_PAR_UPDATE_RN,
        [DSP_OP_PAR_XY_DUAL]         = &&lbl_PAR_XY_DUAL,
        [DSP_OP_PAR_XY_DUAL_NOALU]   = &&lbl_PAR_XY_DUAL_NOALU,
        [DSP_OP_PAR_IFCC]            = &&lbl_PAR_IFCC,
        [DSP_OP_PAR_REG_REG]         = &&lbl_PAR_REG_REG,
        [DSP_OP_PAR_REG_REG_NOALU]   = &&lbl_PAR_REG_REG_NOALU,
        [DSP_OP_PAR_SHORT_IMM]       = &&lbl_PAR_SHORT_IMM,
        [DSP_OP_PAR_SHORT_IMM_NOALU] = &&lbl_PAR_SHORT_IMM_NOALU,
        [DSP_OP_PAR_CLASS1]          = &&lbl_PAR_CLASS1,
        [DSP_OP_PAR_LONG_XY]         = &&lbl_COLD,
        [DSP_OP_PAR_IMM24]           = &&lbl_PAR_IMM24,
        [DSP_OP_PAR_EA_MOVE]         = &&lbl_PAR_EA_MOVE,
        [DSP_OP_PAR_EA_NOALU]        = &&lbl_PAR_EA_NOALU,
        [DSP_OP_PAR_AA_MOVE]         = &&lbl_PAR_AA_MOVE,
        [DSP_OP_PAR_AA_NOALU]        = &&lbl_PAR_AA_NOALU,
        [DSP_OP_UNIMP]               = &&lbl_COLD,
    };

    static const DspInsn nop_insn = {
        .op = 0, .imm = 0, .kind = DSP_OP_NOP, .words = 1
    };

    enum { WB_NONE = 0, WB_REG5, WB_MEM_X, WB_MEM_Y } wb = WB_NONE;
    uint32_t wb_val = 0, wb_addr = 0;
    unsigned wb_reg = 0;

    uint32_t pc = d->pc;
    const DspInsn *ins;
    uint32_t next_pc;

#define DSP_FETCH()                                                     \
    do {                                                                \
        next_pc = pc + 1;                                               \
        if (__builtin_expect(pc < DSP_PRAM_SIZE, 1)) {                  \
            uint32_t _op = d->p[pc];                                    \
            DspInsn *_slot = &d->icache[pc];                            \
            if (__builtin_expect(                                       \
                    _slot->kind == DSP_OP_INVALID || _slot->op != _op, 0)) { \
                _slot = dsp_icache_fill(d, pc, _op);                    \
            }                                                           \
            ins = _slot;                                                \
        } else {                                                        \
            ins = &nop_insn;                                            \
        }                                                               \
        goto *dispatch[ins->kind];                                      \
    } while (0)

#define DSP_STEP_END()                                                  \
    do {                                                                \
        if ((d->rep_remaining | (d->sr & SR_LF)) != 0) {                \
            if (d->rep_remaining && pc == d->rep_pc) {                  \
                if (--d->rep_remaining) {                               \
                    d->lc = d->rep_remaining;                           \
                    next_pc = d->rep_pc;                                \
                } else {                                                \
                    d->lc = d->rep_saved_lc;                            \
                }                                                       \
            } else if ((d->sr & SR_LF) && next_pc == d->la + 1 &&       \
                       (pc == d->la || pc + 1 == d->la)) {              \
                if (d->lc != 1 || (d->sr & SR_FV)) {                    \
                    d->lc = (d->lc - 1) & DSP_MASK24;                   \
                    next_pc = d->ssh[d->sp & 0xF];                      \
                } else {                                                \
                    uint32_t _h, _l, _ssr;                              \
                    stack_pop(d, NULL, &_ssr);                          \
                    stack_pop(d, &_h, &_l);                             \
                    d->la = _h;                                         \
                    d->lc = _l;                                         \
                    d->sr = (d->sr & ~(SR_LF | SR_FV)) |                \
                            (_ssr & (SR_LF | SR_FV));                   \
                }                                                       \
            }                                                           \
        }                                                               \
        pc = next_pc & DSP_MASK24;                                      \
        if (__builtin_expect(pc >= 0x1000u && pc != DSP_BOOT_ROM &&     \
                             !d->unimp_logged, 0)) {                    \
            d->unimp_logged = 1;                                        \
            info_report("cdj850-dsp: WILD PC P:%06x -> P:%06x "         \
                        "(op=0x%06x, sp=0x%x, sr=0x%06x, la=0x%06x, "   \
                        "lc=0x%x)",                                     \
                        d->pc, pc, ins->op, d->sp, d->sr, d->la, d->lc); \
        }                                                               \
        if (--budget == 0 ||                                            \
            (stop_sp < 0 ? d->yield_requested                           \
                         : (int32_t)d->sp == stop_sp)) {                \
            d->pc = pc;                                                 \
            return true;                                                \
        }                                                               \
        DSP_FETCH();                                                    \
    } while (0)

    DSP_FETCH();

lbl_PAR_EA_NOALU: {
    unsigned reg5 = ins->a & 0x1Fu;
    bool space_y = (ins->a >> 5) & 1;
    bool mem_to_reg = (ins->a >> 6) & 1;
    uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
    uint32_t addr = eval_ea(d, ins->imm, ext, NULL, true);

    if (mem_to_reg) {
        write_reg5(d, reg5, space_y ? mem_read_y(d, addr) : mem_read_x(d, addr));
    } else {
        uint32_t val = read_reg5(d, reg5);
        if (space_y) {
            mem_write_y(d, addr, val);
        } else {
            mem_write_x(d, addr, val);
        }
    }
    if (ins->words == 2) {
        next_pc = pc + 2;
    }
    DSP_STEP_END();
}

lbl_PAR_AA_NOALU: {
    unsigned reg5 = ins->a & 0x1Fu;
    bool space_y = (ins->a >> 5) & 1;
    bool mem_to_reg = (ins->a >> 6) & 1;
    uint32_t addr = ins->imm;

    if (mem_to_reg) {
        write_reg5(d, reg5, space_y ? mem_read_y(d, addr) : mem_read_x(d, addr));
    } else {
        uint32_t val = read_reg5(d, reg5);
        if (space_y) {
            mem_write_y(d, addr, val);
        } else {
            mem_write_x(d, addr, val);
        }
    }
    DSP_STEP_END();
}

lbl_PAR_XY_DUAL_NOALU: {
    uint32_t pk = ins->imm;
    unsigned xr = pk & 0xFu;
    unsigned yr = (pk >> 4) & 0xFu;
    unsigned xn = (pk >> 8) & 7u;
    unsigned yn = (pk >> 11) & 7u;
    unsigned xm = (pk >> 14) & 3u;
    unsigned ym = (pk >> 16) & 3u;
    bool rx = (pk >> 18) & 1u;
    bool ry = (pk >> 19) & 1u;
    uint32_t ax = d->r[xn];
    uint32_t ay = d->r[yn];
    uint32_t vx = rx ? mem_read_x(d, ax) : read_xy_reg(d, xr);
    uint32_t vy = ry ? mem_read_y(d, ay) : read_xy_reg(d, yr);

    if (xm) {
        d->r[xn] = agu_add(d, xn, xm == 1 ? sext24(d->n[xn]) :
                                  xm == 2 ? -1 : 1);
    }
    if (ym) {
        d->r[yn] = agu_add(d, yn, ym == 1 ? sext24(d->n[yn]) :
                                  ym == 2 ? -1 : 1);
    }
    if (rx) {
        write_xy_reg(d, xr, vx);
    } else {
        mem_write_x(d, ax, vx);
    }
    if (ry) {
        write_xy_reg(d, yr, vy);
    } else {
        mem_write_y(d, ay, vy);
    }
    DSP_STEP_END();
}

lbl_PAR_XY_DUAL: {
    uint32_t pk = ins->imm;
    unsigned xr = pk & 0xFu;
    unsigned yr = (pk >> 4) & 0xFu;
    unsigned xn = (pk >> 8) & 7u;
    unsigned yn = (pk >> 11) & 7u;
    unsigned xm = (pk >> 14) & 3u;
    unsigned ym = (pk >> 16) & 3u;
    bool rx = (pk >> 18) & 1u;
    bool ry = (pk >> 19) & 1u;
    uint32_t ax = d->r[xn];
    uint32_t ay = d->r[yn];
    uint32_t vx = rx ? mem_read_x(d, ax) : read_xy_reg(d, xr);
    uint32_t vy = ry ? mem_read_y(d, ay) : read_xy_reg(d, yr);

    if (xm) {
        d->r[xn] = agu_add(d, xn, xm == 1 ? sext24(d->n[xn]) :
                                  xm == 2 ? -1 : 1);
    }
    if (ym) {
        d->r[yn] = agu_add(d, yn, ym == 1 ? sext24(d->n[yn]) :
                                  ym == 2 ? -1 : 1);
    }
    exec_alu(d, ins->alu);
    if (rx) {
        write_xy_reg(d, xr, vx);
    } else {
        mem_write_x(d, ax, vx);
    }
    if (ry) {
        write_xy_reg(d, yr, vy);
    } else {
        mem_write_y(d, ay, vy);
    }
    DSP_STEP_END();
}

lbl_PAR_REG_REG_NOALU:
    write_reg5(d, ins->imm, read_reg5(d, ins->a));
    DSP_STEP_END();

lbl_PAR_SHORT_IMM_NOALU:
    write_reg5(d, ins->a, ins->imm);
    DSP_STEP_END();

lbl_PAR_EA_MOVE: {
    unsigned reg5 = ins->a & 0x1Fu;
    bool space_y = (ins->a >> 5) & 1;
    bool mem_to_reg = (ins->a >> 6) & 1;
    uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
    uint32_t addr = eval_ea(d, ins->imm, ext, NULL, true);

    if (mem_to_reg) {
        wb_val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        wb_reg = reg5;
        wb = WB_REG5;
    } else {
        wb_val = read_reg5(d, reg5);
        wb_addr = addr;
        wb = space_y ? WB_MEM_Y : WB_MEM_X;
    }
    if (ins->words == 2) {
        next_pc = pc + 2;
    }
    goto par_exec_alu;
}

lbl_PAR_AA_MOVE: {
    unsigned reg5 = ins->a & 0x1Fu;
    bool space_y = (ins->a >> 5) & 1;
    bool mem_to_reg = (ins->a >> 6) & 1;
    uint32_t addr = ins->imm;

    if (mem_to_reg) {
        wb_val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        wb_reg = reg5;
        wb = WB_REG5;
    } else {
        wb_val = read_reg5(d, reg5);
        wb_addr = addr;
        wb = space_y ? WB_MEM_Y : WB_MEM_X;
    }
    goto par_exec_alu;
}

lbl_PAR_IMM24:
    wb_val = dsp_ext(d, pc);
    wb_reg = ins->a;
    wb = WB_REG5;
    next_pc = pc + 2;
    goto par_exec_alu;

lbl_PAR_REG_REG:
    wb_val = read_reg5(d, ins->a);
    wb_reg = ins->imm;
    wb = WB_REG5;
    goto par_exec_alu;

lbl_PAR_SHORT_IMM:
    wb_val = ins->imm;
    wb_reg = ins->a;
    wb = WB_REG5;
    goto par_exec_alu;

lbl_PAR_UPDATE_RN:
    eval_ea(d, ins->a, 0, NULL, true);
    wb = WB_NONE;
par_exec_alu:
    if (ins->alu) {
        exec_alu(d, ins->alu);
    }
    switch (wb) {
    case WB_NONE:
        break;
    case WB_REG5:
        write_reg5(d, wb_reg, wb_val);
        break;
    case WB_MEM_X:
        mem_write_x(d, wb_addr, wb_val);
        break;
    case WB_MEM_Y:
        mem_write_y(d, wb_addr, wb_val);
        break;
    }
    DSP_STEP_END();

lbl_PAR_IFCC:
    if (eval_cc(d, ins->a & 0xFu)) {
        uint32_t saved_sr = d->sr;
        if (ins->alu) {
            exec_alu(d, ins->alu);
        }
        if (!(ins->a & 0x10u)) {
            d->sr = saved_sr;
        }
    }
    DSP_STEP_END();

lbl_PAR_CLASS1: {
    uint32_t pk = ins->imm;
    unsigned memory_reg = pk & 0x1Fu;
    unsigned acc_reg = (pk >> 5) & 0x1Fu;
    unsigned input_reg = (pk >> 10) & 0x1Fu;
    unsigned ea = (pk >> 15) & 0x3Fu;
    bool y = (pk >> 21) & 1;
    bool read = (pk >> 22) & 1;
    uint32_t ext = (ins->words == 2) ? dsp_ext(d, pc) : 0;
    uint32_t addr = eval_ea(d, ea, ext, NULL, true);
    uint32_t value = read ? (ea == 0x34 ? ext :
                               y ? mem_read_y(d, addr) : mem_read_x(d, addr)) :
                           read_reg6(d, memory_reg);
    uint32_t acc = read_reg6(d, acc_reg);

    if (ins->alu) {
        exec_alu(d, ins->alu);
    }
    write_reg6(d, input_reg, acc);
    if (read) {
        write_reg6(d, memory_reg, value);
    } else if (y) {
        mem_write_y(d, addr, value);
    } else {
        mem_write_x(d, addr, value);
    }
    if (ins->words == 2) {
        next_pc = pc + 2;
    }
    DSP_STEP_END();
}

lbl_NOP:
    DSP_STEP_END();

lbl_RTS: {
    uint32_t h;
    stack_pop(d, &h, NULL);
    next_pc = h;
    DSP_STEP_END();
}

lbl_JMP12:
    next_pc = ins->imm;
    DSP_STEP_END();

lbl_JSR12:
    stack_push(d, pc + 1, d->sr);
    next_pc = ins->imm;
    DSP_STEP_END();

lbl_JCC12:
    if (eval_cc(d, ins->a)) {
        next_pc = ins->imm;
    }
    DSP_STEP_END();

lbl_JSCC12:
    if (eval_cc(d, ins->a)) {
        stack_push(d, pc + 1, d->sr);
        next_pc = ins->imm;
    }
    DSP_STEP_END();

lbl_SHIFT56: {
    bool left = (ins->a >> 2) & 1;
    uint64_t value = (ins->a & 2) ? d->b : d->a;
    unsigned n = (ins->a & 8) ? ins->imm : (read_reg6(d, ins->imm) & 0x3Fu);
    bool carry = n ? (left ? (n <= 56 && ((value >> (56 - n)) & 1)) :
                            ((value >> (n - 1)) & 1)) : (d->sr & SR_C);
    uint64_t result = left ? (value << n) & DSP_MASK56 :
                             (uint64_t)(sext56(value) >> n) & DSP_MASK56;
    bool overflow = left && (sext56(result) >> n) != sext56(value);
    if (ins->a & 1) {
        d->b = result;
    } else {
        d->a = result;
    }
    set_ccr56(d, result, carry, overflow);
    DSP_STEP_END();
}

lbl_MPY_IMM: {
    unsigned n = ins->imm & 0x1Fu;
    unsigned src_reg = (ins->imm >> 5) & 0x3Fu;
    uint64_t *dst = (ins->a & 8u) ? &d->b : &d->a;
    int64_t prod = sext56(pack_acc24(read_reg6(d, src_reg))) >> n;
    int64_t res = ((ins->a & 2u) ? sext56(*dst) : 0) +
                  ((ins->a & 4u) ? -prod : prod);
    if (ins->a & 1u) {
        uint64_t r = (uint64_t)res;
        if ((r & 0xFFFFFFULL) > 0x800000ULL ||
            ((r & 0xFFFFFFULL) == 0x800000ULL && (r & 0x1000000ULL))) {
            res += 0x1000000LL;
        }
        res &= ~0xFFFFFFLL;
    }
    *dst = (uint64_t)res & DSP_MASK56;
    set_ccr56(d, *dst, false, false);
    DSP_STEP_END();
}

lbl_BIT_OP: {
    uint8_t mode = ins->a & 7u;
    bool space_y = (ins->a >> 3) & 1;
    bool bit_set_cond = (ins->a >> 4) & 1;
    uint8_t subop = (ins->a >> 6) & 3u;
    uint32_t val = 0, addr = 0;

    switch (mode) {
    case 0:
        addr = 0xFFFF80u + ins->alu;
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    case 1:
        val = read_reg6(d, ins->alu);
        break;
    case 2:
        addr = 0xFFFFC0u + ins->alu;
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    case 3:
        addr = eval_ea(d, ins->alu, ins->words == 2 ? dsp_ext(d, pc) : 0, NULL, true);
        if (ins->words == 2) {
            next_pc = pc + 2;
        }
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    default:
        addr = ins->alu;
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    }
    if (val & ins->imm) {
        d->sr |= SR_C;
    } else {
        d->sr &= ~SR_C;
    }
    if (subop != 2) {
        uint32_t nval = (subop == 1) ? (val ^ ins->imm) :
                        (bit_set_cond ? (val | ins->imm) :
                                        (val & ~ins->imm));
        if (mode == 1) {
            write_reg6(d, ins->alu, nval);
        } else if (space_y) {
            mem_write_y(d, addr, nval);
        } else {
            mem_write_x(d, addr, nval);
        }
    }
    DSP_STEP_END();
}

lbl_BIT_BRANCH: {
    uint8_t mode = ins->a & 7u;
    bool space_y = (ins->a >> 3) & 1;
    bool bit_set_cond = (ins->a >> 4) & 1;
    bool is_jsr = (ins->a >> 5) & 1;
    uint32_t val = 0, addr = 0;

    switch (mode) {
    case 0:
        addr = 0xFFFF80u + ins->alu;
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    case 1:
        val = read_reg6(d, ins->alu);
        break;
    case 2:
        addr = 0xFFFFC0u + ins->alu;
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    case 3:
        addr = eval_ea(d, ins->alu, dsp_ext(d, pc), NULL, true);
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    default:
        addr = ins->alu;
        val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
        break;
    }
    bool was_set = (val & ins->imm) != 0;
    if (bit_set_cond ? was_set : !was_set) {
        if (is_jsr) {
            stack_push(d, pc + 2, d->sr);
        }
        next_pc = dsp_ext(d, pc) & DSP_MASK24;
    } else {
        next_pc = pc + 2;
    }
    DSP_STEP_END();
}

lbl_COLD:
    d->pc = pc;
    if (!step_one_cold(d, ins, pc, &next_pc)) {
        return false;
    }
    if (d->halted || d->wait_mode || d->stop_mode) {
        d->pc = next_pc & DSP_MASK24;
        return true;
    }
    DSP_STEP_END();

#undef DSP_FETCH
#undef DSP_STEP_END
}

static bool step_one(Dsp56300 *d)
{
    return dsp_exec(d, 1, -1);
}

static void dsp_run_cycles(Dsp56300 *d, unsigned budget)
{
    d->yield_requested = false;
    dsp_exec(d, budget, -1);
}

/* Can an ESAI (IPL 2) interrupt be taken right now? Not inside REP, and
 * only when SR I1:I0 (bits 9:8) does not mask level 2. */
static bool dsp_esai_irq_ready(Dsp56300 *d)
{
    return !d->halted && !d->stop_mode && !d->rep_remaining &&
           ((d->sr >> SR_IMASK_SHIFT) & 3) <= 2;
}

static void dsp_fire_irq(Dsp56300 *d, uint32_t vec)
{
    if (!dsp_esai_irq_ready(d)) {
        return;
    }
    uint32_t interrupted_pc = d->pc;
    uint32_t vector_pc = (d->vba + vec) & DSP_MASK24;
    uint32_t op = mem_read_p(d, vector_pc);

    d->wait_mode = false;
    /* A JSR in the vector promotes a fast interrupt to a long interrupt.
     * Its return address is the interrupted PC, not the end of the vector.
     * The handler's RTI restores that PC and SR from the single stack frame.
     */
    if ((op & 0xFFF000u) == 0x0D0000u || op == 0x0BF080u) {
        uint32_t old_sp = d->sp;
        stack_push(d, interrupted_pc, d->sr);
        d->sr = (d->sr & ~(SR_LF | SR_S0 | SR_S1 | (1u << 17) | 0x300u)) |
                0x300u;
        d->pc = op == 0x0BF080u ? mem_read_p(d, vector_pc + 1) : op & 0xFFFu;
        dsp_exec(d, 256, (int32_t)old_sp);
    } else {
        /* Fast vectors execute two instruction words without a stack frame. */
        d->pc = vector_pc;
        step_one(d);
        if (d->pc == vector_pc + 1) {
            step_one(d);
        }
        d->pc = interrupted_pc;
    }
}

/*
 * The serial slots run in bursts, one tick at a time. Two things matter:
 * - Frames are counted from the virtual clock, so a late tick catches up
 *   instead of losing audio time (a fixed 44/45 frames per tick produced
 *   only ~31k frames per virtual second whenever ticks ran late).
 * - The tick is short. The DSP holds one 84-frame block request in flight:
 *   MAIN can only answer between bursts, and a block is 90 serial words.
 *   With 1 ms bursts (88 slots) a block always spanned two bursts, so the
 *   DSP got exactly 500 blocks/s = 42000 frames/s and underran; 0.5 ms
 *   bursts complete a request in about 1.5 ms, inside the 1.9 ms a block
 *   lasts.
 */
#define DSP_AUDIO_RATE     44100u
#define DSP_AUDIO_TICK_NS  500000
#define DSP_AUDIO_MAX      4410u        /* catch-up cap after a stall */
#define DSP_WORKER_CHUNK   22u          /* ~500 us per lock acquisition */

static inline void dsp_host_lock(Dsp56300 *d)
{
#ifndef DSP_TEST_COMMON
    if (d->threaded) {
        qatomic_inc(&d->host_waiting);
        qemu_mutex_lock(&d->lock);
        qatomic_dec(&d->host_waiting);
    }
#else
    (void)d;
#endif
}

static inline void dsp_host_unlock(Dsp56300 *d)
{
#ifndef DSP_TEST_COMMON
    if (d->threaded) {
        qemu_mutex_unlock(&d->lock);
    }
#else
    (void)d;
#endif
}

static void dsp_run_slot(Dsp56300 *d)
{
    uint32_t rcr = d->x_io[XIO_ESAI_RCR];
    uint32_t tcr = d->x_io[XIO_ESAI_TCR];

    d->tfs_slot = !d->tfs_slot;
    dsp_run_cycles(d, d->slot_budget);

    /* ESAI RX0 / RX1 from SSI0 / SSI1. With RIE set the word waits
     * in the FIFO until the interrupt can be taken: a dropped RX
     * interrupt loses a word, and the DSP's block receiver counts
     * exactly 84 words per block (LFSR at P:$3CD..$3DF) before it
     * clears RIE and can request the next block. */
    if ((rcr & 1) && (!(rcr & (1u << 22)) || dsp_esai_irq_ready(d))) {
        uint32_t w0 = 0, w1 = 0;
        bool have = false;

#ifndef DSP_TEST_COMMON
        if (d->threaded) {
            qemu_mutex_lock(&d->rx_lock);
        }
#endif
        if (d->ssi_rx[0].count > 0 || d->ssi_rx[1].count > 0) {
            fifo_pop(&d->ssi_rx[0], &w0);
            fifo_pop(&d->ssi_rx[1], &w1);
            have = true;
        }
#ifndef DSP_TEST_COMMON
        if (d->threaded) {
            qemu_mutex_unlock(&d->rx_lock);
        }
#endif
        if (have) {
            d->x_io[XIO_ESAI_RX0] = w0;
            d->x_io[XIO_ESAI_RX1] = w1;
            d->esai_rx_pending = rcr & 0xF;
            d->x_io[XIO_ESAI_SAISR] &= ~SAISR_RFS;
            if (d->tfs_slot) {
                d->x_io[XIO_ESAI_SAISR] |= SAISR_RFS;
            }
            d->x_io[XIO_ESAI_SAISR] |= SAISR_RDF;
            if (rcr & (1u << 22)) { /* RIE */
                dsp_fire_irq(d, 0x0030u);
            }
        }
    }

    /* ESAI TX0 -> host audio */
    if (tcr & 1) {
        /* Shift the holding register, then request the next slot. */
        if (d->esai_tx_written && d->audio_sink) {
            int16_t sample = sext24(d->x_io[XIO_ESAI_TX0]) >> 8;
            if (d->tfs_slot) {
                d->left_sample = sample;
                d->have_left = true;
            } else if (d->have_left) {
                cdj_host_audio_put(d->audio_sink, d->left_sample, sample);
                d->have_left = false;
            }
        } else if (d->tfs_slot) {
            d->have_left = false;
        }
        d->esai_tx_written = false;
        if (tcr & (1u << 22)) { /* TIE */
            dsp_fire_irq(d, 0x0038u);
        }
    }
}

static void dsp_debug_tick(Dsp56300 *d, int64_t now)
{
    /* Main instructions are interleaved with the serial slots above. */
    if (d->audio_tick == 6000 && d->debug_enabled) {
        unsigned k;

        for (k = 0x30; k < 0x40; k += 2) {
            info_report("cdj850-dsp: P:%02x = %06x %06x", k,
                        d->p[k], d->p[k + 1]);
        }
        for (k = 0xB0; k < 0xBA; k++) {
            info_report("cdj850-dsp: X:FFFF%02X = %06x", k,
                        d->x_io[IO_OFF(0xFFFF00u + k)]);
        }
        info_report("cdj850-dsp: IPRC=%06x IPRP=%06x SR=%06x OMR=%06x "
                    "VBA=%06x", d->x_io[IO_OFF(0xFFFFFF)],
                    d->x_io[IO_OFF(0xFFFFFE)], d->sr, d->omr, d->vba);
    }
    if (now >= d->debug_next_ns && d->debug_enabled) {
        uint64_t rx_words;

        /* Every 500 ms of virtual time (ticks are clock-based now). */
        d->debug_next_ns = now + 500000000LL;
#ifndef DSP_TEST_COMMON
        if (d->threaded) {
            qemu_mutex_lock(&d->rx_lock);
            rx_words = d->esai_rx_words;
            qemu_mutex_unlock(&d->rx_lock);
        } else {
            rx_words = d->esai_rx_words;
        }
#else
        rx_words = d->esai_rx_words;
#endif
        info_report("cdj850-dsp: %" PRId64 " ms tick=%u P:%06x TCR=0x%06x RCR=0x%06x "
                    "Y0=0x%06x Y20=0x%06x Y22=0x%06x X0C=0x%06x X0D=0x%06x "
                    "X1E=0x%06x X1F=0x%06x X38=0x%06x PF10=%u",
                    now / 1000000, d->audio_tick, d->pc, d->x_io[XIO_ESAI_TCR],
                    d->x_io[XIO_ESAI_RCR], d->y[0], d->y[0x20], d->y[0x22],
                    d->x[0x0c], d->x[0x0d], d->x[0x1e], d->x[0x1f],
                    d->x[0x38], (io_read_y(d, 0xFFFFF5) >> 10) & 1);
        info_report("cdj850-dsp: ESAI RX=%" PRIu64 " TX0=%" PRIu64
                    " nonzero=%" PRIu64 " peak=0x%06x",
                    rx_words, d->esai_tx_words, d->esai_tx_nonzero,
                    d->esai_tx_peak);
        d->esai_tx_peak = 0;
    }
}

static void dsp_audio_tick(void *opaque)
{
    Dsp56300 *d = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t acc = (uint64_t)(now > d->audio_last_ns ? now - d->audio_last_ns : 0) *
                   DSP_AUDIO_RATE + d->audio_frac;
    unsigned frames = acc / 1000000000ULL;
    unsigned i;

    d->audio_frac = acc % 1000000000ULL;
    d->audio_last_ns = now;
    if (frames > DSP_AUDIO_MAX) {
        frames = DSP_AUDIO_MAX;
    }
    d->audio_tick++;

    if (d->boot_state == BOOT_RUN && !d->halted) {
        for (i = 0; i < frames * 2; i++) {
            dsp_run_slot(d);
        }
        dsp_debug_tick(d, now);
    }

    timer_mod(d->audio_timer, now + DSP_AUDIO_TICK_NS);
}

#ifndef DSP_TEST_COMMON
static void *dsp_worker_thread(void *opaque)
{
    Dsp56300 *d = opaque;

    while (qatomic_read(&d->running)) {
        if (qatomic_read(&d->paused) || qatomic_read(&d->host_waiting)) {
            g_usleep(qatomic_read(&d->paused) ? 1000 : 0);
            continue;
        }

        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        qemu_mutex_lock(&d->lock);
        if (!qatomic_read(&d->running)) {
            qemu_mutex_unlock(&d->lock);
            break;
        }
        if (d->boot_state != BOOT_RUN || d->halted) {
            d->audio_last_ns = now;
            d->audio_frac = 0;
            qemu_mutex_unlock(&d->lock);
            g_usleep(500);
            continue;
        }

        uint64_t acc = (uint64_t)(now > d->audio_last_ns ?
                                  now - d->audio_last_ns : 0) *
                       DSP_AUDIO_RATE + d->audio_frac;
        unsigned frames = acc / 1000000000ULL;

        if (frames == 0) {
            qemu_mutex_unlock(&d->lock);
            g_usleep(250);
            continue;
        }
        if (frames > DSP_AUDIO_MAX) {
            frames = DSP_AUDIO_MAX;
            acc = (uint64_t)frames * 1000000000ULL + (acc % 1000000000ULL);
        }

        unsigned chunk = frames < DSP_WORKER_CHUNK ? frames : DSP_WORKER_CHUNK;
        unsigned done_frames = 0;

        for (unsigned f = 0; f < chunk; f++) {
            if (qatomic_read(&d->host_waiting) || !qatomic_read(&d->running)) {
                break;
            }
            dsp_run_slot(d);
            dsp_run_slot(d);
            done_frames++;
        }
        if (done_frames == frames) {
            d->audio_last_ns = now;
            d->audio_frac = acc % 1000000000ULL;
        } else if (done_frames > 0) {
            uint64_t rem = acc - (uint64_t)done_frames * 1000000000ULL;

            d->audio_last_ns = now - (int64_t)(rem / DSP_AUDIO_RATE);
            d->audio_frac = rem % DSP_AUDIO_RATE;
        }
        if (done_frames > 0) {
            d->audio_tick++;
            dsp_debug_tick(d, now);
        }
        qemu_mutex_unlock(&d->lock);
    }
    return NULL;
}

static void dsp_vm_state_cb(void *opaque, bool running, RunState state)
{
    Dsp56300 *d = opaque;

    (void)state;
    qatomic_set(&d->paused, !running);
    if (!running && d->threaded) {
        /* Wait until the worker exits any in-flight slot before savevm. */
        qemu_mutex_lock(&d->lock);
        qemu_mutex_unlock(&d->lock);
    }
}

static int dsp_post_load(void *opaque, int version_id)
{
    Dsp56300 *d = opaque;

    (void)version_id;
    memset(d->icache, 0, sizeof(d->icache));
    return 0;
}

static bool dsp_has_timer(void *opaque, int version_id)
{
    Dsp56300 *d = opaque;

    (void)version_id;
    return d->audio_timer != NULL;
}

static const VMStateDescription vmstate_cdj850_dsp56300 = {
    .name = "cdj850-dsp56300",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dsp_post_load,
    .fields = (const VMStateField[]) {
        CDJ_VMSTATE_SPAN(Dsp56300, a, ssi_rx),
        VMSTATE_TIMER_PTR_TEST(audio_timer, Dsp56300, dsp_has_timer),
        CDJ_VMSTATE_SPAN(Dsp56300, audio_tick, shi_tx_logged),
        VMSTATE_BOOL(dspdreq_pin, Dsp56300),
        VMSTATE_UINT32(main_pfdr, Dsp56300),
        VMSTATE_UINT8_ARRAY(irq7_q, Dsp56300, 32),
        VMSTATE_UINT32(irq7_head, Dsp56300),
        VMSTATE_UINT32(irq7_tail, Dsp56300),
        VMSTATE_UINT32(irq7_count, Dsp56300),
        VMSTATE_UINT32_ARRAY(p, Dsp56300, DSP_PRAM_SIZE),
        VMSTATE_UINT32_ARRAY(x, Dsp56300, DSP_DRAM_SIZE),
        VMSTATE_UINT32_ARRAY(y, Dsp56300, DSP_DRAM_SIZE),
        VMSTATE_UINT32_ARRAY(x_io, Dsp56300, DSP_IO_SIZE),
        VMSTATE_UINT32_ARRAY(y_io, Dsp56300, DSP_IO_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void dsp_exit_cb(Notifier *n, void *data)
{
    Dsp56300 *d = container_of(n, Dsp56300, exit);

    (void)data;
    qatomic_set(&d->running, false);
    qemu_thread_join(&d->thread);
}

static void dsp56300_reset(void *opaque)
{
    Dsp56300 *d = opaque;
    unsigned i;

    dsp_host_lock(d);
    if (d->threaded) {
        qemu_mutex_lock(&d->rx_lock);
    }
    memset(d->ssi_rx, 0, sizeof(d->ssi_rx));
    d->esai_rx_words = 0;
    if (d->threaded) {
        qemu_mutex_unlock(&d->rx_lock);
        qemu_mutex_lock(&d->irq_lock);
    }
    memset(d->irq7_q, 0, sizeof(d->irq7_q));
    d->irq7_head = d->irq7_tail = d->irq7_count = 0;
    if (d->threaded) {
        qemu_mutex_unlock(&d->irq_lock);
    }

    memset(&d->a, 0, offsetof(Dsp56300, ssi_rx) - offsetof(Dsp56300, a));
    d->audio_tick = 0;
    d->audio_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    d->audio_frac = 0;
    d->debug_next_ns = 0;
    d->left_sample = 0;
    d->have_left = false;
    d->tfs_slot = false;
    d->esai_tx_written = false;
    d->esai_rx_pending = 0;
    d->unimp_logged = 0;
    d->esai_tx_words = 0;
    d->esai_tx_nonzero = 0;
    d->esai_tx_peak = 0;
    d->shi_last_tx = 0;
    d->shi_tx_logged = 0;
    qatomic_set(&d->main_pfdr, 0);

    memset(d->p, 0, sizeof(d->p));
    memset(d->icache, 0, sizeof(d->icache));
    memset(d->x, 0, sizeof(d->x));
    memset(d->y, 0, sizeof(d->y));
    memset(d->x_io, 0, sizeof(d->x_io));
    memset(d->y_io, 0, sizeof(d->y_io));

    for (i = 0; i < 8; i++) {
        d->m[i] = DSP_MASK24;
    }
    d->sr = 0x0300u; /* I1:I0 = 3 at reset */
    d->omr = 0x05u;  /* Boot mode 5 (SHI slave SPI) */
    d->pc = DSP_BOOT_ROM;
    d->halted = true;
    d->boot_state = BOOT_COUNT;
    d->dspdreq_pin = cdj850_intc_get_pin(7);

    if (d->audio_timer) {
        timer_mod(d->audio_timer, d->audio_last_ns + DSP_AUDIO_TICK_NS);
    }
    dsp_host_unlock(d);
}
#endif

void dsp56300_shi_tx(uint8_t byte)
{
    Dsp56300 *d = dsp;
    uint32_t w;

    if (!d) {
        return;
    }
    dsp_host_lock(d);
    /* Finish instructions after a bootstrap GPIO edge before the next word. */
    dsp_run_cycles(d, 64);
    d->shi_acc = (d->shi_acc << 8) | byte;
    if (++d->shi_nbytes < 3) {
        dsp_host_unlock(d);
        return;
    }
    w = d->shi_acc & DSP_MASK24;
    d->shi_acc = 0;
    d->shi_nbytes = 0;

    if (d->boot_state != BOOT_RUN) {
        switch (d->boot_state) {
        case BOOT_COUNT:
            if (w > 0x10000u) {
                break;
            }
            d->boot_count = d->boot_left = w;
            d->boot_state = BOOT_ADDR;
            break;
        case BOOT_ADDR:
            d->boot_addr = d->boot_entry = w;
            if (d->boot_left == 0) {
                d->boot_state = BOOT_RUN;
                d->pc = d->boot_entry;
                d->halted = false;
                dsp_run_cycles(d, 200000);
            } else {
                d->boot_state = BOOT_DATA;
            }
            break;
        case BOOT_DATA:
            mem_write_p(d, d->boot_addr++, w);
            if (--d->boot_left == 0) {
                d->boot_segs++;
                if (cdj850_trace()) {
                    info_report("cdj850: dsp56371 exec segment %u: %u words "
                                "@P:%06x (%" PRIu64 " ms)", d->boot_segs,
                                d->boot_count, d->boot_entry, cdj850_vms());
                }
                d->boot_state = BOOT_RUN;
                d->pc = d->boot_entry;
                d->halted = false;
                dsp_run_cycles(d, 200000);
            }
            break;
        default:
            break;
        }
        dsp_host_unlock(d);
        return;
    }

    if (d->debug_enabled) {
        /* Log transitions: MAIN repeats 2-word frames every few ms, so a
         * word differing from the one two positions back is a change. */
        static uint32_t shi_prev[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        static unsigned shi_pos;

        if (shi_prev[shi_pos & 1] != w) {
            info_report("cdj850-dsp: SHI RX 0x%06x (was 0x%06x, %" PRIu64
                        " ms)", w, shi_prev[shi_pos & 1], cdj850_vms());
            shi_prev[shi_pos & 1] = w;
        }
        shi_pos++;
    }
    fifo_push(&d->shi_rx, w);
    dsp_run_cycles(d, 4000);
    dsp_host_unlock(d);
}

uint8_t dsp56300_shi_rx(void)
{
    Dsp56300 *d = dsp;
    uint8_t b;

    if (!d) {
        return 0;
    }
    dsp_host_lock(d);
    if (d->shi_tx_bytes_left == 0) {
        if (!fifo_pop(&d->shi_tx, &d->shi_tx_word)) {
            dsp_host_unlock(d);
            return 0;
        }
        d->shi_tx_bytes_left = 3;
    }
    d->shi_tx_bytes_left--;
    b = (uint8_t)((d->shi_tx_word >> (d->shi_tx_bytes_left * 8)) & 0xFFu);
    dsp_host_unlock(d);
    return b;
}

void dsp56300_shi_cs(bool active)
{
    Dsp56300 *d = dsp;

    if (!d) {
        return;
    }
    if (!active) {
        dsp_host_lock(d);
        d->shi_nbytes = 0;
        d->shi_acc = 0;
        dsp_host_unlock(d);
    }
}

void dsp56300_set_pf(uint16_t pfdrh, uint16_t pfdrl)
{
    Dsp56300 *d = dsp;

    if (!d) {
        return;
    }
#ifdef DSP_TEST_COMMON
    d->main_pfdr = ((uint32_t)pfdrh << 16) | pfdrl;
#else
    qatomic_set(&d->main_pfdr, ((uint32_t)pfdrh << 16) | pfdrl);
#endif
}

void dsp56300_esai_rx_push(unsigned ch, uint32_t word24)
{
    Dsp56300 *d = dsp;

    if (!d || ch > 1) {
        return;
    }
#ifndef DSP_TEST_COMMON
    if (d->threaded) {
        qemu_mutex_lock(&d->rx_lock);
    }
#endif
    d->esai_rx_words++;
    fifo_push(&d->ssi_rx[ch], word24);
#ifndef DSP_TEST_COMMON
    if (d->threaded) {
        qemu_mutex_unlock(&d->rx_lock);
    }
#endif
}

void dsp56300_init(void)
{
    static const CdjHostAudioCfg audio_cfg = {
        .env = "CDJ_DSP_AUDIO",
        .voice = "cdj850-dsp-esai0",
        .label = "dsp56371",
        .source = "ESAI0",
    };
    Dsp56300 *d = g_new0(Dsp56300, 1);
    unsigned i;

    for (i = 0; i < 8; i++) {
        d->m[i] = DSP_MASK24;
    }
    d->sr = 0x0300u; /* I1:I0 = 3 at reset */
    d->omr = 0x05u;  /* Boot mode 5 (SHI slave SPI) */
    d->pc = DSP_BOOT_ROM;
    d->halted = true;
    d->boot_state = BOOT_COUNT;
    d->dspdreq_pin = cdj850_intc_get_pin(7);

    {
        const char *e = getenv("CDJ850_DSP_IPS");

        d->slot_budget = e && atoi(e) > 0 ? (unsigned)atoi(e) : 256;
    }
    d->debug_enabled = getenv("CDJ850_DSP_DEBUG") != NULL;
    {
        const char *e = getenv("CDJ850_DSP_PLAY_PF");

        if (e) {
            d->play_pf_override = true;
            d->play_pf_bit = (unsigned)atoi(e);
        }
    }
    d->audio_sink = cdj_host_audio_open(&audio_cfg);
    d->audio_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    dsp = d;

#ifndef DSP_TEST_COMMON
    qemu_mutex_init(&d->lock);
    qemu_mutex_init(&d->rx_lock);
    qemu_mutex_init(&d->irq_lock);
    qemu_register_reset(dsp56300_reset, d);
    vmstate_register_any(NULL, &vmstate_cdj850_dsp56300, d);
    d->threaded = !icount_enabled();
    if (d->threaded) {
        d->irq_bh = qemu_bh_new(dsp_irq_bh, d);
        d->running = true;
        d->paused = !runstate_is_running();
        d->vmstate = qemu_add_vm_change_state_handler(dsp_vm_state_cb, d);
        d->exit.notify = dsp_exit_cb;
        qemu_add_exit_notifier(&d->exit);
        qemu_thread_create(&d->thread, "cdj850-dsp", dsp_worker_thread, d,
                           QEMU_THREAD_JOINABLE);
        return;
    }
#endif
    d->audio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_audio_tick, d);
    timer_mod(d->audio_timer, d->audio_last_ns + DSP_AUDIO_TICK_NS);
}
