/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x CorePac interrupt controller (SPRUGW0 ch. 9) and chip interrupt
 * controller CIC0 (SPRS814 table 7-37).
 *
 * As the program configures them (app 0x8007E260, 0x8007E4F4): INTMUX sends
 * core events 25/68/24/64/22/27 to INT4..INT9, and CIC0 channels 0/2/3/4/5
 * carry TINT4L, EDMA3_CC_INT1, UPPINT, TINT5L and EDMA3_CC_INT2.
 */
#include "soc_internal.h"

/* ---- INTC ------------------------------------------------------------------ */
static void cpu_pulse(c6655_soc *s, int line)
{
    if (!s->cfg.core)
        return;
    c66x_set_irq(s->cfg.core, line, 1);
    c66x_set_irq(s->cfg.core, line, 0);
}

/* Every INT line whose INTSEL names this event sees it. */
static void intc_route(c6655_soc *s, unsigned evt)
{
    for (int line = 4; line <= 15; line++) {
        unsigned idx = line - 4;
        unsigned sel = (s->intc.intmux[idx / 4] >> (8 * (idx % 4))) & 0x7F;
        if (sel == evt)
            cpu_pulse(s, line);
    }
}

static void intc_combiners(c6655_soc *s)
{
    soc_intc *ic = &s->intc;
    for (unsigned n = 0; n < 4; n++) {
        uint32_t pend = ic->flag[n] & ~ic->mask[n];
        int level = pend != 0;
        int was = (ic->combiner_level >> n) & 1;
        if (level == was)
            continue;
        ic->combiner_level ^= 1u << n;
        if (level)
            intc_route(s, n);
    }
}

void intc_reset(soc_intc *ic)
{
    *ic = (soc_intc){ 0 };
    for (int i = 0; i < 4; i++)
        ic->mask[i] = ic->expmask[i] = 0xFFFFFFFF;   /* reset value unverified */
}

void intc_event(c6655_soc *s, unsigned evt, int rising)
{
    if (!rising || evt < 4 || evt > 127)
        return;
    s->intc.flag[evt / 32] |= 1u << (evt % 32);
    intc_route(s, evt);
    intc_combiners(s);
}

uint32_t intc_read(c6655_soc *s, uint32_t off, unsigned size)
{
    soc_intc *ic = &s->intc;
    uint32_t a = off & ~3u, v;
    if (a < 0x10)
        v = ic->flag[a / 4];
    else if (a >= 0x80 && a < 0x90)
        v = ic->mask[(a - 0x80) / 4];
    else if (a >= 0xA0 && a < 0xB0)
        v = ic->flag[(a - 0xA0) / 4] & ~ic->mask[(a - 0xA0) / 4];
    else if (a >= 0xC0 && a < 0xD0)
        v = ic->expmask[(a - 0xC0) / 4];
    else if (a >= 0xE0 && a < 0xF0)
        v = ic->flag[(a - 0xE0) / 4] & ~ic->expmask[(a - 0xE0) / 4];
    else if (a >= 0x104 && a <= 0x10C)
        v = ic->intmux[(a - 0x104) / 4];
    else if (a == 0x180)
        v = ic->intxstat;
    else if (a == 0x188)
        v = ic->intdmask;
    else if ((a >= 0x20 && a < 0x30) || (a >= 0x40 && a < 0x50) || a == 0x184)
        v = 0;                                      /* write-only */
    else {
        soc_log_unimp(s, "INTC", 0x01800000 + off, 0, 0);
        v = 0;
    }
    return reg_get(v, off, size);
}

