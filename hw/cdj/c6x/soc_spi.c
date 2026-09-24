/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPI (SPRUGP2A). The DSP is the master of MAIN's command link: SPIGCR1 = 3,
 * CS0 with SPIFMT0 0x0E020F10 (16-bit, prescale 15), CS1 a second slave with
 * its own format. Words move through EDMA3 channels 30 (SPIXEVT -> SPIDAT1) and
 * 31 (SPIBUF -> SPIREVT); SPIINT0 = DMAREQEN | TXINTENA | RXINTENA (app
 * 0x008021A8, 0x8007E580).
 *
 * A word is exchanged with the peer when it has been clocked out, CHARLEN bit
 * periods after SPIDAT1 is written. The module clock (1 GHz / 6) is inferred,
 * as for the timers.
 */
#include "soc_internal.h"

enum { GCR1_MASTER = 1, GCR1_CLKMOD = 2, GCR1_LOOPBACK = 1u << 16, GCR1_ENABLE = 1u << 24 };
enum { INT_RXINTENA = 1u << 8, INT_TXINTENA = 1u << 9, INT_OVRNINTENA = 1u << 6,
       INT_DMAREQEN = 1u << 16 };
enum { FLG_OVRN = 1u << 6, FLG_RXINT = 1u << 8, FLG_TXINT = 1u << 9 };
enum { BUF_RXEMPTY = 1u << 31, BUF_RXOVR = 1u << 30, BUF_TXFULL = 1u << 29 };
enum { DAT1_CSHOLD = 1u << 28, DAT1_WDEL = 1u << 26 };

static void spi_outputs(c6655_soc *s)
{
    soc_spi *p = &s->spi;
    uint32_t pend = p->flg & p->int0 & (FLG_OVRN | FLG_RXINT | FLG_TXINT);
    soc_signal(s, SIG_SPIINT0, (pend & ~p->lvl) != 0);
    soc_signal(s, SIG_SPIINT1, (pend & p->lvl) != 0);
    int dma = (p->int0 & INT_DMAREQEN) && (p->gcr1 & GCR1_ENABLE);
    soc_signal(s, SIG_SPIXEVT, dma && !p->txfull);
    soc_signal(s, SIG_SPIREVT, dma && !p->rxempty);
}

/* Highest-priority pending interrupt as INTVECTn: 0x13 overrun, 0x12 receive,
 * 0x14 transmit. */
static uint32_t intvec(const soc_spi *p, int line)
{
    uint32_t pend = p->flg & p->int0 & (line ? p->lvl : ~p->lvl);
    if (pend & FLG_OVRN) return 0x13 << 1;
    if (pend & FLG_RXINT) return 0x12 << 1;
    if (pend & FLG_TXINT) return 0x14 << 1;
    return 0;
}

static uint64_t word_ns(c6655_soc *s)
{
    soc_spi *p = &s->spi;
    uint32_t fmt = p->fmt[(p->dat1_ctl >> 24) & 3];
    unsigned bits = fmt & 0x1F, prescale = (fmt >> 8) & 0xFF;
    uint64_t module_hz = s->cfg.spi_clk_hz ? s->cfg.spi_clk_hz : NS_PER_S / 6;
    uint64_t ns = (uint64_t)bits * (prescale + 1) * NS_PER_S / module_hz;
    if (!p->cshold_prev) {                   /* chip-select setup/hold delays */
        unsigned c2t = p->delay >> 24, t2c = (p->delay >> 16) & 0xFF;
        ns += (uint64_t)(c2t + t2c + 3) * NS_PER_S / module_hz;
    }
    if (p->dat1_ctl & DAT1_WDEL)
        ns += (uint64_t)(((fmt >> 24) & 0x3F) + 2) * NS_PER_S / module_hz;
    return ns ? ns : 1;
}

void spi_reset(c6655_soc *s, soc_spi *p)
{
    *p = (soc_spi){ .rxempty = 1, .done_ns = UINT64_MAX };
    (void)s;
}

void spi_fire(c6655_soc *s)
{
    soc_spi *p = &s->spi;
    p->done_ns = UINT64_MAX;
    unsigned bits = p->fmt[(p->dat1_ctl >> 24) & 3] & 0x1F;
    unsigned csnr = (p->dat1_ctl >> 16) & 0xFF;
    int cshold = !!(p->dat1_ctl & DAT1_CSHOLD);
    uint16_t rx;
    if (p->gcr1 & GCR1_LOOPBACK)
        rx = p->txbuf;
    else if (s->cfg.spi_xfer)
        rx = s->cfg.spi_xfer(s->cfg.opaque, p->txbuf, bits, csnr, cshold);
    else
        rx = 0;
    if (!p->rxempty) {
        p->rxbuf |= BUF_RXOVR;
        p->flg |= FLG_OVRN;
    }
    p->rxbuf = (p->rxbuf & BUF_RXOVR) | rx;
    p->rxempty = 0;
    p->flg |= FLG_RXINT | FLG_TXINT;
    p->txfull = 0;
    p->cshold_prev = cshold;
    spi_outputs(s);
}

