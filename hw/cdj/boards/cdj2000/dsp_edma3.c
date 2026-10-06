/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_edma3.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
/*
 * The C674x's EDMA3, as far as the CDJ-2000NXS's DSP uses it: the two
 * McASP transmit channels, event 3 (McASP1) and event 5 (McASP2), each a
 * linked pair of A-synchronised parameter sets that moves one 16-frame half
 * of a planar ping-pong buffer to the serializer's data port, one sample per
 * event, and raises the completion flag of its TCC. The QDMA channels carry
 * the DSP's sector copies from L2 to SDRAM and back.
 *
 * The McASP events are paced here, on the DSP's own clock, one half buffer
 * per 16 frames at 44.1 kHz, so a core slower than the chip never has its
 * transmitters outrun it. The channel controller's register groups (the
 * global view at +0x1000 and the shadow regions 0x200 apart from +0x2000)
 * are all the same state: the region masks (DRAE) are not applied.
 */

#define REG_ER          0x00
#define REG_ECR         0x08
#define REG_ESR         0x10
#define REG_EER         0x20
#define REG_EECR        0x28
#define REG_EESR        0x30
#define REG_IER         0x50
#define REG_IECR        0x58
#define REG_IESR        0x60
#define REG_IPR         0x68
#define REG_ICR         0x70

#define GLOBAL_REGS     0x1000u
#define REGION_REGS     0x2000u
#define REGION_STRIDE   0x200u
#define QCHMAP_BASE     0x200u
#define QDMA_CHANNELS   8
#define REG_QEER        0x1084u
#define REG_QEECR       0x1088u
#define REG_QEESR       0x108Cu
#define PARAM_BASE      0x4000u
#define PARAM_SETS      512
#define CHANNELS        32

#define OPT_SYNCDIM     (1u << 2)
#define OPT_TCINTEN     (1u << 20)
#define OPT_TCC_SHIFT   12

#define AUDIO_FRAMES    16
#define NS_PER_HALF     (AUDIO_FRAMES * 1000000000ull / 44100)

static uint32_t *param_set(CdjEdma3 *e, unsigned set)
{
    return &e->w[(PARAM_BASE + set * 0x20) / 4];
}

/* The register a channel-controller address names, -1 outside the groups. */
static int group_reg(uint32_t off)
{
    if (off >= GLOBAL_REGS && off <= GLOBAL_REGS + REG_ICR) {
        return off - GLOBAL_REGS;
    }
    if (off >= REGION_REGS && off < REGION_REGS + 8 * REGION_STRIDE &&
        (off - REGION_REGS) % REGION_STRIDE <= REG_ICR) {
        return (off - REGION_REGS) % REGION_STRIDE;
    }
    return -1;
}

static uint32_t *reg_word(CdjEdma3 *e, unsigned reg)
{
    return &e->w[(GLOBAL_REGS + reg) / 4];
}

static void update_irq(CdjEdma3 *e)
{
    e->set_irq(e->chip, (*reg_word(e, REG_IPR) & *reg_word(e, REG_IER)) != 0);
}

static void move_array(CdjEdma3 *e, uint32_t src, uint32_t dst, uint32_t len)
{
    uint8_t *from = e->ram(e->chip, src);
    uint8_t *to = e->ram(e->chip, dst);

    if (to) {
        if (from) {
            memmove(to, from, len);
        } else {
            memset(to, 0, len);
        }
        e->stored(e->chip, dst, len);
    } else if (len == 4) {
        e->write_word(e->chip, dst, from ? ldl_le_p(from) : 0);
    }
}

static void channel_event(CdjEdma3 *e, unsigned ch)
{
    uint32_t *p = param_set(e, ch);
    uint32_t opt = p[0], src = p[1], dst = p[3];
    uint32_t acnt = p[2] & 0xFFFF, bcnt = p[2] >> 16;
    int16_t src_bidx = p[4], dst_bidx = p[4] >> 16;
    uint32_t link = p[5] & 0xFFFF, bcnt_reload = p[5] >> 16;
    int16_t src_cidx = p[6], dst_cidx = p[6] >> 16;
    uint32_t ccnt = p[7] & 0xFFFF;

    if (!acnt || !bcnt || !ccnt) {
        return;
    }
    move_array(e, src, dst, acnt);
    e->events++;
    if (--bcnt) {
        src += src_bidx;
        dst += dst_bidx;
    } else {
        /* After a frame's last array the frame index counts from that array. */
        src += src_cidx;
        dst += dst_cidx;
        bcnt = bcnt_reload;
        ccnt--;
    }
    p[1] = src;
    p[3] = dst;
    p[2] = (bcnt << 16) | acnt;
    p[7] = ccnt;
    if (ccnt) {
        return;
    }
    e->completions++;
    if (opt & OPT_TCINTEN) {
        *reg_word(e, REG_IPR) |= 1u << ((opt >> OPT_TCC_SHIFT) & 0x1F);
        update_irq(e);
    }
    if (link >= PARAM_BASE && link < PARAM_BASE + PARAM_SETS * 0x20) {
        memcpy(p, param_set(e, (link - PARAM_BASE) / 0x20), 0x20);
    }
}