void intc_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size)
{
    soc_intc *ic = &s->intc;
    uint32_t a = off & ~3u;
    uint32_t v = reg_put(0, off, val, size);
    if (a >= 0x20 && a < 0x30) {
        unsigned base = (a - 0x20) / 4 * 32;
        for (unsigned b = 0; b < 32; b++)
            if ((v >> b) & 1)
                intc_event(s, base + b, 1);
    } else if (a >= 0x40 && a < 0x50) {
        ic->flag[(a - 0x40) / 4] &= ~v;
        intc_combiners(s);
    } else if (a >= 0x80 && a < 0x90) {
        uint32_t *m = &ic->mask[(a - 0x80) / 4];
        *m = reg_put(*m, off, val, size);
        intc_combiners(s);
    } else if (a >= 0xC0 && a < 0xD0) {
        uint32_t *m = &ic->expmask[(a - 0xC0) / 4];
        *m = reg_put(*m, off, val, size);
    } else if (a >= 0x104 && a <= 0x10C) {
        uint32_t *m = &ic->intmux[(a - 0x104) / 4];
        *m = reg_put(*m, off, val, size);
    } else if (a == 0x184) {
        if (v & 1)
            ic->intxstat = 0;
    } else if (a == 0x188) {
        ic->intdmask = reg_put(ic->intdmask, off, val, size);
    } else {
        soc_log_unimp(s, "INTC", 0x01800000 + off, 1, val);
    }
}

/* ---- CIC0 ------------------------------------------------------------------ */

/* CIC0 host output -> CorePac 0 event (SPRS814 table 7-33, n = 0). */
static int cic_host_event(unsigned host)
{
    if (host <= 9)
        return 22 + host;
    if (host == 10 || host == 11)
        return 92 + (host - 10);
    if (host >= 40 && host <= 47)
        return 56 + (host - 40);
    return -1;
}

/* The program never writes HINT_MAP and enables host interrupts by channel
 * number (0x8007E4A0), so channel n drives host output n. */
static void cic_update(c6655_soc *s)
{
    soc_cic *c = &s->cic0;
    uint8_t want[CIC_NHOST] = { 0 };
    if (c->global_enable & 1)
        for (unsigned i = 0; i < CIC_NSYS; i++) {
            if (!((c->raw[i / 32] & c->enable[i / 32]) >> (i % 32) & 1))
                continue;
            unsigned h = c->chmap[i];
            if (h < CIC_NHOST && ((c->hint_enable[h / 32] >> (h % 32)) & 1))
                want[h] = 1;
        }
    for (unsigned h = 0; h < CIC_NHOST; h++) {
        if (want[h] == c->out_level[h])
            continue;
        c->out_level[h] = want[h];
        int evt = cic_host_event(h);
        if (evt >= 0)
            intc_event(s, evt, want[h]);
    }
}

void cic_reset(soc_cic *c)
{
    *c = (soc_cic){ 0 };
    for (unsigned h = 0; h < CIC_NHOST; h++)
        c->hintmap[h] = h;
}

void cic_input(c6655_soc *s, unsigned sysint, int level)
{
    soc_cic *c = &s->cic0;
    if (sysint >= 7 * 32)
        return;
    int rising = level && !c->level[sysint];
    c->level[sysint] = level;
    uint32_t bit = 1u << (sysint % 32);
    if (!rising || (c->raw[sysint / 32] & bit))
        return;                              /* status latches on edges only */
    c->raw[sysint / 32] |= bit;
    /* McBSP XEVT/REVT toggle twice per audio word and are not enabled into
     * CIC0; rescanning all channels for them was most of the SoC's time. */
    if (c->enable[sysint / 32] & bit)
        cic_update(s);
}

/* Status latches on the input's rising edge and a clear always takes: the
 * program's CIC dispatcher (0x0080C1A8) clears status before the EDMA3 ISR
 * clears IPR (0x00803BA4), so level re-latching would hold host 5 high. */
static void cic_clear(soc_cic *c, unsigned sysint)
{
    if (sysint < 7 * 32)
        c->raw[sysint / 32] &= ~(1u << (sysint % 32));
}