static void start_word(c6655_soc *s, uint16_t data)
{
    soc_spi *p = &s->spi;
    if (!(p->gcr0 & 1))
        return;                              /* module in reset */
    p->txbuf = data;
    p->txfull = 1;
    p->flg &= ~FLG_TXINT;
    if ((p->gcr1 & GCR1_ENABLE) && (p->gcr1 & GCR1_MASTER) && p->done_ns == UINT64_MAX)
        p->done_ns = s->now_ns + word_ns(s);
    else if (!(p->gcr1 & GCR1_MASTER))
        soc_log(s, "SPI: slave mode is not modelled");
    spi_outputs(s);
}

uint32_t spi_read(c6655_soc *s, uint32_t off, unsigned size)
{
    soc_spi *p = &s->spi;
    uint32_t v;
    switch (off & ~3u) {
    case 0x00: v = p->gcr0; break;
    case 0x04: v = p->gcr1; break;
    case 0x08: v = p->int0; break;
    case 0x0C: v = p->lvl; break;
    case 0x10: v = p->flg; break;
    case 0x14: v = p->pc0; break;
    case 0x38: v = p->txbuf; break;
    case 0x3C: v = p->dat1_ctl | p->txbuf; break;
    case 0x40: case 0x44:
        v = p->rxbuf | (p->rxempty ? BUF_RXEMPTY : 0) | (p->txfull ? BUF_TXFULL : 0);
        if ((off & ~3u) == 0x40) {           /* SPIBUF read consumes the word */
            p->rxempty = 1;
            p->rxbuf &= 0xFFFF;
            p->flg &= ~(FLG_RXINT | FLG_OVRN);
            spi_outputs(s);
        }
        break;
    case 0x48: v = p->delay; break;
    case 0x4C: v = p->def; break;
    case 0x50: case 0x54: case 0x58: case 0x5C: v = p->fmt[((off & ~3u) - 0x50) / 4]; break;
    case 0x60: v = intvec(p, 0); break;
    case 0x64: v = intvec(p, 1); break;
    default:
        soc_log_unimp(s, "SPI", 0x20BF0000 + off, 0, 0);
        v = 0;
    }
    return reg_get(v, off, size);
}

void spi_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size)
{
    soc_spi *p = &s->spi;
    uint32_t a = off & ~3u;
    switch (a) {
    case 0x00:
        p->gcr0 = reg_put(p->gcr0, off, val, size) & 1;
        if (!p->gcr0) {
            uint32_t fmt[4];
            for (int i = 0; i < 4; i++) fmt[i] = p->fmt[i];
            spi_reset(s, p);
            for (int i = 0; i < 4; i++) p->fmt[i] = fmt[i];
        }
        break;
    case 0x04: p->gcr1 = reg_put(p->gcr1, off, val, size); break;
    case 0x08: p->int0 = reg_put(p->int0, off, val, size); break;
    case 0x0C: p->lvl = reg_put(p->lvl, off, val, size); break;
    case 0x10: p->flg &= ~reg_put(0, off, val, size); break;
    case 0x14: p->pc0 = reg_put(p->pc0, off, val, size); break;
    case 0x38:
        start_word(s, (uint16_t)reg_put(p->txbuf, off, val, size));
        return;
    case 0x3C: {
        uint32_t v = reg_put(p->dat1_ctl | p->txbuf, off, val, size);
        p->dat1_ctl = v & 0xFFFF0000;
        /* A write reaching the data half starts a word; one confined to the
         * control half only updates the fields. */
        if ((off & 3) < 2)
            start_word(s, (uint16_t)v);
        else
            spi_outputs(s);
        return;
    }
    case 0x48: p->delay = reg_put(p->delay, off, val, size); break;
    case 0x4C: p->def = reg_put(p->def, off, val, size); break;
    case 0x50: case 0x54: case 0x58: case 0x5C: {
        uint32_t *f = &p->fmt[(a - 0x50) / 4];
        *f = reg_put(*f, off, val, size);
        break;
    }
    default:
        soc_log_unimp(s, "SPI", 0x20BF0000 + off, 1, val);
        return;
    }
    spi_outputs(s);
}
