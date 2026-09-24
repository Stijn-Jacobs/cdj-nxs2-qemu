/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * uPP (SPRUGJ5B register map), channel A receive into DMA channel I -- the only
 * direction the board uses (FPGA IC1201 -> DSP).
 *
 * How the program drives it:
 *   stage 1 0x008015E0  UPPCR SWRST pulse, UPCTL 0x02020000 (1 channel, RX),
 *                       UPICR 0x18, UPIES EOWI, UPPCR EN
 *   stage 1 0x00801738  UPID0 0x108C8800, UPID1 0x00014000 (1 x 16 KB), UPID2 0
 *   stage 1 0x008016C0  polls UPIER bit 3, writes 8 then 0x10 to it
 *   app     0x0080B560  UPID0 = buf, UPID1 = 0x10000 | len, UPID2 &= ~0xFFF8
 *   app     0x0080B5E4  ISR loop: UPIER bit 3 -> write 8; UPIS2 bit 1 polled
 *
 * Both writers finish a descriptor with UPID2, so that write arms the window
 * (the user guide only says to program UPID0-2; which write starts the DMA is
 * not stated -- unverified). Interrupt output is the level (UPISR & UPIES);
 * UPEOI is stored but does not gate it.
 */
#include <stdlib.h>
#include <string.h>

#include "soc_internal.h"

enum { PCR_EN = 1u << 3, PCR_SWRST = 1u << 4 };
enum { ISR_EOLI = 1u << 4, ISR_EOWI = 1u << 3, ISR_UORI = 1u << 1 };

static int rx_enabled(const soc_upp *u)
{
    unsigned mode = u->ctl & 3;
    return (u->pcr & PCR_EN) && !(u->pcr & PCR_SWRST) && (mode == 0 || mode == 2);
}

static void upp_irq(c6655_soc *s)
{
    soc_signal(s, SIG_UPPINT, (s->upp.isr & s->upp.ies) != 0);
}

static soc_upp_window window_from_regs(const soc_upp *u)
{
    return (soc_upp_window){
        .addr = u->id[0] & ~7u,
        .lines = u->id[1] >> 16,
        .bytes = u->id[1] & 0xFFFE,
        .lnoffset = u->id[2] & 0xFFF8,
    };
}

static void arm(c6655_soc *s)
{
    soc_upp *u = &s->upp;
    soc_upp_window w = window_from_regs(u);
    if (!w.lines || !w.bytes) {
        u->isr |= 1;                         /* DPEI: programming error */
        soc_log(s, "uPP: invalid descriptor lines=%u bytes=%u", w.lines, w.bytes);
        upp_irq(s);
        return;
    }
    if (!u->act) {
        u->cur = w;
        u->act = 1;
        u->line = u->byte = 0;
    } else if (!u->pend) {
        u->next = w;
        u->pend = 1;
    } else {
        soc_log(s, "uPP: descriptor written while one is already pending; dropped");
    }
}

void upp_drain(c6655_soc *s)
{
    soc_upp *u = &s->upp;
    while (u->qlen && u->act && rx_enabled(u)) {
        uint32_t base = u->cur.addr + u->line * u->cur.lnoffset + u->byte;
        size_t n = u->cur.bytes - u->byte;
        size_t run = u->qcap - u->qhead;     /* contiguous part of the ring */
        if (n > u->qlen) n = u->qlen;
        if (n > run) n = run;
        const uint8_t *p = u->q + u->qhead;
        size_t i = 0;
        for (; i + 4 <= n; i += 4)
            soc_dma_write(s, base + i, p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24, 4);
        for (; i < n; i++)
            soc_dma_write(s, base + i, p[i], 1);
        u->qhead = (u->qhead + n) % u->qcap;
        u->qlen -= n;
        u->byte += n;
        if (u->byte < u->cur.bytes)
            continue;
        u->isr |= ISR_EOLI;
        u->byte = 0;
        if (++u->line < u->cur.lines)
            continue;
        u->isr |= ISR_EOWI;
        u->act = 0;
        u->windows_done++;
        if (u->pend) {
            u->cur = u->next;
            u->pend = 0;
            u->act = 1;
            u->line = 0;
        }
    }
    upp_irq(s);
}

/*
 * A ship that runs past the window it completes loses its remainder (UORI);
 * bytes that arrive before any window is armed still wait, because at boot
 * MAIN ships as soon as READY drops. MAIN ships whole 16 KB DMA units: the
 * 0xFC00-byte audio window gets 0x10000 bytes and the 0x18-byte record window a
 * 0x40-byte ship. The excess must be dropped, not carried into the next window.
 */
static void upp_discard_overshoot(c6655_soc *s, size_t from_this_ship)
{
    soc_upp *u = &s->upp;
    size_t n = u->qlen < from_this_ship ? u->qlen : from_this_ship;
    if (!n || u->act)
        return;
    u->dropped += n;
    u->qlen -= n;                         /* the newest bytes sit at the ring's tail */
    u->isr |= ISR_UORI;
    upp_irq(s);
}