uint32_t cic_read(c6655_soc *s, uint32_t off, unsigned size)
{
    soc_cic *c = &s->cic0;
    uint32_t a = off & ~3u, v = 0;
    if (a == 0x00)
        v = 0x4E82A900;                  /* REVISION; value unverified */
    else if (a == 0x04)
        v = c->control;
    else if (a == 0x0C)
        v = c->host_control;
    else if (a == 0x10)
        v = c->global_enable;
    else if (a >= 0x200 && a < 0x21C)
        v = c->raw[(a - 0x200) / 4];
    else if (a >= 0x280 && a < 0x29C)
        v = c->raw[(a - 0x280) / 4] & c->enable[(a - 0x280) / 4];
    else if ((a >= 0x300 && a < 0x31C) || (a >= 0x380 && a < 0x39C))
        v = c->enable[(a & 0x7F) / 4];
    else if (a >= 0x400 && a < 0x400 + CIC_NSYS) {
        unsigned i = a - 0x400;
        v = c->chmap[i] | c->chmap[i + 1] << 8 | c->chmap[i + 2] << 16 | (uint32_t)c->chmap[i + 3] << 24;
    } else if (a >= 0x800 && a < 0x800 + CIC_NHOST) {
        unsigned i = a - 0x800;
        v = c->hintmap[i] | c->hintmap[i + 1] << 8 | c->hintmap[i + 2] << 16 | (uint32_t)c->hintmap[i + 3] << 24;
    } else if (a >= 0x1500 && a < 0x150C)
        v = c->hint_enable[(a - 0x1500) / 4];
    else if (a >= 0x20 && a <= 0x38)
        v = 0;
    else
        soc_log_unimp(s, "CIC0", 0x02600000 + off, 0, 0);
    return reg_get(v, off, size);
}

void cic_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size)
{
    soc_cic *c = &s->cic0;
    uint32_t a = off & ~3u;
    uint32_t v = reg_put(0, off, val, size);
    if (a == 0x04)
        c->control = v;
    else if (a == 0x0C)
        c->host_control = v;
    else if (a == 0x10)
        c->global_enable = v;
    else if (a == 0x20) {
        if (v < 7 * 32) c->raw[v / 32] |= 1u << (v % 32);
    } else if (a == 0x24)
        cic_clear(c, v);
    else if (a == 0x28) {
        if (v < 7 * 32) c->enable[v / 32] |= 1u << (v % 32);
    } else if (a == 0x2C) {
        if (v < 7 * 32) c->enable[v / 32] &= ~(1u << (v % 32));
    } else if (a == 0x34) {
        if (v < CIC_NHOST) c->hint_enable[v / 32] |= 1u << (v % 32);
    } else if (a == 0x38) {
        if (v < CIC_NHOST) c->hint_enable[v / 32] &= ~(1u << (v % 32));
    } else if (a >= 0x200 && a < 0x21C)
        c->raw[(a - 0x200) / 4] |= v;
    else if (a >= 0x280 && a < 0x29C) {
        unsigned base = (a - 0x280) / 4 * 32;
        for (unsigned b = 0; b < 32; b++)
            if ((v >> b) & 1)
                cic_clear(c, base + b);
    } else if (a >= 0x300 && a < 0x31C)
        c->enable[(a - 0x300) / 4] |= v;
    else if (a >= 0x380 && a < 0x39C)
        c->enable[(a - 0x380) / 4] &= ~v;
    else if (a >= 0x400 && a < 0x400 + CIC_NSYS) {
        for (unsigned i = 0; i < size && off - 0x400 + i < CIC_NSYS; i++)
            c->chmap[off - 0x400 + i] = (val >> (8 * i)) & 0xFF;
    } else if (a >= 0x800 && a < 0x800 + CIC_NHOST) {
        for (unsigned i = 0; i < size && off - 0x800 + i < CIC_NHOST; i++) {
            uint8_t hm = (val >> (8 * i)) & 0xFF;
            c->hintmap[off - 0x800 + i] = hm;
            if (hm != off - 0x800 + i)
                soc_log(s, "CIC0: HINT_MAP channel %u -> host %u is not modelled",
                        off - 0x800 + i, hm);
        }
    } else if (a >= 0x1500 && a < 0x150C) {
        uint32_t *h = &c->hint_enable[(a - 0x1500) / 4];
        *h = reg_put(*h, off, val, size);
    } else
        soc_log_unimp(s, "CIC0", 0x02600000 + off, 1, val);
    cic_update(s);
}
