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
 * window and which pins carry the command; this runs the core.
 */
#ifndef CDJ2000_DSP_HOST_H
#define CDJ2000_DSP_HOST_H
#include "sh7763.h"
#include "qemu/timer.h"
#include "c6x/c66x.h"

#define HPIC_HWOB       (1u << 0)
#define HPIC_DSPINT     (1u << 1)
#define HPIC_HINT       (1u << 2)
#define HPIC_HRDY       (1u << 3)

#define DSP_HOST_MAX_ADDR 128

/* Both images send control words to a converter on SPI1 and nothing comes
 * back: after each write to SPIDAT1 they wait for SPIBUF to show the
 * transmitter free and the receive buffer empty (C6747 image B 0xC004F3C4). */
#define SPIBUF_RXEMPTY  (1u << 31)

typedef struct DspAddrCount {
    uint32_t addr;
    uint64_t count;
    uint32_t last;
} DspAddrCount;

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
    uint64_t cycles_per_slice;
    QEMUTimer *slice;

    uint32_t hpic;
    unsigned command;
    uint64_t commands, hint_edges, react_cycles;
    uint64_t pin_reads;         /* DSP reads of the command pins */
    /* The chip drives its command pins from MAIN's 2-bit command. */
    void (*set_pins)(void *chip, unsigned bits);
    void *chip;
    CdjDspWires wires;

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

uint32_t cdj_dsp_host_hpic(const CdjDspHost *h);
/* The HPIC as MAIN writes it (clears HINT, raises DSPINT) and as the DSP
 * writes its own (clears DSPINT, raises HINT). Returns true when MAIN raised
 * DSPINT. */
bool cdj_dsp_host_main_hpic(CdjDspHost *h, uint32_t val);
void cdj_dsp_host_dsp_hpic(CdjDspHost *h, uint32_t val);

/* GBLCTL, RGBLCTL and XGBLCTL of McASP n; 0 for any other address. */
uint32_t cdj_dsp_mcasp_read(const DspMcasps *m, uint32_t addr);
void cdj_dsp_mcasp_write(DspMcasps *m, uint32_t addr, uint32_t val);

/* Save @len bytes of DSP memory as <dir>/<name>. */
void cdj_dsp_dump_mem(const char *dir, const char *name, const void *p,
                      size_t len);
void cdj_dsp_count(DspAddrTable *t, uint32_t addr, uint32_t val);
void cdj_dsp_host_report(CdjDspHost *h);

#endif