size_t c6655_soc_upp_dropped(const c6655_soc *s)
{
    return s->upp.dropped;
}

size_t c6655_soc_upp_rx(c6655_soc *s, const uint8_t *buf, size_t len)
{
    soc_upp *u = &s->upp;
    if (u->qlen + len > u->qcap) {
        size_t cap = u->qcap ? u->qcap : 1 << 16;
        while (cap < u->qlen + len)
            cap *= 2;
        uint8_t *q = malloc(cap);
        for (size_t i = 0; i < u->qlen; i++)
            q[i] = u->q[(u->qhead + i) % u->qcap];
        free(u->q);
        u->q = q;
        u->qcap = cap;
        u->qhead = 0;
    }
    for (size_t i = 0; i < len; i++)
        u->q[(u->qhead + u->qlen + i) % u->qcap] = buf[i];
    u->qlen += len;
    uint64_t done_before = u->windows_done;
    upp_drain(s);
    if (u->windows_done != done_before)
        upp_discard_overshoot(s, len);
    return u->qlen;
}

size_t c6655_soc_upp_queued(const c6655_soc *s)
{
    return s->upp.qlen;
}

void upp_reset(c6655_soc *s, soc_upp *u)
{
    /* The queue models the FPGA's side of the bus and survives a DSP reset. */
    uint8_t *q = u->q;
    size_t cap = u->qcap, head = u->qhead, len = u->qlen;
    *u = (soc_upp){ 0 };
    u->q = q; u->qcap = cap; u->qhead = head; u->qlen = len;
    (void)s;
}

void upp_free(soc_upp *u)
{
    free(u->q);
    u->q = NULL;
}

uint32_t upp_read(c6655_soc *s, uint32_t off, unsigned size)
{
    soc_upp *u = &s->upp;
    uint32_t v;
    switch (off & ~3u) {
    case 0x00: v = 0x44231100; break;    /* UPPID; value unverified */
    case 0x04: v = u->pcr | (u->act ? 0x80 : 0); break;   /* DB */
    case 0x08: v = u->dlb; break;
    case 0x10: v = u->ctl; break;
    case 0x14: v = u->icr; break;
    case 0x18: v = u->ivr; break;
    case 0x1C: v = u->tcr; break;
    case 0x20: v = u->isr; break;
    case 0x24: v = u->isr & u->ies; break;
    case 0x28: case 0x2C: v = u->ies; break;
    case 0x30: v = u->eoi; break;
    case 0x40: v = u->id[0]; break;
    case 0x44: v = u->id[1]; break;
    case 0x48: v = u->id[2]; break;
    case 0x50: v = u->act ? u->cur.addr + u->line * u->cur.lnoffset + u->byte : 0; break;
    case 0x54: v = u->act ? (u->line << 16 | u->byte) : 0; break;
    case 0x58: v = (u->pend ? 2 : 0) | (u->act ? 1 : 0); break;
    case 0x60: v = u->qd[0]; break;
    case 0x64: v = u->qd[1]; break;
    case 0x68: v = u->qd[2]; break;
    case 0x70: case 0x74: case 0x78: v = 0; break;
    default:
        soc_log_unimp(s, "uPP", 0x02580000 + off, 0, 0);
        v = 0;
    }
    return reg_get(v, off, size);
}

void upp_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size)
{
    soc_upp *u = &s->upp;
    uint32_t v = reg_put(0, off, val, size);
    switch (off & ~3u) {
    case 0x04:
        u->pcr = reg_put(u->pcr, off, val, size) & 0x1F;
        if (u->pcr & PCR_SWRST) {
            u->act = u->pend = 0;
            u->isr = 0;
        }
        break;
    case 0x08: u->dlb = reg_put(u->dlb, off, val, size); break;
    case 0x10: u->ctl = reg_put(u->ctl, off, val, size); break;
    case 0x14: u->icr = reg_put(u->icr, off, val, size); break;
    case 0x18: u->ivr = reg_put(u->ivr, off, val, size); break;
    case 0x1C: u->tcr = reg_put(u->tcr, off, val, size); break;
    case 0x20: u->isr |= v & 0x1F1F; break;             /* simulate events */
    case 0x24: u->isr &= ~v; break;
    case 0x28: u->ies |= v & 0x1F1F; break;
    case 0x2C: u->ies &= ~v; break;
    case 0x30: u->eoi = reg_put(u->eoi, off, val, size); break;
    case 0x40: u->id[0] = reg_put(u->id[0], off, val, size) & ~7u; break;
    case 0x44: u->id[1] = reg_put(u->id[1], off, val, size) & ~1u; break;
    case 0x48:
        u->id[2] = reg_put(u->id[2], off, val, size) & 0xFFF8;
        arm(s);
        break;
    case 0x60: case 0x64: case 0x68:
        u->qd[((off & ~3u) - 0x60) / 4] = reg_put(0, off, val, size);
        soc_log(s, "uPP: channel Q (transmit) is not modelled");
        break;
    default:
        soc_log_unimp(s, "uPP", 0x02580000 + off, 1, val);
        return;
    }
    upp_drain(s);
}
