/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * McBSP transmit (SPRU580G register layout; the KeyStone block keeps it).
 *
 * The program makes each port clock and frame master (app 0x00806CA8): XCR =
 * 2 words x 32 bits, SRGR FSGM with FPER 63, FWID 31, CLKGDV 7 or 3 from the
 * CLKS pin, PCR FSXM | CLKXM | FSXP | CLKXP. With the 22.5792 MHz oscillator
 * that is 44.1 kHz (CLKGDV 7) or 88.2 kHz (CLKGDV 3) stereo frames. DXR is fed
 * by EDMA3 channel 37 / 39 from ping-pong PaRAM sets (app 0x8007D61C). Receive
 * is configured by nothing and is not modelled.
 */
#include "soc_internal.h"

enum { SPCR_XRST = 1u << 16, SPCR_XRDY = 1u << 17, SPCR_XEMPTY = 1u << 18,
       SPCR_GRST = 1u << 22 };

static unsigned word_bits(uint32_t xcr)
{
    static const unsigned len[8] = { 8, 12, 16, 20, 24, 32, 32, 32 };
    return len[(xcr >> 5) & 7];
}

static unsigned words_per_frame(uint32_t xcr)
{
    unsigned n = ((xcr >> 8) & 0x7F) + 1;
    if (xcr & 0x80000000u)
        n += ((xcr >> 24) & 0x7F) + 1;
    return n;
}

static int clocking(const soc_mcbsp *m)
{
    return (m->spcr & SPCR_XRST) && (m->spcr & SPCR_GRST);
}

static void outputs(c6655_soc *s, soc_mcbsp *m)
{
    int xrdy = (m->spcr & SPCR_XRST) && !m->dxr_fresh;
    soc_signal(s, SIG_MCBSP_XEVT0 + 4 * m->port, xrdy);
    soc_signal(s, SIG_MCBSP_XINT0 + 4 * m->port, xrdy && ((m->spcr >> 20) & 3) == 0);
}

/* Word k of the current run is due at start + k * frame / words. Anchoring to
 * the start keeps the rate exact over hours of audio. */
static uint64_t word_due(const c6655_soc *s, const soc_mcbsp *m, uint64_t start, uint64_t k)
{
    unsigned clkgdv = m->srgr & 0xFF, fper = (m->srgr >> 16) & 0xFFF;
    unsigned __int128 num = (unsigned __int128)k * (clkgdv + 1) * (fper + 1) * NS_PER_S;
    unsigned __int128 den = (unsigned __int128)s->cfg.mcbsp_clks_hz * words_per_frame(m->xcr);
    return start + (uint64_t)((num + den - 1) / den);
}

static void clock_update(c6655_soc *s, soc_mcbsp *m, int was_clocking)
{
    int now = clocking(m);
    if (now && !was_clocking) {
        if (!(m->pcr & (1u << 9)) || !(m->pcr & (1u << 11)))
            soc_log(s, "McBSP%u: external CLKX/FSX; clocking from SRGR anyway", m->port);
        m->words = m->start_words = 0;
        m->start_ns = s->now_ns;
        m->next_word_ns = word_due(s, m, m->start_ns, 1);
    } else if (!now) {
        m->next_word_ns = UINT64_MAX;
    }
}

/* When the k-th word from now (k >= 1) leaves; UINT64_MAX when not clocking. */
uint64_t mcbsp_word_ns(const c6655_soc *s, const soc_mcbsp *m, uint64_t k)
{
    if (m->next_word_ns == UINT64_MAX || k == 0)
        return UINT64_MAX;
    if (k == 1)
        return m->next_word_ns;
    return word_due(s, m, m->start_ns, m->words - m->start_words + k);
}

void mcbsp_reset(c6655_soc *s, soc_mcbsp *m, unsigned port)
{
    *m = (soc_mcbsp){ .port = port, .next_word_ns = UINT64_MAX };
    (void)s;
}

void mcbsp_fire(c6655_soc *s, soc_mcbsp *m)
{
    uint32_t word = m->dxr_fresh ? m->dxr : 0;
    if (!m->dxr_fresh && m->underruns++ == 0)
        soc_log(s, "McBSP%u: DXR not refilled in time (underrun)", m->port);
    m->dxr_fresh = 0;
    if (s->cfg.mcbsp_tx)
        s->cfg.mcbsp_tx(s->cfg.opaque, m->port, word, word_bits(m->xcr));
    m->words++;
    uint64_t due = m->next_word_ns;
    m->next_word_ns = word_due(s, m, m->start_ns, m->words - m->start_words + 1);
    if (m->next_word_ns <= due)
        m->next_word_ns = due + 1;
    outputs(s, m);
}

uint32_t mcbsp_read(c6655_soc *s, soc_mcbsp *m, uint32_t off, unsigned size)
{
    uint32_t v;
    switch (off & ~3u) {
    case 0x00: v = m->drr; break;
    case 0x04: v = m->dxr; break;
    case 0x08:
        v = m->spcr & ~(SPCR_XRDY | SPCR_XEMPTY);
        if ((m->spcr & SPCR_XRST) && !m->dxr_fresh) v |= SPCR_XRDY;
        if ((m->spcr & SPCR_XRST) && m->words) v |= SPCR_XEMPTY;   /* 1 = not empty */
        break;
    case 0x0C: v = m->rcr; break;
    case 0x10: v = m->xcr; break;
    case 0x14: v = m->srgr; break;
    case 0x18: v = m->mcr; break;
    case 0x1C: v = m->rce[0]; break;
    case 0x20: v = m->xce[0]; break;
    case 0x24: v = m->pcr; break;
    default:
        soc_log_unimp(s, "McBSP", (m->port ? 0x021B8000 : 0x021B4000) + off, 0, 0);
        v = 0;
    }
    return reg_get(v, off, size);
}

void mcbsp_write(c6655_soc *s, soc_mcbsp *m, uint32_t off, uint32_t val, unsigned size)
{
    int was = clocking(m);
    switch (off & ~3u) {
    case 0x04:
        m->dxr = reg_put(m->dxr, off, val, size);
        m->dxr_fresh = 1;
        break;
    case 0x08:
        m->spcr = reg_put(m->spcr, off, val, size) & ~(SPCR_XRDY | SPCR_XEMPTY);
        if (!(m->spcr & SPCR_XRST))
            m->dxr_fresh = 0;
        break;
    case 0x0C: m->rcr = reg_put(m->rcr, off, val, size); break;
    case 0x10: m->xcr = reg_put(m->xcr, off, val, size); break;
    case 0x14: m->srgr = reg_put(m->srgr, off, val, size); break;
    case 0x18: m->mcr = reg_put(m->mcr, off, val, size); break;
    case 0x1C: m->rce[0] = reg_put(m->rce[0], off, val, size); break;
    case 0x20: m->xce[0] = reg_put(m->xce[0], off, val, size); break;
    case 0x24: m->pcr = reg_put(m->pcr, off, val, size); break;
    default:
        soc_log_unimp(s, "McBSP", (m->port ? 0x021B8000 : 0x021B4000) + off, 1, val);
        return;
    }
    clock_update(s, m, was);
    if (was && clocking(m) && ((off & ~3u) == 0x10 || (off & ~3u) == 0x14)) {
        /* A rate change takes effect from now, not retroactively. */
        m->start_ns = s->now_ns;
        m->start_words = m->words;
        m->next_word_ns = word_due(s, m, m->start_ns, 1);
    }
    outputs(s, m);
}
