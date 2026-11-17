/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_host_audio.h"
#include "qemu/timer.h"
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

typedef struct {
    uint32_t p[DSP_PRAM_SIZE];
    uint32_t x[DSP_DRAM_SIZE];
    uint32_t y[DSP_DRAM_SIZE];
    uint32_t x_io[DSP_IO_SIZE];
    uint32_t y_io[DSP_IO_SIZE];

    /* Data ALU */
    uint64_t a, b;          /* 56-bit in low 56 bits */
    uint32_t x0, x1, y0, y1;

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
     * about 4000. 64 (5.6 MIPS) keeps the block-request loop fed; 256
     * costs so much host time that MAIN misses USB timing (the virtual
     * clock follows the host). $CDJ850_DSP_IPS overrides. */
    unsigned slot_budget;
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

static uint32_t read_acc24_limited(Dsp56300 *d, uint64_t acc)
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

static void update_pedr_dspdreq(Dsp56300 *d)
{
    /* Port E bit 4 (PE4) = DSPDREQ -> MAIN IRQ7 when configured as GPIO out */
    uint32_t pedr = d->y_io[YIO_PEDR];
    uint32_t peddr = d->y_io[YIO_PEDDR];
    uint32_t pecr = d->y_io[YIO_PECR];

    if ((peddr & (1u << 4)) && !(pecr & (1u << 4))) {
        bool lvl = (pedr & (1u << 4)) != 0;
        if (getenv("CDJ850_DSP_DEBUG") && cdj850_intc_get_pin(7) != lvl) {
            info_report("cdj850-dsp: DSPDREQ=%d at P:%06x (%" PRIu64 " ms)",
                        lvl, d->pc, cdj850_vms());
        }
        if (cdj850_intc_get_pin(7) != lvl) {
            /* Let MAIN observe the GPIO edge before another interpreter burst. */
            d->yield_requested = true;
        }
        cdj850_intc_set_pin(7, lvl);
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
        if (getenv("CDJ850_DSP_DEBUG") && d->shi_tx_logged < 40 &&
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
        const char *e = getenv("CDJ850_DSP_PLAY_PF");
        bool play = e ? ((cdj850_port_pfdrl() & (1u << (unsigned)atoi(e))) != 0)
                      : ((cdj850_port_pfdrh() & 1u) != 0);

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

static inline uint32_t mem_read_x(Dsp56300 *d, uint32_t addr)
{
    addr &= DSP_MASK24;
    if (addr >= DSP_IO_BASE) {
        return io_read_x(d, addr);
    }
    return addr < DSP_DRAM_SIZE ? d->x[addr] : 0;
}

static inline void mem_write_x(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    addr &= DSP_MASK24;
    if (addr >= DSP_IO_BASE) {
        io_write_x(d, addr, val);
        return;
    }
    if (addr < DSP_DRAM_SIZE) {
        d->x[addr] = val & DSP_MASK24;
    }
}

static inline uint32_t mem_read_y(Dsp56300 *d, uint32_t addr)
{
    addr &= DSP_MASK24;
    if (addr >= DSP_IO_BASE) {
        return io_read_y(d, addr);
    }
    return addr < DSP_DRAM_SIZE ? d->y[addr] : 0;
}

static inline void mem_write_y(Dsp56300 *d, uint32_t addr, uint32_t val)
{
    addr &= DSP_MASK24;
    if (addr >= DSP_IO_BASE) {
        io_write_y(d, addr, val);
        return;
    }
    if (addr < DSP_DRAM_SIZE) {
        d->y[addr] = val & DSP_MASK24;
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
    }
}

/* Stack push / pop (SP bits 3:0 = index 0..15) */
static void stack_push(Dsp56300 *d, uint32_t h, uint32_t l)
{
    unsigned idx = ((d->sp & 0xF) + 1) & 0xF;

    d->sp = (d->sp & ~0xFu) | idx;
    d->ssh[idx] = h & DSP_MASK24;
    d->ssl[idx] = l & DSP_MASK24;
}

static void stack_pop(Dsp56300 *d, uint32_t *h, uint32_t *l)
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
static uint32_t agu_add(Dsp56300 *d, unsigned reg, int32_t delta)
{
    uint32_t r = d->r[reg] & DSP_MASK24;
    uint32_t m = d->m[reg] & DSP_MASK24;

    if (m == DSP_MASK24 || m >= 0x8000u) {
        return (r + (uint32_t)delta) & DSP_MASK24;
    }
    /* Modulo (m + 1) */
    uint32_t mod = m + 1;
    uint32_t p2 = 1;

    while (p2 < mod) {
        p2 <<= 1;
    }
    uint32_t base = r & ~(p2 - 1);
    int32_t off = (int32_t)(r - base) + delta;

    while (off < 0) {
        off += (int32_t)mod;
    }
    while (off >= (int32_t)mod) {
        off -= (int32_t)mod;
    }
    return (base + (uint32_t)off) & DSP_MASK24;
}

/* Compute EA from 6-bit MMM RRR (does not handle mode 6 ext unless passed) */
static uint32_t eval_ea(Dsp56300 *d, unsigned mmmrrr, uint32_t ext,
                        bool *used_ext, bool writeback)
{
    unsigned mmm = (mmmrrr >> 3) & 7;
    unsigned rrr = mmmrrr & 7;
    uint32_t addr = d->r[rrr];
    int32_t nval = sext24(d->n[rrr]);

    if (used_ext) {
        *used_ext = false;
    }
    switch (mmm) {
    case 0: /* (Rn)-Nn */
        if (writeback) {
            d->r[rrr] = agu_add(d, rrr, -nval);
        }
        return addr;
    case 1: /* (Rn)+Nn */
        if (writeback) {
            d->r[rrr] = agu_add(d, rrr, nval);
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
        return agu_add(d, rrr, nval);
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
static bool eval_cc(Dsp56300 *d, unsigned cc)
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
static void set_ccr56(Dsp56300 *d, uint64_t res, bool carry, bool ovf)
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

static void set_ccr_logic(Dsp56300 *d, uint64_t acc)
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
static uint32_t read_reg6(Dsp56300 *d, unsigned reg)
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

static void write_reg6(Dsp56300 *d, unsigned reg, uint32_t val)
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
static uint32_t read_reg5(Dsp56300 *d, unsigned reg5)
{
    return read_reg6(d, reg5 & 0x1F);
}

static void write_reg5(Dsp56300 *d, unsigned reg5, uint32_t val)
{
    write_reg6(d, reg5 & 0x1F, val);
}

/* Execute 8-bit Data ALU opcode (bits 7:0 of parallel instructions) */
static void exec_alu(Dsp56300 *d, uint8_t alu)
{
    if (alu == 0x00) {
        return; /* MOVE */
    }
    bool is_mac = (alu & 0x80) != 0;

    if (is_mac) {
        /* Multiply / MAC family: 1 QQQ d k sss */
        unsigned qqq = (alu >> 4) & 7;
        bool dst_b = (alu >> 3) & 1;
        bool neg = (alu >> 2) & 1;
        bool mac = (alu >> 1) & 1;
        bool rnd = (alu & 1) != 0;
        int32_t s1 = 0, s2 = 0;

        switch (qqq) {
        case 0: s1 = sext24(d->x0); s2 = sext24(d->x0); break;
        case 1: s1 = sext24(d->y0); s2 = sext24(d->y0); break;
        case 2: s1 = sext24(d->x1); s2 = sext24(d->x0); break;
        case 3: s1 = sext24(d->y1); s2 = sext24(d->y0); break;
        case 4: s1 = sext24(d->x0); s2 = sext24(d->y1); break;
        case 5: s1 = sext24(d->y0); s2 = sext24(d->x0); break;
        case 6: s1 = sext24(d->x1); s2 = sext24(d->y0); break;
        case 7: s1 = sext24(d->y1); s2 = sext24(d->x1); break;
        }
        /* Fractional 24x24 -> 48 bit shifted left by 1 */
        int64_t prod = (int64_t)s1 * s2 * 2;
        if (neg) {
            prod = -prod;
        }
        int64_t acc = mac ? sext56(dst_b ? d->b : d->a) : 0;
        int64_t res = acc + prod;
        if (rnd) {
            /* Convergent rounding at bit 23 */
            uint64_t r = (uint64_t)res;
            if ((r & 0xFFFFFFULL) > 0x800000ULL ||
                ((r & 0xFFFFFFULL) == 0x800000ULL && (r & 0x1000000ULL))) {
                res += 0x1000000LL;
            }
            res &= ~0xFFFFFFLL;
        }
        uint64_t res56 = (uint64_t)res & DSP_MASK56;
        if (dst_b) {
            d->b = res56;
        } else {
            d->a = res56;
        }
        set_ccr56(d, res56, false, false);
        return;
    }

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

/* Execute parallel move (bits 23:8 of 24-bit opcode != 0 in bits 23:20) */
static void exec_parallel(Dsp56300 *d, uint32_t op, uint32_t ext,
                          bool *used_ext)
{
    uint32_t mv = (op >> 8) & 0xFFFFu;
    uint8_t alu = op & 0xFFu;

    *used_ext = false;

    /* 1. X: + Y: dual move: 1w0m mrrr 1w1M MRRR (bits 23:20 >= 8) */
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
        bool rx = (mv >> 7) & 1;
        bool ry = (mv >> 14) & 1;
        uint32_t xa = d->r[xn], ya = d->r[yn];
        uint32_t xv = rx ? mem_read_x(d, xa) : read_reg6(d, xr);
        uint32_t yv = ry ? mem_read_y(d, ya) : read_reg6(d, yr);

        if (xm) {
            d->r[xn] = agu_add(d, xn, xm == 1 ? sext24(d->n[xn]) :
                                      xm == 2 ? -1 : 1);
        }
        if (ym) {
            d->r[yn] = agu_add(d, yn, ym == 1 ? sext24(d->n[yn]) :
                                      ym == 2 ? -1 : 1);
        }
        exec_alu(d, alu);
        if (rx) {
            write_reg6(d, xr, xv);
        } else {
            mem_write_x(d, xa, xv);
        }
        if (ry) {
            write_reg6(d, yr, yv);
        } else {
            mem_write_y(d, ya, yv);
        }
        return;
    }

    /* 2. Non-dual parallel moves (mv < 0x8000): bits 23:20 = 1..7 */
    unsigned top4 = (op >> 20) & 0xF;

    if (top4 == 2 || top4 == 3) {
        /* DSP56300FM instruction encoding. */
        /* 0010 00dd ddd0 ... or 0010 10... */
        /* DSP56300FM instruction encoding. */
        if (mv >= 0x2020u && mv <= 0x203Fu) {
            /* IFcc / IFcc.U: 0010 0000 001u cccc */
            if (eval_cc(d, mv & 0xFu)) {
                uint32_t saved_sr = d->sr;
                exec_alu(d, alu);
                if (!(mv & 0x10u)) {
                    /* IFcc (not .U) preserves CCR */
                    d->sr = saved_sr;
                }
            }
            return;
        }
        if ((mv & 0xFC00u) <= 0x22FFu) {
            unsigned src = (mv >> 5) & 0x1Fu;
            unsigned dst = mv & 0x1Fu;
            uint32_t v = read_reg5(d, src);
            exec_alu(d, alu);
            write_reg5(d, dst, v);
            return;
        } else {
            /* DSP56300FM instruction encoding. */
            unsigned dst = (mv >> 8) & 0x1Fu;
            uint32_t imm = mv & 0xFFu;
            /* Short moves to ALU operands are signed fractions (FM 13-113). */
            if ((dst >= 4 && dst <= 7) || dst == 14 || dst == 15) {
                imm <<= 16;
            }
            exec_alu(d, alu);
            write_reg5(d, dst, imm);
            return;
        }
    }

    if (top4 == 1) {
        /* Class I X:R / R:Y: memory plus accumulator-to-input-register. */
        bool y = (op >> 14) & 1;
        bool read = (op >> 15) & 1;
        unsigned reg = y ? ((op >> 16) & 3) : ((op >> 18) & 3);
        static const uint8_t xr[] = { 4, 5, 14, 15 };
        static const uint8_t yr[] = { 6, 7, 14, 15 };
        unsigned memory_reg = y ? yr[reg] : xr[reg];
        unsigned acc_reg = 14 + ((op >> (y ? 19 : 17)) & 1);
        unsigned input_reg = (y ? 4 : 6) + ((op >> (y ? 18 : 16)) & 1);
        unsigned ea = (op >> 8) & 0x3F;
        uint32_t addr = eval_ea(d, ea, ext, used_ext, true);
        uint32_t value = read ? (ea == 0x34 ? ext :
                                   y ? mem_read_y(d, addr) : mem_read_x(d, addr)) :
                               read_reg6(d, memory_reg);
        uint32_t acc = read_reg6(d, acc_reg);

        exec_alu(d, alu);
        write_reg6(d, input_reg, acc);
        if (read) {
            write_reg6(d, memory_reg, value);
        } else if (y) {
            mem_write_y(d, addr, value);
        } else {
            mem_write_x(d, addr, value);
        }
        return;
    }

    /* Long moves use X:Y as the high:low halves of a 48-bit operand. */
    if ((op & 0xF40000u) == 0x400000u) {
        unsigned reg = ((op >> 17) & 4) | ((op >> 16) & 3);
        unsigned ea = (op >> 8) & 0x3F;
        uint32_t addr = (op & 0x4000) ? eval_ea(d, ea, ext, used_ext, true) : ea;
        bool read = op & 0x8000;
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
        exec_alu(d, alu);
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
        return;
    }

    if (top4 >= 4 && top4 <= 7) {
        unsigned reg5 = ((op >> 17) & 0x18u) | ((op >> 16) & 0x07u);
        bool space_y = (op >> 19) & 1;
        bool mem_to_reg = (op >> 15) & 1;
        bool ea_mode = (op >> 14) & 1;
        unsigned mmmrrr = (op >> 8) & 0x3Fu;

        if (ea_mode) {
            if (mmmrrr == 0x34 && mem_to_reg) {
                /* #>xxxxxx immediate */
                *used_ext = true;
                exec_alu(d, alu);
                write_reg5(d, reg5, ext);
                return;
            }
            uint32_t addr = eval_ea(d, mmmrrr, ext, used_ext, true);
            if (mem_to_reg) {
                uint32_t v = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
                exec_alu(d, alu);
                write_reg5(d, reg5, v);
            } else {
                uint32_t v = read_reg5(d, reg5);
                exec_alu(d, alu);
                if (space_y) {
                    mem_write_y(d, addr, v);
                } else {
                    mem_write_x(d, addr, v);
                }
            }
        } else {
            uint32_t addr = mmmrrr;
            if (mem_to_reg) {
                uint32_t v = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
                exec_alu(d, alu);
                write_reg5(d, reg5, v);
            } else {
                uint32_t v = read_reg5(d, reg5);
                exec_alu(d, alu);
                if (space_y) {
                    mem_write_y(d, addr, v);
                } else {
                    mem_write_x(d, addr, v);
                }
            }
        }
        return;
    }


}

static bool step_one(Dsp56300 *d)
{
    if (d->halted || d->wait_mode || d->stop_mode) {
        return false;
    }

    uint32_t pc = d->pc;
    uint32_t op = mem_read_p(d, pc);
    uint32_t ext = mem_read_p(d, pc + 1);
    uint32_t next_pc = pc + 1;
    bool used_ext = false;

    /* Check parallel move (bits 23:20 != 0) */
    if ((op >> 20) != 0) {
        if ((op & 0xFFC000u) == 0x204000u) {
            /* Update (Rn) parallel move: 0010 0000 010m mrrr */
            unsigned mmmrrr = (op >> 8) & 0x3Fu;
            eval_ea(d, mmmrrr, 0, NULL, true);
            exec_alu(d, op & 0xFFu);
        } else {
            exec_parallel(d, op, ext, &used_ext);
            if (used_ext) {
                next_pc = pc + 2;
            }
        }
        goto advance;
    }

    /* Non-parallel instructions (bits 23:20 == 0) */
    if (op == 0x000000u) {
        /* NOP */
    } else if (op == 0x000004u) {
        /* RTI */
        uint32_t h, l;
        stack_pop(d, &h, &l);
        d->sr = l;
        next_pc = h;
    } else if (op >= 0x000008u && op <= 0x00000Bu) {
        /* INC A/B (8/9), DEC A/B (A/B) */
        uint64_t *dst = (op & 1) ? &d->b : &d->a;
        int64_t v = sext56(*dst) + ((op & 2) ? -1 : 1);
        *dst = (uint64_t)v & DSP_MASK56;
        set_ccr56(d, *dst, false, false);
    } else if (op == 0x00000Cu) {
        /* RTS */
        uint32_t h;
        stack_pop(d, &h, NULL);
        next_pc = h;
    } else if (op == 0x000203u) {
        stack_push(d, d->la, d->lc);
        stack_push(d, pc + 2, d->sr);
        d->la = ext;
        d->sr |= SR_LF | SR_FV;
        next_pc = pc + 2;
    } else if (op == 0x00008Cu) {
        /* ENDDO */
        uint32_t h, l;
        uint32_t saved_sr;
        stack_pop(d, NULL, &saved_sr);
        stack_pop(d, &h, &l);
        d->la = h;
        d->lc = l;
        d->sr = (d->sr & ~(SR_LF | SR_FV)) | (saved_sr & (SR_LF | SR_FV));
    } else if (op == 0x000086u) {
        d->stop_mode = true;
    } else if (op == 0x000087u) {
        d->wait_mode = true;
    } else if ((op & 0xFF00FCu) == 0x0000B8u) {
        /* ANDI #xx, D */
        uint32_t imm = (op >> 8) & 0xFFu;
        unsigned dd = op & 3;
        if (dd == 0) {
            d->sr = (d->sr & 0x00FFu) | (((d->sr >> 8) & imm) << 8);
        } else if (dd == 1) {
            d->sr = (d->sr & 0xFF00u) | (d->sr & imm);
        } else if (dd == 2) {
            d->omr = (d->omr & ~0xFFu) | (d->omr & imm);
        }
    } else if ((op & 0xFF00FCu) == 0x0000F8u) {
        /* ORI #xx, D */
        uint32_t imm = (op >> 8) & 0xFFu;
        unsigned dd = op & 3;
        if (dd == 0) {
            d->sr |= (imm << 8);
        } else if (dd == 1) {
            d->sr |= imm;
        } else if (dd == 2) {
            d->omr |= imm;
        }
    } else if ((op & 0xFFFE00u) == 0x0C1C00u ||
               (op & 0xFFFFE0u) == 0x0C1E40u ||
               (op & 0xFFFFE0u) == 0x0C1E60u) {
        bool immediate = (op & 0xFFFE00u) == 0x0C1C00u;
        bool left = immediate ? (op & 0x100u) : !(op & 0x20u);
        unsigned n;
        uint64_t value = (op & (immediate ? 0x80u : 0x10u)) ? d->b : d->a;
        if (immediate) {
            n = (op >> 1) & 0x3F;
        } else {
            static const uint8_t regs[] = { 0, 0, 12, 13, 4, 6, 5, 7 };
            n = read_reg6(d, regs[(op >> 1) & 7]) & 0x3F;
        }
        bool carry = n ? (left ? (n <= 56 && ((value >> (56 - n)) & 1)) :
                                ((value >> (n - 1)) & 1)) : (d->sr & SR_C);
        uint64_t result = left ? (value << n) & DSP_MASK56 :
                                  (uint64_t)(sext56(value) >> n) & DSP_MASK56;
        bool overflow = left && (sext56(result) >> n) != sext56(value);
        if (op & 1) { d->b = result; } else { d->a = result; }
        set_ccr56(d, result, carry, overflow);
    } else if ((op & 0xFFFF80u) == 0x0C1E80u ||
               (op & 0xFFFFD0u) == 0x0C1E10u) {
        bool immediate = op & 0x80u;
        bool left = !(op & (immediate ? 0x40u : 0x20u));
        static const uint8_t regs[] = { 0, 0, 12, 13, 4, 6, 5, 7 };
        unsigned n = immediate ? ((op >> 1) & 31) :
                     (read_reg6(d, regs[(op >> 1) & 7]) & 31);
        uint64_t *dst = (op & 1) ? &d->b : &d->a;
        uint32_t value = (*dst >> 24) & DSP_MASK24;
        bool carry = n ? (left ? (n <= 24 && ((value >> (24 - n)) & 1)) :
                                ((value >> (n - 1)) & 1)) : (d->sr & SR_C);
        uint32_t result = left ? (value << n) & DSP_MASK24 : value >> n;
        *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)result << 24);
        set_ccr_logic(d, *dst);
        d->sr = (d->sr & ~SR_C) | (carry ? SR_C : 0);
    } else if ((op & 0xFFF000u) == 0x0C0000u) {
        /* JMP xxxx (12-bit) */
        next_pc = op & 0xFFFu;
    } else if ((op & 0xFFF000u) == 0x0D0000u) {
        /* JSR xxxx (12-bit) */
        stack_push(d, pc + 1, d->sr);
        next_pc = op & 0xFFFu;
    } else if ((op & 0xFF0000u) == 0x0E0000u) {
        /* Jcc xxxx (12-bit) */
        unsigned cc = (op >> 12) & 0xFu;
        if (eval_cc(d, cc)) {
            next_pc = op & 0xFFFu;
        }
    } else if ((op & 0xFF0000u) == 0x0F0000u) {
        /* JScc xxxx (12-bit) */
        unsigned cc = (op >> 12) & 0xFu;
        if (eval_cc(d, cc)) {
            stack_push(d, pc + 1, d->sr);
            next_pc = op & 0xFFFu;
        }
    } else if (op == 0x0AF080u) {
        /* JMP >xxxxxx */
        if (ext == DSP_BOOT_ROM) {
            d->boot_state = BOOT_COUNT;
            d->halted = true;
            d->pc = DSP_BOOT_ROM;
            return false;
        }
        next_pc = ext & DSP_MASK24;
    } else if (op == 0x0BF080u) {
        /* JSR >xxxxxx */
        stack_push(d, pc + 2, d->sr);
        next_pc = ext & DSP_MASK24;
    } else if ((op & 0xFFFFF0u) == 0x0AF0A0u) {
        /* Jcc >xxxxxx */
        if (eval_cc(d, op & 0xFu)) {
            next_pc = ext & DSP_MASK24;
        } else {
            next_pc = pc + 2;
        }
    } else if ((op & 0xFFFFF0u) == 0x0BF0A0u) {
        /* JScc >xxxxxx */
        if (eval_cc(d, op & 0xFu)) {
            stack_push(d, pc + 2, d->sr);
            next_pc = ext & DSP_MASK24;
        } else {
            next_pc = pc + 2;
        }
    } else if ((op & 0xFFFC20u) == 0x050C00u ||
               (op & 0xFFFC20u) == 0x050800u ||
               (op & 0xFF0C20u) == 0x050400u) {
        /* Signed nine-bit displacement: bits 9:6,4:0. */
        unsigned bits = ((op >> 1) & 0x1E0u) | (op & 0x1Fu);
        int32_t disp = bits & 0x100 ? (int32_t)bits - 0x200 : bits;
        bool bsr = (op & 0xC00u) == 0x800u;
        bool take = (op & 0xC00u) != 0x400u || eval_cc(d, (op >> 12) & 15);

        if (take) {
            if (bsr) {
                stack_push(d, pc + 1, d->sr);
            }
            next_pc = (pc + disp) & DSP_MASK24;
        }
    } else if (op == 0x0D10C0u || op == 0x0D1080u) {
        if (op == 0x0D1080u) {
            stack_push(d, pc + 2, d->sr);
        }
        next_pc = (pc + sext24(ext)) & DSP_MASK24;
    } else if ((op & 0xFFC000u) == 0x044000u ||
               (op & 0xFFC000u) == 0x04C000u) {
        unsigned reg = (op >> 8) & 0x3Fu;
        bool write = (op >> 15) & 1;

        if ((op & 0xA0u) == 0xA0u) {
            /* MOVEC: control register in bits 5:0, general register 13:8. */
            unsigned control = op & 0x3Fu;
            if (write) {
                write_reg6(d, control, read_reg6(d, reg));
            } else {
                write_reg6(d, reg, read_reg6(d, control));
            }
        } else {
            /* MOVEP low I/O: q[5] is encoded in bit 6, X/Y in bit 7. */
            bool space_x = (op >> 7) & 1;
            uint32_t addr = 0xFFFF80u + ((op >> 1) & 0x20u) + (op & 0x1Fu);
            if (write) {
                uint32_t v = read_reg6(d, reg);
                if (space_x) {
                    mem_write_x(d, addr, v);
                } else {
                    mem_write_y(d, addr, v);
                }
            } else {
                write_reg6(d, reg, space_x ? mem_read_x(d, addr) :
                                            mem_read_y(d, addr));
            }
        }
    } else if ((op & 0xFF0000u) == 0x050000u) {
        /* MOVEC */
        if ((op & 0x0000E0u) == 0x0000A0u) {
            /* movec #imm8, D (0x0500BB, 0x053FA0) */
            write_reg6(d, op & 0x3Fu, (op >> 8) & 0xFFu);
        } else {
            /* movec <ea>/<aa>, D or D, <ea>/<aa> */
            unsigned mid = (op >> 8) & 0xFFu;
            bool w = (mid >> 7) & 1;
            bool ea_mode = (mid >> 6) & 1;
            unsigned mmmrrr = mid & 0x3Fu;
            bool space_y = (op >> 6) & 1;
            unsigned reg6 = op & 0x3Fu;

            if (ea_mode && mmmrrr == 0x34 && w) {
                write_reg6(d, reg6, ext);
                next_pc = pc + 2;
            } else {
                uint32_t addr = ea_mode ? eval_ea(d, mmmrrr, ext, &used_ext, true)
                                        : mmmrrr;
                if (used_ext) {
                    next_pc = pc + 2;
                }
                if (w) {
                    uint32_t v = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
                    write_reg6(d, reg6, v);
                } else {
                    uint32_t v = read_reg6(d, reg6);
                    if (space_y) mem_write_y(d, addr, v); else mem_write_x(d, addr, v);
                }
            }
        }
    } else if ((op & 0xFF0000u) == 0x060000u) {
        /* REP (bit 5 = 1) or DO (bit 5 = 0) */
        bool is_rep = (op & 0x20u) != 0;
        uint32_t cnt = 0;

        if ((op & 0xD0u) == 0x80u) {
            /* #xxx immediate (12-bit: bits 3:0 << 8 | bits 15:8) */
            cnt = ((op & 0x0Fu) << 8) | ((op >> 8) & 0xFFu);
        } else if ((op & 0xC0DFu) == 0xC000u) {
            /* S register */
            cnt = read_reg6(d, (op >> 8) & 0x3Fu);
        } else {
            /* X:<ea> / Y:<ea> */
            uint32_t ea = eval_ea(d, (op >> 8) & 0x3Fu, 0, NULL, true);
            bool space_y = (op >> 6) & 1;
            cnt = space_y ? mem_read_y(d, ea) : mem_read_x(d, ea);
        }

        if (is_rep) {
            d->rep_saved_lc = d->lc;
            d->rep_remaining = cnt ? cnt : 65536;
            d->lc = d->rep_remaining;
            d->rep_pc = pc + 1;
        } else if (!cnt && !(d->sc & (1u << 13))) {
            next_pc = (ext + 1) & DSP_MASK24;
        } else {
            if (getenv("CDJ850_DSP_DEBUG")) {
                info_report("cdj850-dsp: DO at P:%06x op=0x%06x cnt=%u la=0x%06x",
                            pc, op, cnt, ext & DSP_MASK24);
            }
            stack_push(d, d->la, d->lc);
            stack_push(d, pc + 2, d->sr);
            d->la = ext & DSP_MASK24;
            d->lc = cnt ? cnt : 65536;
            d->sr = (d->sr & ~SR_FV) | SR_LF;
            next_pc = pc + 2;
        }
    } else if ((op & 0xFE0000u) == 0x080000u || (op & 0xFF0000u) == 0x070000u) {
        /* DSP56300FM instruction encoding. */
        if ((op & 0xFE00C0u) == 0x080000u) {
            uint32_t addr = 0xFFFFC0u + (op & 0x3Fu);
            unsigned reg = (op >> 8) & 0x3Fu;
            bool y = op & 0x10000u;
            if (op & 0x8000u) {
                uint32_t v = read_reg6(d, reg);
                if (y) { mem_write_y(d, addr, v); } else { mem_write_x(d, addr, v); }
            } else {
                write_reg6(d, reg, y ? mem_read_y(d, addr) : mem_read_x(d, addr));
            }
            goto advance;
        }
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
        if (mmmrrr == 0x34 && w) {
            /* #>ext -> pp */
            if (space_y) mem_write_y(d, paddr, ext); else mem_write_x(d, paddr, ext);
            next_pc = pc + 2;
        } else {
            uint32_t ea = eval_ea(d, mmmrrr, ext, &used_ext, true);
            if (used_ext) {
                next_pc = pc + 2;
            }
            /* Note: bit 6 of op in 0x07..0x09 indicates EA space (0=X, 1=Y) */
            bool ea_y = (op >> 6) & 1;
            if (w) {
                uint32_t v = ea_y ? mem_read_y(d, ea) : mem_read_x(d, ea);
                if (space_y) mem_write_y(d, paddr, v); else mem_write_x(d, paddr, v);
            } else {
                uint32_t v = space_y ? mem_read_y(d, paddr) : mem_read_x(d, paddr);
                if (ea_y) mem_write_y(d, ea, v); else mem_write_x(d, ea, v);
            }
        }
    } else if ((op & 0xFE0080u) == 0x020080u) {
        /* Seven-bit signed displacement split across 16:11 and 6. */
        unsigned rn = (op >> 8) & 7;
        unsigned bits = ((op >> 10) & 0x7Eu) | ((op >> 6) & 1);
        int32_t disp = bits & 0x40 ? (int32_t)bits - 0x80 : bits;
        uint32_t addr = (d->r[rn] + disp) & DSP_MASK24;
        unsigned reg = op & 0xFu;
        bool y = op & 0x20u;

        if (op & 0x10u) {
            write_reg6(d, reg, y ? mem_read_y(d, addr) : mem_read_x(d, addr));
        } else if (y) {
            mem_write_y(d, addr, read_reg6(d, reg));
        } else {
            mem_write_x(d, addr, read_reg6(d, reg));
        }
    } else if ((op & 0xFFC0FFu) == 0x0AC080u) {
        /* JMP <ea> */
        uint32_t target = eval_ea(d, (op >> 8) & 0x3Fu, ext, &used_ext, false);
        if (target == DSP_BOOT_ROM) {
            d->boot_state = BOOT_COUNT;
            d->halted = true;
            d->pc = DSP_BOOT_ROM;
            return false;
        }
        next_pc = target;
    } else if ((op & 0xFFC0F0u) == 0x0AC0A0u) {
        /* Jcc <ea> */
        uint32_t target = eval_ea(d, (op >> 8) & 0x3Fu, ext, &used_ext, false);
        if (eval_cc(d, op & 0xFu)) {
            next_pc = target;
        } else if (used_ext) {
            next_pc = pc + 2;
        }
    } else if ((op & 0xFFC0FFu) == 0x0BC080u) {
        /* JSR <ea> */
        uint32_t target = eval_ea(d, (op >> 8) & 0x3Fu, ext, &used_ext, false);
        stack_push(d, used_ext ? pc + 2 : pc + 1, d->sr);
        next_pc = target;
    } else if ((op & 0xFFC0F0u) == 0x0BC0A0u) {
        /* JScc <ea> */
        uint32_t target = eval_ea(d, (op >> 8) & 0x3Fu, ext, &used_ext, false);
        if (eval_cc(d, op & 0xFu)) {
            stack_push(d, used_ext ? pc + 2 : pc + 1, d->sr);
            next_pc = target;
        } else if (used_ext) {
            next_pc = pc + 2;
        }
    } else if ((op & 0xFEF880u) == 0x0A7080u) {
        /* MOVE X/Y:(Rn+xxxx) long displacement (FM: 0000 101S 0111 0RRR
         * 1WDD DDDD, displacement in the extension word). It shares the
         * 0x0A/0x0B page with the <ea> bit ops; MMM=110 with bit 7 set is
         * not a valid JCLR/JSET <ea>, so this pattern is unambiguous.
         * Rn is not updated. W=1 loads D, W=0 stores it. */
        unsigned rn = (op >> 8) & 7;
        uint32_t addr = (d->r[rn] + ext) & DSP_MASK24;
        bool y = op & 0x10000u;
        unsigned reg = op & 0x3Fu;

        if (op & 0x40u) {
            write_reg6(d, reg, y ? mem_read_y(d, addr) : mem_read_x(d, addr));
        } else if (y) {
            mem_write_y(d, addr, read_reg6(d, reg));
        } else {
            mem_write_x(d, addr, read_reg6(d, reg));
        }
        next_pc = pc + 2;
    } else if ((op & 0xFF0000u) == 0x010000u ||
               (op & 0xFF0000u) == 0x0A0000u ||
               (op & 0xFF0000u) == 0x0B0000u) {
        /* Bit operations & bit-test branches + ADD/SUB/CMP #xx, D:
         *   0x01xxxx: bit ops on pp (0xFFFFC0..FF when bit7=1 or 0xFFFF80..BF)
         *     - 0x011100: bclr #0, x:<<0xffff91
         *     - 0x011120: bset #0, x:<<0xffff91
         *     - 0x011D64: bset #4, y:<<0xffff9d
         *     - 0x011D44: bclr #4, y:<<0xffff9d
         *     - 0x013716: bclr #0x16, x:<<0xffffb7
         *     - 0x019191: jclr #0x11, x:<<0xffff91, ext
         *     - 0x01B796: jclr #0x16, x:<<0xffffb7, ext
         *   0x0Axxxx / 0x0Bxxxx:
         *     - 0x0A0C30: bset #0x10, x:<0x0c
         *     - 0x0A20D0: jclr #0x10, y:<0x20, ext
         *     - 0x0AC457: bclr #0x17, x0
         *     - 0x0AC417: jclr #0x17, x0, ext
         *     - 0x0AB5EA: jset #0xa, y:<<0xfffff5, ext
         *     - 0x0BB56A: btst #0xa, y:<<0xfffff5
         *     - 0x014188: add #1, b (0x01 0x41 0x88 -> bit 7=1)
         */
        if ((op & 0xFF0080u) == 0x010080u && (op & 0x008000u) == 0 && (op & 0x004000u) != 0) {
            /* Immediate ADD/SUB/CMP/OR/EOR/AND #xx / #>ext, D */
            bool long_imm = (op & 0x0000C0u) == 0x0000C0u;
            uint32_t imm = long_imm ? ext : ((op >> 8) & 0x3Fu);
            bool dst_b = (op >> 3) & 1;
            uint64_t *dst = dst_b ? &d->b : &d->a;
            uint64_t imm56 = pack_acc24(imm);
            unsigned kind = op & 7;
            if (long_imm) {
                next_pc = pc + 2;
            }
            if (kind == 0) { /* ADD */
                *dst = (uint64_t)(sext56(*dst) + sext56(imm56)) & DSP_MASK56;
                set_ccr56(d, *dst, false, false);
            } else if (kind == 2) { /* OR */
                uint32_t a1 = ((uint32_t)(*dst >> 24) | imm) & DSP_MASK24;
                *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
                set_ccr_logic(d, *dst);
            } else if (kind == 3) { /* EOR */
                uint32_t a1 = ((uint32_t)(*dst >> 24) ^ imm) & DSP_MASK24;
                *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
                set_ccr_logic(d, *dst);
            } else if (kind == 4) { /* SUB */
                *dst = (uint64_t)(sext56(*dst) - sext56(imm56)) & DSP_MASK56;
                set_ccr56(d, *dst, false, false);
            } else if (kind == 5) { /* CMP */
                set_ccr56(d, (uint64_t)(sext56(*dst) - sext56(imm56)), *dst < imm56, false);
            } else if (kind == 6) { /* AND */
                uint32_t a1 = ((uint32_t)(*dst >> 24) & imm) & DSP_MASK24;
                *dst = (*dst & ~0xFFFFFF000000ULL) | ((uint64_t)a1 << 24);
                set_ccr_logic(d, *dst);
            }
        } else {
            unsigned bit = op & 0x1Fu;
            unsigned mask = 1u << bit;
            bool is_reg_op = ((op >> 16) == 0x0Au || (op >> 16) == 0x0Bu) &&
                             (((op >> 8) & 0xFFu) >= 0xC0u);
            bool is_branch = ((op >> 16) == 0x01u) ? ((op >> 15) & 1) :
                             (is_reg_op ? !((op >> 6) & 1) : ((op >> 7) & 1));
            bool is_jsr = ((op >> 16) == 0x0Bu) && is_branch;
            bool bit_set_cond = (op >> 5) & 1;
            bool space_y = (op >> 6) & 1;
            uint32_t val = 0;
            uint32_t addr = 0;
            bool is_reg = false;
            unsigned reg6 = 0;

            if ((op >> 16) == 0x01u) {
                addr = 0xFFFF80u + ((op >> 8) & 0x3Fu);
                val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
            } else {
                /* 0x0A or 0x0B */
                unsigned mid = (op >> 8) & 0xFFu;
                if (mid >= 0xC0u) {
                    is_reg = true;
                    reg6 = mid & 0x3Fu;
                    val = read_reg6(d, reg6);
                } else if (mid >= 0x80u) {
                    addr = 0xFFFFC0u + (mid & 0x3Fu);
                    val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
                } else if (mid >= 0x40u) {
                    /* <ea>: 01MM MRRR. The jump forms already use the
                     * extension word as their target, so only the plain
                     * bit ops may take an absolute address (MMMRRR 110000). */
                    bool ea_ext = false;
                    addr = eval_ea(d, mid & 0x3Fu, ext,
                                   is_branch ? NULL : &ea_ext, true);
                    if (ea_ext) {
                        next_pc = pc + 2;
                    }
                    val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
                } else {
                    addr = mid & 0x3Fu;
                    val = space_y ? mem_read_y(d, addr) : mem_read_x(d, addr);
                }
            }

            bool was_set = (val & mask) != 0;
            if (!is_branch) {
                if (was_set) { d->sr |= SR_C; } else { d->sr &= ~SR_C; }
            }

            if (is_branch) {
                bool cond = bit_set_cond ? was_set : !was_set;
                if (cond) {
                    if (is_jsr) {
                        stack_push(d, pc + 2, d->sr);
                    }
                    next_pc = ext & DSP_MASK24;
                } else {
                    next_pc = pc + 2;
                }
            } else {
                /* bclr/bset/bchg/btst */
                bool is_btst = false;
                bool is_bchg = false;
                if ((op >> 16) == 0x01u) {
                    is_btst = (op & 0x4000u) && (op & 0x20u);
                    is_bchg = (op & 0x4000u) && !(op & 0x20u);
                } else if ((op >> 16) == 0x0Bu) {
                    if (op & 0x20u) {
                        is_btst = true;
                    } else {
                        is_bchg = true;
                    }
                }
                if (!is_btst) {
                    uint32_t nval = is_bchg ? (val ^ mask) :
                                    (bit_set_cond ? (val | mask) : (val & ~mask));
                    if (is_reg) {
                        write_reg6(d, reg6, nval);
                    } else if (space_y) {
                        mem_write_y(d, addr, nval);
                    } else {
                        mem_write_x(d, addr, nval);
                    }
                }
            }
        }
    } else if ((op & 0xFE0000u) == 0x020000u) {
        /* Tcc, including the optional address-register transfer (FM 13-177):
         *   0000 0010 CCCC 0000 0JJJ D000   Tcc S1,D1
         *   0000 0011 CCCC 0ttt 0JJJ DTTT   Tcc S1,D1 S2,D2
         *   0000 0010 CCCC 1ttt 0000 0TTT   Tcc S2,D2
         * CCCC occupies bits 15:12, so the address-only flag is bit 11. */
        unsigned cc = (op >> 12) & 0xFu;
        bool addr_only = !(op & 0x10000u) && (op & 0x800u);
        if (eval_cc(d, cc)) {
            if (!addr_only) {
                unsigned src = (op >> 4) & 7;
                bool dst_b = op & 8;
                uint64_t value = dst_b ? d->a : d->b;
                switch (src) {
                case 4: value = pack_acc24(d->x0); break;
                case 5: value = pack_acc24(d->y0); break;
                case 6: value = pack_acc24(d->x1); break;
                case 7: value = pack_acc24(d->y1); break;
                }
                if (dst_b) { d->b = value; } else { d->a = value; }
            }
            if ((op & 0x10000u) || addr_only) {
                d->r[op & 7] = d->r[(op >> 8) & 7];
            }
        }
    } else {
        if (d->unimp_logged < 20) {
            d->unimp_logged++;
            info_report("cdj850-dsp: unimplemented op 0x%06x at P:%06x",
                        op, pc);
        }
    }

advance:
    if (d->rep_remaining && pc == d->rep_pc) {
        if (--d->rep_remaining) {
            d->lc = d->rep_remaining;
            next_pc = d->rep_pc;
        } else {
            d->lc = d->rep_saved_lc;
        }
    } else if ((d->sr & SR_LF) && next_pc == d->la + 1 &&
               (pc == d->la || pc + 1 == d->la)) {
        if (d->lc != 1 || (d->sr & SR_FV)) {
            d->lc = (d->lc - 1) & DSP_MASK24;
            next_pc = d->ssh[d->sp & 0xF];
        } else {
            uint32_t h, l, saved_sr;
            stack_pop(d, NULL, &saved_sr);
            stack_pop(d, &h, &l);
            d->la = h;
            d->lc = l;
            d->sr = (d->sr & ~(SR_LF | SR_FV)) | (saved_sr & (SR_LF | SR_FV));
        }
    }
    d->pc = next_pc & DSP_MASK24;
    if (d->pc >= 0x1000u && d->pc != DSP_BOOT_ROM && !d->unimp_logged) {
        d->unimp_logged = 1;
        info_report("cdj850-dsp: WILD PC P:%06x -> P:%06x (op=0x%06x, sp=0x%x, "
                    "sr=0x%06x, la=0x%06x, lc=0x%x)",
                    pc, d->pc, op, d->sp, d->sr, d->la, d->lc);
    }
    return true;
}

static void dsp_run_cycles(Dsp56300 *d, unsigned budget)
{
    d->yield_requested = false;
    while (budget-- > 0 && !d->halted && !d->wait_mode && !d->stop_mode) {
        if (!step_one(d) || d->yield_requested) {
            break;
        }
    }
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
        unsigned budget = 256;
        stack_push(d, interrupted_pc, d->sr);
        d->sr = (d->sr & ~(SR_LF | SR_S0 | SR_S1 | (1u << 17) | 0x300u)) |
                0x300u;
        d->pc = op == 0x0BF080u ? mem_read_p(d, vector_pc + 1) : op & 0xFFFu;
        do {
            if (!step_one(d)) {
                break;
            }
        } while (d->sp != old_sp && --budget);
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

static void dsp_audio_tick(void *opaque)
{
    Dsp56300 *d = opaque;
    unsigned frames = 44;
    unsigned i;

    d->audio_tick++;
    if ((d->audio_tick % 10) == 0) {
        frames = 45;
    }

    if (d->boot_state == BOOT_RUN && !d->halted) {
        for (i = 0; i < frames * 2; i++) {
            uint32_t rcr = d->x_io[XIO_ESAI_RCR];
            uint32_t tcr = d->x_io[XIO_ESAI_TCR];

            d->tfs_slot = !d->tfs_slot;
            dsp_run_cycles(d, d->slot_budget);

            /* ESAI RX0 / RX1 from SSI0 / SSI1. With RIE set the word waits
             * in the FIFO until the interrupt can be taken: a dropped RX
             * interrupt loses a word, and the DSP's block receiver counts
             * exactly 84 words per block (LFSR at P:$3CD..$3DF) before it
             * clears RIE and can request the next block. */
            if ((rcr & 1) && (d->ssi_rx[0].count > 0 || d->ssi_rx[1].count > 0) &&
                (!(rcr & (1u << 22)) || dsp_esai_irq_ready(d))) {
                uint32_t w0 = 0, w1 = 0;

                fifo_pop(&d->ssi_rx[0], &w0);
                fifo_pop(&d->ssi_rx[1], &w1);
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
        /* Main instructions are interleaved with the serial slots above. */
        if (d->audio_tick == 3000 && getenv("CDJ850_DSP_DEBUG")) {
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
        if ((d->audio_tick % 500) == 0 && getenv("CDJ850_DSP_DEBUG")) {
            info_report("cdj850-dsp: tick=%u P:%06x TCR=0x%06x RCR=0x%06x "
                        "Y0=0x%06x Y20=0x%06x Y22=0x%06x X0C=0x%06x X0D=0x%06x "
                        "X1E=0x%06x X1F=0x%06x X38=0x%06x PF10=%u",
                        d->audio_tick, d->pc, d->x_io[XIO_ESAI_TCR],
                        d->x_io[XIO_ESAI_RCR], d->y[0], d->y[0x20], d->y[0x22],
                        d->x[0x0c], d->x[0x0d], d->x[0x1e], d->x[0x1f],
                        d->x[0x38], (io_read_y(d, 0xFFFFF5) >> 10) & 1);
            info_report("cdj850-dsp: ESAI RX=%" PRIu64 " TX0=%" PRIu64
                        " nonzero=%" PRIu64 " peak=0x%06x",
                        d->esai_rx_words, d->esai_tx_words, d->esai_tx_nonzero,
                        d->esai_tx_peak);
            d->esai_tx_peak = 0;
        }
    }

    timer_mod(d->audio_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

void dsp56300_shi_tx(uint8_t byte)
{
    Dsp56300 *d = dsp;
    uint32_t w;

    if (!d) {
        return;
    }
    /* Finish instructions after a bootstrap GPIO edge before the next word. */
    dsp_run_cycles(d, 64);
    d->shi_acc = (d->shi_acc << 8) | byte;
    if (++d->shi_nbytes < 3) {
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
        return;
    }

    if (getenv("CDJ850_DSP_DEBUG")) {
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
}

uint8_t dsp56300_shi_rx(void)
{
    Dsp56300 *d = dsp;

    if (!d) {
        return 0;
    }
    if (d->shi_tx_bytes_left == 0) {
        if (!fifo_pop(&d->shi_tx, &d->shi_tx_word)) {
            return 0;
        }
        d->shi_tx_bytes_left = 3;
    }
    d->shi_tx_bytes_left--;
    return (uint8_t)((d->shi_tx_word >> (d->shi_tx_bytes_left * 8)) & 0xFFu);
}

void dsp56300_shi_cs(bool active)
{
    Dsp56300 *d = dsp;

    if (!d) {
        return;
    }
    if (!active) {
        d->shi_nbytes = 0;
        d->shi_acc = 0;
    }
}

void dsp56300_esai_rx_push(unsigned ch, uint32_t word24)
{
    Dsp56300 *d = dsp;

    if (!d || ch > 1) {
        return;
    }
    d->esai_rx_words++;
    fifo_push(&d->ssi_rx[ch], word24);
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

    {
        const char *e = getenv("CDJ850_DSP_IPS");

        d->slot_budget = e && atoi(e) > 0 ? (unsigned)atoi(e) : 64;
    }
    d->audio_sink = cdj_host_audio_open(&audio_cfg);
    d->audio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_audio_tick, d);
    timer_mod(d->audio_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    dsp = d;
}
