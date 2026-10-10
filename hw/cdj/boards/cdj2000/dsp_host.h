/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the CDJ-2000's and the CDJ-2000NXS's DSPs have in common as MAIN sees
 * them: a TI C67x-family chip booted through its host port (UHPI), with the
 * same handshake on both boards --
 *   MAIN drives a 2-bit command on two DSP pins from its port latch;
 *   the DSP's loader answers each command by raising HINT in its HPIC;
 *   the HINT pin (active low) comes back to MAIN as the latch's busy input,
 *   and MAIN clears HINT through its own view of the HPIC.
 * The chip files (dsp_c6747.c, dsp_c6727.c) supply the memory map, the host
 * window and which pins carry the command; this runs the core, on its own
 * thread. What the core and MAIN's host port share is under @lock, which the
 * chip takes in its host-port handlers (CDJ_DSP_HOST_GUARD).
 */
#ifndef CDJ2000_DSP_HOST_H
#define CDJ2000_DSP_HOST_H
#include "sh7763.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "c6x/c66x.h"

#define HPIC_HWOB       (1u << 0)
#define HPIC_DSPINT     (1u << 1)
#define HPIC_HINT       (1u << 2)
#define HPIC_HRDY       (1u << 3)

#define DSP_HOST_MAX_ADDR 128
#define DSP_HOST_MAX_POSTED 256

/* Both images send control words to a converter on SPI1 and nothing comes
 * back: after each write to SPIDAT1 they wait for SPIBUF to show the
 * transmitter free and the receive buffer empty (C6747 image B 0xC004F3C4). */
#define SPIBUF_RXEMPTY  (1u << 31)

typedef struct DspAddrCount {
    uint32_t addr;
    uint64_t count;
    uint32_t last;
} DspAddrCount;

typedef struct DspPostedWrite {
    uint32_t addr, val;
} DspPostedWrite;

typedef struct DspAddrTable {
    DspAddrCount e[DSP_HOST_MAX_ADDR];
    unsigned n;
} DspAddrTable;

/* The three McASPs' global control: both chips bring each one up by
 * setting reset bits through RGBLCTL/XGBLCTL and polling GBLCTL until they
 * read back (C6747 image B 0xC004E700, C6727 image B on 0x45000044). */
typedef struct DspMcasps {
    uint32_t base, stride;
    uint32_t gblctl[3];
} DspMcasps;

typedef struct CdjDspHost {
    const char *name;
    c66x_core *core;
    bool enabled, running;
    c66x_stop last_stop;
    uint32_t trap_pc;
    uint64_t cycles_per_chunk;
    uint64_t slice_cycles;      /* the longest a chunk runs under @lock */
    QemuMutex lock;
    unsigned host_waiting;      /* MAIN waits for @lock */
    bool host_had_bql;          /* MAIN's holder of @lock let the BQL go */
    uint64_t host_locks;
    int64_t host_wait_ns;
    QemuThread thread;
    bool paused;                /* the machine is stopped: run nothing */
    GByteArray *snap;           /* the core's image in a snapshot */
    int64_t dsp_ns;             /* the virtual time the core has run up to */
    int64_t lag_max_ns;
    int64_t slack_ns;           /* 0: the core may lag without bound */
    QEMUTimer *slack_timer;
    QemuMutex progress_lock;    /* @dsp_ns, for the slack timer's wait */
    QemuCond progress;

    /* MAIN's data writes to the core's RAM, held until the core thread's
     * next slice; see cdj_dsp_host_write_word. NULL @ram: not used. */
    uint8_t *(*ram)(void *chip, uint32_t addr);
    QemuMutex posted_lock;
    DspPostedWrite posted[DSP_HOST_MAX_POSTED];
    unsigned nposted;

    uint32_t hpic;
    int dspint_line;            /* CPU interrupt MAIN's DSPINT raises, 0 = none */
    unsigned command;
    uint64_t commands, hint_edges, react_cycles;
    uint64_t idle_skipped;      /* cycles an idle core did not run */
    uint64_t pin_reads;         /* DSP reads of the command pins */
    /* The chip drives its command pins from MAIN's 2-bit command. */
    void (*set_pins)(void *chip, unsigned bits);
    /* Called with the core's lock held after each chunk it has run, with
     * the DSP's own clock; NULL for chips that need nothing paced. */
    void (*after_chunk)(void *chip, int64_t dsp_ns);
    void *chip;
    CdjDspWires wires;
    Notifier profile_exit;      /* C66X_JIT_PROFILE: free the core at exit */

    DspAddrTable busr, busw;
} CdjDspHost;