static void qdma_trigger(CdjEdma3 *e, unsigned set, int word);

static void qdma_run(CdjEdma3 *e, unsigned set)
{
    uint32_t *p = param_set(e, set);
    uint32_t opt = p[0], src = p[1], dst = p[3];
    uint32_t acnt = p[2] & 0xFFFF, bcnt = p[2] >> 16;
    int16_t src_bidx = p[4], dst_bidx = p[4] >> 16;
    uint32_t link = p[5] & 0xFFFF;
    int16_t src_cidx = p[6], dst_cidx = p[6] >> 16;
    uint32_t ccnt = p[7] & 0xFFFF, b, c;

    for (c = 0; c < ccnt; c++) {
        uint32_t frame_src = src, frame_dst = dst;

        for (b = 0; b < bcnt; b++) {
            move_array(e, src, dst, acnt);
            src += src_bidx;
            dst += dst_bidx;
        }
        if (opt & OPT_SYNCDIM) {
            src = frame_src + src_cidx;
            dst = frame_dst + dst_cidx;
        } else {
            src += src_cidx - src_bidx;
            dst += dst_cidx - dst_bidx;
        }
    }
    e->completions++;
    if (opt & OPT_TCINTEN) {
        *reg_word(e, REG_IPR) |= 1u << ((opt >> OPT_TCC_SHIFT) & 0x1F);
        update_irq(e);
    }
    if (link >= PARAM_BASE && link < PARAM_BASE + PARAM_SETS * 0x20) {
        memcpy(p, param_set(e, (link - PARAM_BASE) / 0x20), 0x20);
        qdma_trigger(e, set, -1);
    }
}

/* A QDMA channel fires when the DSP writes the trigger word of the parameter
 * set its QCHMAP entry names, and again when a link reload rewrites it. The
 * set is run to completion at once: the firmware's copies are single-trigger. */
static void qdma_trigger(CdjEdma3 *e, unsigned set, int word)
{
    unsigned ch;

    for (ch = 0; ch < QDMA_CHANNELS; ch++) {
        uint32_t map = e->w[(QCHMAP_BASE + ch * 4) / 4];

        if (e->w[REG_QEER / 4] & (1u << ch) &&
            ((map >> 5) & 0x1FF) == set &&
            (word < 0 || ((map >> 2) & 7) == word)) {
            qdma_run(e, set);
            return;
        }
    }
}

uint32_t cdj_edma3_read(const CdjEdma3 *e, uint32_t addr)
{
    uint32_t off = (addr - EDMA3_BASE) & ~3u;
    int reg = group_reg(off);

    if (reg >= 0) {
        return e->w[(GLOBAL_REGS + reg) / 4];
    }
    return e->w[off / 4];
}

void cdj_edma3_write(CdjEdma3 *e, uint32_t addr, uint32_t val)
{
    uint32_t off = (addr - EDMA3_BASE) & ~3u;
    int reg = group_reg(off);
    unsigned ch;

    if (reg < 0) {
        e->w[off / 4] = val;
        if (off >= PARAM_BASE && off < PARAM_BASE + PARAM_SETS * 0x20) {
            qdma_trigger(e, (off - PARAM_BASE) / 0x20, (off % 0x20) / 4);
        } else if (off == REG_QEESR) {
            e->w[REG_QEER / 4] |= val;
        } else if (off == REG_QEECR) {
            e->w[REG_QEER / 4] &= ~val;
        }
        return;
    }
    switch (reg) {
    case REG_ECR:
        *reg_word(e, REG_ER) &= ~val;
        break;
    case REG_ESR:
        for (ch = 0; ch < CHANNELS; ch++) {
            if (val & (1u << ch)) {
                channel_event(e, ch);
            }
        }
        break;
    case REG_EECR:
        *reg_word(e, REG_EER) &= ~val;
        break;
    case REG_EESR:
        *reg_word(e, REG_EER) |= val;
        break;
    case REG_IECR:
        *reg_word(e, REG_IER) &= ~val;
        update_irq(e);
        break;
    case REG_IESR:
        *reg_word(e, REG_IER) |= val;
        update_irq(e);
        break;
    case REG_ICR:
        *reg_word(e, REG_IPR) &= ~val;
        update_irq(e);
        break;
    }
}

void cdj_edma3_paced(CdjEdma3 *e, int64_t dsp_ns)
{
    uint32_t enabled = *reg_word(e, REG_EER);
    unsigned ch, n;

    if (!e->deadline_ns) {
        e->deadline_ns = dsp_ns + NS_PER_HALF;
    }
    if (dsp_ns < e->deadline_ns) {
        return;
    }
    e->deadline_ns = dsp_ns + NS_PER_HALF;
    for (ch = 0; ch < CHANNELS; ch++) {
        if (enabled & (1u << ch)) {
            for (n = 0; n < 2 * AUDIO_FRAMES; n++) {
                channel_event(e, ch);
            }
        }
    }
}