/* CDJ_<PREFIX>=0 turns the core off; CDJ_<PREFIX>_MHZ sets its clock. */
void cdj_dsp_host_init(CdjDspHost *h, const char *name, const char *env_prefix,
                       unsigned default_mhz,
                       void (*set_pins)(void *chip, unsigned bits), void *chip);
/* A core on @bus with nothing mapped yet; the chip maps its RAM, then
 * cdj_dsp_host_run() starts it at @entry. */
c66x_core *cdj_dsp_host_core(CdjDspHost *h, const c66x_bus *bus);
void cdj_dsp_host_run(CdjDspHost *h, uint32_t entry);

/* Snapshots. The chip embeds vmstate_cdj_dsp_host in its own state; once its
 * state is loaded and h->snap is not empty, it makes the core again (with
 * cdj_dsp_host_core, mapping its RAM and hooks as at boot) and calls
 * cdj_dsp_host_resume() instead of cdj_dsp_host_run(). */
extern const VMStateDescription vmstate_cdj_dsp_host;
void cdj_dsp_host_resume(CdjDspHost *h);

/* MAIN's side of @lock, for the chip's host-port handlers. The core's
 * thread steps aside while MAIN waits: a mutex alone lets it take the lock
 * straight back, chunk after chunk, and MAIN starves. */
void cdj_dsp_host_lock(CdjDspHost *h);
void cdj_dsp_host_unlock(CdjDspHost *h);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CdjDspHost, cdj_dsp_host_unlock)
#define CDJ_DSP_HOST_GUARD(h) \
    g_autoptr(CdjDspHost) host_guard_ = (cdj_dsp_host_lock(h), (h))

/* MAIN's data access to the core's RAM without stopping the core: the chip
 * sets @ram to the host pointer behind a DSP address (NULL when there is no
 * RAM there). A write is posted and the core thread stores it, and tells the
 * core, between two slices, so the core and its recording see it at one
 * point of the instruction stream; a read sees posted writes already. A
 * write outside RAM returns false. */
uint32_t cdj_dsp_host_read_word(CdjDspHost *h, uint32_t addr);
bool cdj_dsp_host_write_word(CdjDspHost *h, uint32_t addr, uint32_t val);

uint32_t cdj_dsp_host_hpic(const CdjDspHost *h);
/* The HPIC as MAIN writes it (clears HINT, raises DSPINT) and as the DSP
 * writes its own (clears DSPINT, raises HINT). Returns true when MAIN raised
 * DSPINT. */
bool cdj_dsp_host_main_hpic(CdjDspHost *h, uint32_t val);
void cdj_dsp_host_dsp_hpic(CdjDspHost *h, uint32_t val);

/* DSP memory as a QEMU RAM block, which a snapshot carries. */
uint8_t *cdj_dsp_ram(MemoryRegion *mr, const char *name, uint64_t size);

/* GBLCTL, RGBLCTL and XGBLCTL of McASP n; 0 for any other address. */
uint32_t cdj_dsp_mcasp_read(const DspMcasps *m, uint32_t addr);
void cdj_dsp_mcasp_write(DspMcasps *m, uint32_t addr, uint32_t val);

/* Save @len bytes of DSP memory as <dir>/<name>. */
void cdj_dsp_dump_mem(const char *dir, const char *name, const void *p,
                      size_t len);
void cdj_dsp_count(DspAddrTable *t, uint32_t addr, uint32_t val);
void cdj_dsp_host_report(CdjDspHost *h);

#endif
